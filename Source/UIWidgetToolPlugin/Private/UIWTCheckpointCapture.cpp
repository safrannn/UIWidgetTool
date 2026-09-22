#include "UIWTCheckpointCapture.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Brush.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/HUD.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Misc/Crc.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Serialization/MemoryWriter.h"
#include "UIWTCheckpointArchive.h"
#include "UIWTCheckpointCodec.h"
#include "UIWTSaveable.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "UIWidgetTool"

namespace UIWTCheckpointDelegates
{
  FUIWTOnCaptureSections &OnCaptureSections()
  {
    static FUIWTOnCaptureSections Delegate;
    return Delegate;
  }

  FUIWTOnRestoreSections &OnRestoreSections()
  {
    static FUIWTOnRestoreSections Delegate;
    return Delegate;
  }
}

namespace
{
  bool GCaptureInProgress = false;

  struct FCaptureLock
  {
    bool bAcquired = false;

    FCaptureLock()
    {
      if (!GCaptureInProgress)
      {
        GCaptureInProgress = true;
        bAcquired = true;
      }
    }
    ~FCaptureLock()
    {
      if (bAcquired)
      {
        GCaptureInProgress = false;
      }
    }
  };

  bool IsExcludedActorClass(const AActor *InActor)
  {
    return InActor->IsA<AWorldSettings>() || InActor->IsA<ABrush>() ||
           InActor->IsA<ALevelScriptActor>();
  }

  bool IsActorEligible(AActor *InActor)
  {
    if (!IsValid(InActor))
    {
      return false;
    }
    if (InActor->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
    {
      return false;
    }
    if (InActor->IsEditorOnly())
    {
      return false;
    }
    if (IsExcludedActorClass(InActor))
    {
      return false;
    }
    return !UIWTImplementsSaveable(InActor) ||
           IUIWidgetToolSaveable::Execute_ShouldCaptureToCheckpoint(InActor);
  }

  void AppendPackageIdentity(const FString &InPackageName,
                             TArray<FString> &OutLines)
  {
    if (InPackageName.IsEmpty())
    {
      return;
    }
    FString Filename;
    if (!FPackageName::DoesPackageExist(InPackageName, &Filename))
    {
      OutLines.Add(FString::Printf(TEXT("%s|missing"), *InPackageName));
      return;
    }
    const int64 FileSize = IFileManager::Get().FileSize(*Filename);
    const FDateTime TimeStamp = IFileManager::Get().GetTimeStamp(*Filename);
    OutLines.Add(FString::Printf(TEXT("%s|%lld|%lld"), *InPackageName, FileSize,
                                 TimeStamp.GetTicks()));
  }

}

namespace UIWTCheckpointCapture
{

  FString GetMapPackagePath(const UWorld *InWorld)
  {
    if (!InWorld || !InWorld->GetOutermost())
    {
      return FString();
    }
    return UWorld::RemovePIEPrefix(InWorld->GetOutermost()->GetName());
  }

  bool CanCaptureWorld(const UWorld *InWorld, FText &OutReason)
  {
    if (!InWorld)
    {
      OutReason = LOCTEXT("CaptureNoWorld",
                          "Start Play In Editor before capturing a checkpoint.");
      return false;
    }
    if (InWorld->WorldType != EWorldType::PIE)
    {
      OutReason = LOCTEXT("CaptureNotPIE",
                          "Checkpoints can only be captured during Play In "
                          "Editor. Simulate and standalone play are not "
                          "supported.");
      return false;
    }
    if (InWorld->GetNetMode() == NM_Client)
    {
      OutReason = LOCTEXT("CaptureNotServer",
                          "Only the server world can be captured; this is a "
                          "client world.");
      return false;
    }
    if (GCaptureInProgress)
    {
      OutReason =
          LOCTEXT("CaptureBusy", "A checkpoint capture is already in progress.");
      return false;
    }
    return true;
  }

  FString ComputeLevelSignature(const UWorld *InWorld)
  {
    if (!InWorld)
    {
      return FString();
    }

    TArray<FString> Lines;
    const FString MapPackage = GetMapPackagePath(InWorld);
    AppendPackageIdentity(MapPackage, Lines);

    if (const ULevel *PersistentLevel = InWorld->PersistentLevel)
    {
      TSet<FString> SeenPackages;
      for (const AActor *Actor : PersistentLevel->Actors)
      {
        if (!IsValid(Actor))
        {
          continue;
        }
        const UPackage *ExternalPackage = Actor->GetExternalPackage();
        if (!ExternalPackage)
        {
          continue;
        }
        const FString PackageName =
            UWorld::RemovePIEPrefix(ExternalPackage->GetName());
        if (!SeenPackages.Contains(PackageName))
        {
          SeenPackages.Add(PackageName);
          AppendPackageIdentity(PackageName, Lines);
        }
      }
    }

    Lines.Sort();
    const FString Joined = FString::Join(Lines, TEXT("\n"));
    const uint32 Crc = FCrc::StrCrc32(*Joined);

    return FString::Printf(TEXT("v1:%d:%08x"), Lines.Num(), Crc);
  }

