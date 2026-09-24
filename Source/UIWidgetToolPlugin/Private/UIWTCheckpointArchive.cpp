#include "UIWTCheckpointArchive.h"

#include "Components/ActorComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "UObject/UnrealType.h"

namespace
{
  FString StripPie(const FString &InPath)
  {
    return UWorld::RemovePIEPrefix(InPath);
  }

  void SerializeNameAsString(FArchive &Ar, FName &InOutName)
  {
    if (Ar.IsLoading())
    {
      FString AsString;
      Ar << AsString;
      InOutName = FName(*AsString);
    }
    else
    {
      FString AsString = InOutName.ToString();
      Ar << AsString;
    }
  }

  void ConfigureArchive(FArchive &Ar)
  {
    Ar.ArNoDelta = true;
    Ar.ArIsSaveGame = false;
    Ar.ArIgnoreClassRef = true;
    Ar.ArIgnoreArchetypeRef = true;
  }

}

namespace UIWTCheckpointArchive
{

  bool ShouldSkipProperty(const FProperty *InProperty,
                          const UClass *InOwnerClass)
  {
    if (!InProperty)
    {
      return true;
    }

    static constexpr uint64 ExcludedFlags =
        CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient |
        CPF_Deprecated | CPF_EditorOnly;
    if (InProperty->HasAnyPropertyFlags(ExcludedFlags))
    {
      return true;
    }

    const UUIWidgetPreviewObjectManagerSettings *Settings =
        GetDefault<UUIWidgetPreviewObjectManagerSettings>();
    return Settings && Settings->IsActorPropertyExcluded(
                           InOwnerClass, InProperty->GetFName());
  }

  FGuid GetActorInstanceGuid(const AActor *InActor)
  {
#if WITH_EDITOR
    return InActor->GetActorGuid();
#else
    return FGuid();
#endif
  }

  void BuildComponentKeys(const AActor *InActor,
                          TMap<FName, UActorComponent *> &OutComponents)
  {
    int32 Order = 0;
    for (UActorComponent *Component : InActor->GetComponents())
    {
      if (!IsValid(Component))
      {
        continue;
      }
      FName Key = Component->GetFName();
      if (OutComponents.Contains(Key))
      {
        Key = FName(*FString::Printf(TEXT("%s_%d"), *Key.ToString(), Order));
      }
      OutComponents.Add(Key, Component);
      ++Order;
    }
  }

  bool WriteObjectProperties(UObject *InObject,
                             const FUIWTCaptureRefContext &InContext,
                             TArray<uint8> &OutBytes)
  {
    if (!IsValid(InObject))
    {
      return false;
    }

    FUIWTCheckpointWriter Writer(OutBytes, InContext);
    InObject->Serialize(Writer);
    return !Writer.IsError();
  }

  bool ReadObjectProperties(UObject *InObject, const TArray<uint8> &InBytes,
                            const FUIWTRestoreRefContext &InContext,
                            FUIWTRestoreReport &InOutReport)
  {
    if (!IsValid(InObject) || InBytes.Num() == 0)
    {
      return false;
    }

    TArray<uint8> &MutableBytes = const_cast<TArray<uint8> &>(InBytes);
    FUIWTCheckpointReader Reader(MutableBytes, InContext, InOutReport);
    InObject->Serialize(Reader);

    if (Reader.IsError())
    {
      InOutReport.AddMessage(FString::Printf(
          TEXT("Property stream for '%s' did not apply cleanly."),
          *InObject->GetName()));
      return false;
    }
    return true;
  }

}

FUIWTCheckpointWriter::FUIWTCheckpointWriter(
    TArray<uint8> &InBytes, const FUIWTCaptureRefContext &InContext)
    : FObjectWriter(InBytes), Context(InContext)
{
  ConfigureArchive(*this);
}

bool FUIWTCheckpointWriter::ShouldSkipProperty(
    const FProperty *InProperty) const
{
  if (FObjectWriter::ShouldSkipProperty(InProperty))
  {
    return true;
  }
  return UIWTCheckpointArchive::ShouldSkipProperty(
      InProperty, InProperty ? InProperty->GetOwnerClass() : nullptr);
}

