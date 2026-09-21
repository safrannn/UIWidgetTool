#include "UIWidgetPreviewObjectManagerSettings.h"

#include "Misc/Paths.h"
#include "UIWidgetToolPlugin.h"

FGuid UUIWidgetPreviewObjectManagerSettings::AddWidgetPreviewObject(
    const TSoftClassPtr<UUserWidget> &InClass, const FString &InWidgetName)
{
  FWidgetPreviewObject WidgetPreviewObject;
  WidgetPreviewObject.Id = FGuid::NewGuid();
  WidgetPreviewObject.WidgetClass = InClass;
  WidgetPreviewObject.WidgetName = InWidgetName;

  const int32 NewIndex = WidgetPreviewObjects.Add(WidgetPreviewObject);
  IdToIndex.Add(WidgetPreviewObject.Id, NewIndex);

  SaveWidgetPreviewObjects();
  return WidgetPreviewObject.Id;
}

void UUIWidgetPreviewObjectManagerSettings::RemoveWidgetPreviewObject(
    const FGuid &Id)
{
  const int32 *IndexPtr = IdToIndex.Find(Id);
  if (!IndexPtr)
  {
    return;
  }

  const int32 IndexToRemove = *IndexPtr;
  const int32 LastIndex = WidgetPreviewObjects.Num() - 1;

  if (IndexToRemove != LastIndex)
  {
    WidgetPreviewObjects.Swap(IndexToRemove, LastIndex);
    IdToIndex.Add(WidgetPreviewObjects[IndexToRemove].Id, IndexToRemove);
  }

  WidgetPreviewObjects.RemoveAt(LastIndex);
  IdToIndex.Remove(Id);

  SaveWidgetPreviewObjects();
}

FGuid UUIWidgetPreviewObjectManagerSettings::DuplicateWidgetPreviewObject(
    const FGuid &SourceId)
{
  const int32 *IndexPtr = IdToIndex.Find(SourceId);
  if (!IndexPtr)
  {
    return FGuid();
  }

  FWidgetPreviewObject Copy = WidgetPreviewObjects[*IndexPtr];
  Copy.Id = FGuid::NewGuid();

  // Insert directly after the source so the copy sits beside it in the table.
  WidgetPreviewObjects.Insert(Copy, *IndexPtr + 1);
  RebuildIndex();

  SaveWidgetPreviewObjects();
  return Copy.Id;
}

FWidgetPreviewObject *
UUIWidgetPreviewObjectManagerSettings::FindWidgetPreviewObject(
    const FGuid &Id)
{
  if (const int32 *IndexPtr = IdToIndex.Find(Id))
  {
    return &WidgetPreviewObjects[*IndexPtr];
  }
  return nullptr;
}

void UUIWidgetPreviewObjectManagerSettings::SaveWidgetPreviewObjects()
{
  SaveConfig();
  TryUpdateDefaultConfigFile();
}

void UUIWidgetPreviewObjectManagerSettings::PostInitProperties()
{
  Super::PostInitProperties();
  RebuildIndex();
}

#if WITH_EDITOR
void UUIWidgetPreviewObjectManagerSettings::PostEditChangeProperty(
    FPropertyChangedEvent &PropertyChangedEvent)
{
  Super::PostEditChangeProperty(PropertyChangedEvent);

  RebuildIndex();

  SaveWidgetPreviewObjects();
}
#endif

void UUIWidgetPreviewObjectManagerSettings::RebuildIndex()
{
  IdToIndex.Reset();
  IdToIndex.Reserve(WidgetPreviewObjects.Num());
  for (int32 Index = 0; Index < WidgetPreviewObjects.Num(); ++Index)
  {
    FWidgetPreviewObject &WidgetPreviewObject = WidgetPreviewObjects[Index];

    if (!WidgetPreviewObject.Id.IsValid() ||
        IdToIndex.Contains(WidgetPreviewObject.Id))
    {
      WidgetPreviewObject.Id = FGuid::NewGuid();
    }

    IdToIndex.Add(WidgetPreviewObject.Id, Index);
  }
}