  void ForEachPlayerActor(
      UWorld &InWorld,
      TFunctionRef<void(int32 PlayerIndex, AActor *Actor)> InVisit)
  {
    int32 PlayerIndex = 0;
    for (FConstPlayerControllerIterator It = InWorld.GetPlayerControllerIterator();
         It; ++It)
    {
      APlayerController *PC = It->Get();
      if (!IsValid(PC))
      {
        continue;
      }
      InVisit(PlayerIndex, PC);
      if (APlayerState *PlayerState = PC->PlayerState)
      {
        InVisit(PlayerIndex, PlayerState);
      }
      if (APawn *Pawn = PC->GetPawn())
      {
        InVisit(PlayerIndex, Pawn);
      }
      if (AHUD *HUD = PC->GetHUD())
      {
        InVisit(PlayerIndex, HUD);
      }
      ++PlayerIndex;
    }
  }

  bool CaptureWorld(UWorld *InWorld, const FString &InDisplayName,
                    FUIWTCaptureResult &OutResult)
  {
    OutResult = FUIWTCaptureResult();

    FText Reason;
    if (!CanCaptureWorld(InWorld, Reason))
    {
      OutResult.Message = Reason.ToString();
      return false;
    }

    FCaptureLock Lock;
    if (!Lock.bAcquired)
    {
      OutResult.Message = TEXT("A checkpoint capture is already in progress.");
      return false;
    }

    const UUIWidgetPreviewObjectManagerSettings *Settings =
        UUIWidgetPreviewObjectManagerSettings::Get();

    const bool bPreviousDebugPause = InWorld->bDebugPauseExecution;
    InWorld->bDebugPauseExecution = true;
    ON_SCOPE_EXIT { InWorld->bDebugPauseExecution = bPreviousDebugPause; };

    TArray<AActor *> EligibleActors;
    TMap<AActor *, int32> PlayerIndices;
    ForEachPlayerActor(*InWorld, [&PlayerIndices](int32 PlayerIndex,
                                                  AActor *Actor)
                       { PlayerIndices.Add(Actor, PlayerIndex); });

    for (TActorIterator<AActor> It(InWorld); It; ++It)
    {
      AActor *Actor = *It;
      if (IsActorEligible(Actor))
      {
        EligibleActors.Add(Actor);
      }
      else
      {
        ++OutResult.SkippedActors;
      }
    }

    FUIWTCaptureRefContext RefContext;
    RefContext.ActorToRecordId.Reserve(EligibleActors.Num());

    TArray<FGuid> RecordIds;
    RecordIds.Reserve(EligibleActors.Num());
    for (AActor *Actor : EligibleActors)
    {
      const FGuid RecordId = FGuid::NewGuid();
      RecordIds.Add(RecordId);
      RefContext.ActorToRecordId.Add(Actor, RecordId);
    }

    TArray<TMap<FName, UActorComponent *>> ActorComponents;
    ActorComponents.SetNum(EligibleActors.Num());
    for (int32 Index = 0; Index < EligibleActors.Num(); ++Index)
    {
      UIWTCheckpointArchive::BuildComponentKeys(EligibleActors[Index],
                                                ActorComponents[Index]);
      for (const TPair<FName, UActorComponent *> &Pair : ActorComponents[Index])
      {
        RefContext.ComponentToToken.Add(Pair.Value, {RecordIds[Index], Pair.Key});
      }
    }

    FUIWTCheckpointPayload Payload;
    Payload.Actors.Reserve(EligibleActors.Num());

    for (int32 Index = 0; Index < EligibleActors.Num(); ++Index)
    {
      AActor *Actor = EligibleActors[Index];

      FUIWTActorRecord Record;
      Record.RecordId = RecordIds[Index];
      Record.ActorInstanceGuid =
          UIWTCheckpointArchive::GetActorInstanceGuid(Actor);
      Record.ActorPath = UWorld::RemovePIEPrefix(Actor->GetPathName());
      Record.ClassPath = Actor->GetClass()->GetPathName();
      Record.Transform = Actor->GetActorTransform();

      // Player actors are matched by ordinal on restore, never respawned.
      const int32 *PlayerIndex = PlayerIndices.Find(Actor);
      Record.PlayerIndex = PlayerIndex ? *PlayerIndex : INDEX_NONE;
      Record.bRuntimeSpawned =
          !PlayerIndex && !Record.ActorInstanceGuid.IsValid();

      UIWTCheckpointArchive::WriteObjectProperties(Actor, RefContext,
                                                   Record.PropertyData);

      for (const TPair<FName, UActorComponent *> &Pair : ActorComponents[Index])
      {
        UActorComponent *Component = Pair.Value;

        FUIWTComponentRecord ComponentRecord;
        ComponentRecord.ClassPath = Component->GetClass()->GetPathName();
        if (const USceneComponent *SceneComponent =
                Cast<USceneComponent>(Component))
        {
          ComponentRecord.RelativeTransform =
              SceneComponent->GetRelativeTransform();
        }

        if (!Settings->IsComponentClassExcluded(ComponentRecord.ClassPath))
        {
          UIWTCheckpointArchive::WriteObjectProperties(
              Component, RefContext, ComponentRecord.PropertyData);
        }

        Record.Components.Add(Pair.Key, MoveTemp(ComponentRecord));
      }

      if (UIWTImplementsSaveable(Actor))
      {
        TArray<uint8> CustomBytes;
        IUIWidgetToolSaveable::Execute_OnCheckpointCapture(Actor, CustomBytes);
        if (CustomBytes.Num() > 0)
        {
          int32 DataVersion =
              IUIWidgetToolSaveable::Execute_GetCheckpointDataVersion(Actor);
          FMemoryWriter Writer(Record.CustomData, true);
          Writer << DataVersion;
          Writer << CustomBytes;
        }
      }

      Payload.Actors.Add(MoveTemp(Record));
    }

    UIWTCheckpointDelegates::OnCaptureSections().Broadcast(InWorld,
                                                           Payload.CustomSections);

    FUIWTCheckpointHeader Header;
    Header.CheckpointId = FGuid::NewGuid();
    Header.MapPackagePath = GetMapPackagePath(InWorld);
    Header.LevelSignature = ComputeLevelSignature(InWorld);
    Header.CapturedAtUtc = FDateTime::UtcNow();
    Header.DisplayName =
        InDisplayName.IsEmpty()
            ? UIWTCheckpointCodec::MakeDefaultDisplayName(Header)
            : InDisplayName;
    Header.WorldTimeSeconds = InWorld->GetTimeSeconds();

    const FString Directory = Settings->GetResolvedCheckpointDirectory();
    FUIWTCheckpointResult SaveResult;
    if (!UIWTCheckpointCodec::Save(Directory, Header, Payload,
                                   OutResult.PayloadPath, OutResult.SidecarPath,
                                   SaveResult))
    {
      OutResult.Message = SaveResult.Message;
      UE_LOG(LogUIWidgetToolPlugin, Error, TEXT("Checkpoint capture failed: %s"),
             *SaveResult.Message);
      return false;
    }

    OutResult.CheckpointId = Header.CheckpointId;
    OutResult.MapPackagePath = Header.MapPackagePath;
    OutResult.ActorCount = Header.ActorCount;
    OutResult.UncompressedBytes = Header.UncompressedPayloadSize;
    OutResult.CompressedBytes = Header.CompressedPayloadSize;

    const FUIWTRetentionReport Retention = UIWTCheckpointCodec::Prune(
        Directory, Header.MapPackagePath, Settings->MaxCheckpointsPerMap,
        Settings->GetAssignedCheckpointIds());
    if (Retention.Deleted > 0 || Retention.FailedToDelete > 0)
    {
      OutResult.Message = FString::Printf(
          TEXT("Retention removed %d older checkpoint(s) for this map%s."),
          Retention.Deleted,
          Retention.FailedToDelete > 0
              ? *FString::Printf(TEXT("; %d could not be deleted"),
                                 Retention.FailedToDelete)
              : TEXT(""));
    }

    UE_LOG(LogUIWidgetToolPlugin, Log,
           TEXT("Captured %d actors (%d skipped) from %s as checkpoint %s."),
           OutResult.ActorCount, OutResult.SkippedActors,
           *OutResult.MapPackagePath,
           *OutResult.CheckpointId.ToString(EGuidFormats::Digits));
    return true;
  }

}

#undef LOCTEXT_NAMESPACE