void FUIWTCheckpointWriter::WriteToken(EUIWTRefToken InToken)
{
  uint8 Token = static_cast<uint8>(InToken);
  *this << Token;
}

void FUIWTCheckpointWriter::WriteReference(UObject *InObject)
{
  if (!InObject)
  {
    WriteToken(EUIWTRefToken::Null);
    return;
  }

  if (const AActor *Actor = Cast<AActor>(InObject))
  {
    if (const FGuid *RecordId = Context.ActorToRecordId.Find(Actor))
    {
      WriteToken(EUIWTRefToken::ActorRecord);
      FGuid Value = *RecordId;
      *this << Value;
      return;
    }
  }
  else if (const UActorComponent *Component =
               Cast<UActorComponent>(InObject))
  {
    if (const FUIWTComponentToken *ComponentToken =
            Context.ComponentToToken.Find(Component))
    {
      WriteToken(EUIWTRefToken::ComponentOfRecord);
      FGuid Owner = ComponentToken->OwnerRecordId;
      FName Key = ComponentToken->ComponentKey;
      *this << Owner;
      *this << Key;
      return;
    }
  }

  // Anything living in the world but not captured cannot be named across
  // sessions; everything else is an asset that a soft path will find again.
  const bool bIsWorldObject = InObject->IsA<AActor>() ||
                              InObject->IsA<UActorComponent>() ||
                              InObject->IsA<UWorld>();
  WriteToken(bIsWorldObject ? EUIWTRefToken::Unresolvable
                            : EUIWTRefToken::AssetPath);
  FString Path = StripPie(InObject->GetPathName());
  *this << Path;
}

FArchive &FUIWTCheckpointWriter::operator<<(FName &Value)
{
  SerializeNameAsString(*this, Value);
  return *this;
}

FArchive &FUIWTCheckpointWriter::operator<<(UObject *&Value)
{
  WriteReference(Value);
  return *this;
}

FArchive &FUIWTCheckpointWriter::operator<<(FObjectPtr &Value)
{
  WriteReference(Value.Get());
  return *this;
}

FArchive &FUIWTCheckpointWriter::operator<<(FWeakObjectPtr &Value)
{
  WriteReference(Value.Get());
  return *this;
}

FArchive &FUIWTCheckpointWriter::operator<<(FSoftObjectPtr &Value)
{
  FSoftObjectPath Path = Value.ToSoftObjectPath();
  return *this << Path;
}

FArchive &FUIWTCheckpointWriter::operator<<(FSoftObjectPath &Value)
{
  if (Value.IsNull())
  {
    WriteToken(EUIWTRefToken::Null);
    return *this;
  }
  WriteToken(EUIWTRefToken::AssetPath);
  FString Path = StripPie(Value.ToString());
  *this << Path;
  return *this;
}

FUIWTCheckpointReader::FUIWTCheckpointReader(
    TArray<uint8> &InBytes, const FUIWTRestoreRefContext &InContext,
    FUIWTRestoreReport &InReport)
    : FObjectReader(InBytes), Context(InContext), Report(InReport)
{
  ConfigureArchive(*this);
}

bool FUIWTCheckpointReader::ShouldSkipProperty(
    const FProperty *InProperty) const
{
  if (FObjectReader::ShouldSkipProperty(InProperty))
  {
    return true;
  }
  return UIWTCheckpointArchive::ShouldSkipProperty(
      InProperty, InProperty ? InProperty->GetOwnerClass() : nullptr);
}

bool FUIWTCheckpointReader::ReadWorldReference(EUIWTRefToken InToken,
                                               UObject *&OutObject)
{
  OutObject = nullptr;

  if (InToken == EUIWTRefToken::ActorRecord)
  {
    FGuid RecordId;
    *this << RecordId;
    if (AActor *const *Found = Context.RecordIdToActor.Find(RecordId))
    {
      OutObject = *Found;
      return true;
    }
    ++Report.SkippedReferences;
    Report.AddMessage(FString::Printf(
        TEXT("Actor reference %s did not resolve; kept the fresh-world value."),
        *RecordId.ToString(EGuidFormats::Digits)));
    return false;
  }

  FGuid OwnerRecordId;
  FName ComponentKey;
  *this << OwnerRecordId;
  *this << ComponentKey;
  if (const TMap<FName, UActorComponent *> *Components =
          Context.RecordIdToComponents.Find(OwnerRecordId))
  {
    if (UActorComponent *const *Found = Components->Find(ComponentKey))
    {
      OutObject = *Found;
      return true;
    }
  }
  ++Report.SkippedReferences;
  Report.AddMessage(FString::Printf(
      TEXT("Component reference %s.%s did not resolve; kept the fresh-world "
           "value."),
      *OwnerRecordId.ToString(EGuidFormats::Digits),
      *ComponentKey.ToString()));
  return false;
}

