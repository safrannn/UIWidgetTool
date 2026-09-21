#pragma once

#include "CoreMinimal.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"
#include "UIWTRestoreReport.h"

class AActor;
class UActorComponent;
class UWorld;

enum class EUIWTRefToken : uint8 {
  Null = 0,
  ActorRecord = 1,
  ComponentOfRecord = 2,
  AssetPath = 3,
  Unresolvable = 4,
};

struct FUIWTComponentToken {
  FGuid OwnerRecordId;
  FName ComponentKey;
};

struct UIWIDGETTOOLPLUGIN_API FUIWTCaptureRefContext {
  TMap<const AActor *, FGuid> ActorToRecordId;
  TMap<const UActorComponent *, FUIWTComponentToken> ComponentToToken;
};

struct UIWIDGETTOOLPLUGIN_API FUIWTRestoreRefContext {
  TMap<FGuid, AActor *> RecordIdToActor;
  TMap<FGuid, TMap<FName, UActorComponent *>> RecordIdToComponents;
};

namespace UIWTCheckpointArchive {
UIWIDGETTOOLPLUGIN_API bool ShouldSkipProperty(const FProperty *InProperty,
                                               const UClass *InOwnerClass);

// The editor's stable per-instance actor GUID. Invalid for actors spawned at
// runtime and outside the editor, which is how records tell level actors
// (matched by GUID or path) from runtime ones (respawned by class).
UIWIDGETTOOLPLUGIN_API FGuid GetActorInstanceGuid(const AActor *InActor);

// Keys an actor's components by name, suffixing duplicates with their
// iteration order. Capture and restore must agree on this, so both call it.
UIWIDGETTOOLPLUGIN_API void
BuildComponentKeys(const AActor *InActor,
                   TMap<FName, UActorComponent *> &OutComponents);

UIWIDGETTOOLPLUGIN_API bool
WriteObjectProperties(UObject *InObject, const FUIWTCaptureRefContext &InContext,
                      TArray<uint8> &OutBytes);

UIWIDGETTOOLPLUGIN_API bool
ReadObjectProperties(UObject *InObject, const TArray<uint8> &InBytes,
                     const FUIWTRestoreRefContext &InContext,
                     FUIWTRestoreReport &InOutReport);

}

class UIWIDGETTOOLPLUGIN_API FUIWTCheckpointWriter : public FObjectWriter {
public:
  FUIWTCheckpointWriter(TArray<uint8> &InBytes,
                        const FUIWTCaptureRefContext &InContext);

  using FObjectWriter::operator<<;

  virtual FArchive &operator<<(FName &Value) override;
  virtual FArchive &operator<<(UObject *&Value) override;
  virtual FArchive &operator<<(FObjectPtr &Value) override;
  virtual FArchive &operator<<(FWeakObjectPtr &Value) override;
  virtual FArchive &operator<<(FSoftObjectPtr &Value) override;
  virtual FArchive &operator<<(FSoftObjectPath &Value) override;

  virtual bool ShouldSkipProperty(const FProperty *InProperty) const override;

  virtual FString GetArchiveName() const override {
    return TEXT("FUIWTCheckpointWriter");
  }

private:
  void WriteToken(EUIWTRefToken InToken);
  void WriteReference(UObject *InObject);

  const FUIWTCaptureRefContext &Context;
};

class UIWIDGETTOOLPLUGIN_API FUIWTCheckpointReader : public FObjectReader {
public:
  FUIWTCheckpointReader(TArray<uint8> &InBytes,
                        const FUIWTRestoreRefContext &InContext,
                        FUIWTRestoreReport &InReport);

  using FObjectReader::operator<<;

  virtual FArchive &operator<<(FName &Value) override;
  virtual FArchive &operator<<(UObject *&Value) override;
  virtual FArchive &operator<<(FObjectPtr &Value) override;
  virtual FArchive &operator<<(FWeakObjectPtr &Value) override;
  virtual FArchive &operator<<(FSoftObjectPtr &Value) override;
  virtual FArchive &operator<<(FSoftObjectPath &Value) override;

  virtual bool ShouldSkipProperty(const FProperty *InProperty) const override;

  virtual FString GetArchiveName() const override {
    return TEXT("FUIWTCheckpointReader");
  }

private:
  // Consumes the payload of an ActorRecord or ComponentOfRecord token and
  // looks it up in the restore context. Counts and reports a miss.
  bool ReadWorldReference(EUIWTRefToken InToken, UObject *&OutObject);
  void ReadReference(UObject *&InOutValue);

  const FUIWTRestoreRefContext &Context;
  FUIWTRestoreReport &Report;
};