UUIWidgetPreviewObjectManagerSettings::UUIWidgetPreviewObjectManagerSettings()
{
  ExcludedActorProperties = {
      TEXT("*:Owner"),
      TEXT("*:Instigator"),
      TEXT("*:RootComponent"),
      TEXT("*:ParentComponent"),
      TEXT("*:Children"),
      TEXT("*:InstanceComponents"),
      TEXT("*:BlueprintCreatedComponents"),
      TEXT("*:ReplicatedMovement"),
      TEXT("*:AttachmentReplication"),
      TEXT("*:Layers"),
      TEXT("*:ParentComponentActor"),

      TEXT("Controller:Pawn"),
      TEXT("Controller:PlayerState"),
      TEXT("PlayerController:Player"),
      TEXT("PlayerController:NetConnection"),
      TEXT("PlayerController:PlayerCameraManager"),
      TEXT("PlayerController:MyHUD"),
      TEXT("PlayerController:SpectatorPawn"),
      TEXT("Pawn:Controller"),
      TEXT("Pawn:PlayerState"),
      TEXT("Pawn:LastHitBy"),

      TEXT("GameModeBase:GameSession"),
      TEXT("GameModeBase:GameState"),
      TEXT("GameStateBase:AuthorityGameMode"),
      TEXT("GameStateBase:PlayerArray"),
      TEXT("GameState:PlayerArray"),
      TEXT("WorldSettings:AssetUserData"),
  };

  ExcludedComponentClasses = {
      TEXT("/Script/Engine.SkeletalMeshComponent"),
      TEXT("/Script/Engine.InstancedStaticMeshComponent"),
      TEXT("/Script/Engine.HierarchicalInstancedStaticMeshComponent"),
      TEXT("/Script/Engine.ParticleSystemComponent"),
      TEXT("/Script/Niagara.NiagaraComponent"),
      TEXT("/Script/Engine.AudioComponent"),
      TEXT("/Script/Landscape.LandscapeComponent"),
      TEXT("/Script/Engine.BrushComponent"),
  };
}

FString
UUIWidgetPreviewObjectManagerSettings::GetResolvedCheckpointDirectory() const
{
  FString Directory = CheckpointDirectory.Path;
  if (Directory.IsEmpty())
  {
    Directory = FPaths::Combine(UIWT::GetSavePath(), TEXT("Checkpoints"));
  }
  if (FPaths::IsRelative(Directory))
  {
    Directory = FPaths::Combine(FPaths::ProjectDir(), Directory);
  }
  FPaths::NormalizeDirectoryName(Directory);
  return FPaths::ConvertRelativePathToFull(Directory);
}

FUIWTCheckpointLoadOptions
UUIWidgetPreviewObjectManagerSettings::MakeLoadOptions() const
{
  FUIWTCheckpointLoadOptions Options;
  Options.MaxCompressedBytes =
      static_cast<int64>(FMath::Max(1, MaxCompressedPayloadMB)) * 1024 * 1024;
  Options.MaxUncompressedBytes =
      static_cast<int64>(FMath::Max(1, MaxDecompressedPayloadMB)) * 1024 * 1024;
  return Options;
}

TSet<FGuid>
UUIWidgetPreviewObjectManagerSettings::GetAssignedCheckpointIds() const
{
  TSet<FGuid> Assigned;
  for (const FWidgetPreviewObject &Entry : WidgetPreviewObjects)
  {
    if (Entry.CheckpointId.IsValid())
    {
      Assigned.Add(Entry.CheckpointId);
    }
  }
  return Assigned;
}

bool UUIWidgetPreviewObjectManagerSettings::IsActorPropertyExcluded(
    const UClass *InOwnerClass, FName InPropertyName) const
{
  const FString PropertyName = InPropertyName.ToString();

  for (const FString &Entry : ExcludedActorProperties)
  {
    FString ClassPart, PropertyPart;
    if (!Entry.Split(TEXT(":"), &ClassPart, &PropertyPart) ||
        !PropertyPart.Equals(PropertyName, ESearchCase::IgnoreCase))
    {
      continue;
    }
    if (ClassPart == TEXT("*"))
    {
      return true;
    }
    for (const UClass *Class = InOwnerClass; Class;
         Class = Class->GetSuperClass())
    {
      if (Class->GetName().Equals(ClassPart, ESearchCase::IgnoreCase))
      {
        return true;
      }
    }
  }
  return false;
}

bool UUIWidgetPreviewObjectManagerSettings::IsComponentClassExcluded(
    const FString &InClassPath) const
{
  for (const FString &Entry : ExcludedComponentClasses)
  {
    if (Entry.Equals(InClassPath, ESearchCase::IgnoreCase))
    {
      return true;
    }
  }
  return false;
}