// A reference that does not resolve leaves InOutValue at its fresh-world value.
void FUIWTCheckpointReader::ReadReference(UObject *&InOutValue)
{
  uint8 RawToken = 0;
  *this << RawToken;
  if (IsError())
  {
    return;
  }

  switch (static_cast<EUIWTRefToken>(RawToken))
  {
  case EUIWTRefToken::Null:
    InOutValue = nullptr;
    return;

  case EUIWTRefToken::ActorRecord:
  case EUIWTRefToken::ComponentOfRecord:
  {
    UObject *Resolved = nullptr;
    if (ReadWorldReference(static_cast<EUIWTRefToken>(RawToken), Resolved))
    {
      InOutValue = Resolved;
    }
    return;
  }

  case EUIWTRefToken::AssetPath:
  {
    FString Path;
    *this << Path;
    const FSoftObjectPath SoftPath(Path);
    UObject *Resolved = SoftPath.ResolveObject();
    if (!Resolved)
    {
      Resolved = SoftPath.TryLoad();
    }
    if (Resolved)
    {
      InOutValue = Resolved;
      return;
    }
    ++Report.SkippedReferences;
    Report.AddMessage(FString::Printf(
        TEXT("Asset '%s' did not resolve; kept the fresh-world value."),
        *Path));
    return;
  }

  case EUIWTRefToken::Unresolvable:
  {
    FString Path;
    *this << Path;
    ++Report.SkippedReferences;
    Report.AddMessage(FString::Printf(
        TEXT("Reference to '%s' was unnameable at capture time; kept the "
             "fresh-world value."),
        *Path));
    return;
  }
  }

  SetError();
}

FArchive &FUIWTCheckpointReader::operator<<(FName &Value)
{
  SerializeNameAsString(*this, Value);
  return *this;
}

FArchive &FUIWTCheckpointReader::operator<<(UObject *&Value)
{
  ReadReference(Value);
  return *this;
}

FArchive &FUIWTCheckpointReader::operator<<(FObjectPtr &Value)
{
  UObject *Resolved = Value.Get();
  ReadReference(Resolved);
  Value = FObjectPtr(Resolved);
  return *this;
}

FArchive &FUIWTCheckpointReader::operator<<(FWeakObjectPtr &Value)
{
  UObject *Resolved = Value.Get();
  ReadReference(Resolved);
  Value = Resolved;
  return *this;
}

FArchive &FUIWTCheckpointReader::operator<<(FSoftObjectPtr &Value)
{
  FSoftObjectPath Path = Value.ToSoftObjectPath();
  *this << Path;
  Value = Path;
  return *this;
}

FArchive &FUIWTCheckpointReader::operator<<(FSoftObjectPath &Value)
{
  uint8 RawToken = 0;
  *this << RawToken;
  if (IsError())
  {
    return *this;
  }

  switch (static_cast<EUIWTRefToken>(RawToken))
  {
  case EUIWTRefToken::Null:
    Value.Reset();
    return *this;

  case EUIWTRefToken::AssetPath:
  {
    FString Path;
    *this << Path;
    Value = FSoftObjectPath(Path);
    return *this;
  }

  case EUIWTRefToken::Unresolvable:
  {
    FString Path;
    *this << Path;
    ++Report.SkippedReferences;
    return *this;
  }

  case EUIWTRefToken::ActorRecord:
  case EUIWTRefToken::ComponentOfRecord:
  {
    UObject *Resolved = nullptr;
    if (ReadWorldReference(static_cast<EUIWTRefToken>(RawToken), Resolved))
    {
      Value = FSoftObjectPath(Resolved);
    }
    return *this;
  }
  }

  SetError();
  return *this;
}
