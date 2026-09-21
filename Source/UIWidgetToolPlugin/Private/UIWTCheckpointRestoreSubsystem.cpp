#include "UIWTCheckpointRestoreSubsystem.h"

#include "Blueprint/UserWidget.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Serialization/MemoryReader.h"
#include "UIWTCheckpointCapture.h"
#include "UIWTCheckpointCodec.h"
#include "UIWTSaveable.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "WorldPartition/WorldPartitionSubsystem.h"

namespace
{
  bool IsSafeToDestroy(AActor *InActor)
  {
    return UIWTImplementsSaveable(InActor) &&
           IUIWidgetToolSaveable::Execute_IsSafeToDestroyOnRestore(InActor);
  }

}

FUIWTOnRestoreFinished &UUIWTCheckpointRestoreSubsystem::OnRestoreFinished()
{
  static FUIWTOnRestoreFinished Delegate;
  return Delegate;
}

bool UUIWTCheckpointRestoreSubsystem::ShouldCreateSubsystem(
    UObject *Outer) const
{
  const UWorld *World = Cast<UWorld>(Outer);
  return World && World->IsGameWorld();
}

void UUIWTCheckpointRestoreSubsystem::OnWorldBeginPlay(UWorld &InWorld)
{
  Super::OnWorldBeginPlay(InWorld);

  if (InWorld.WorldType != EWorldType::PIE ||
      InWorld.GetNetMode() == NM_Client)
  {
    return;
  }

  FUIWTPendingRestore Claimed;
  if (!FUIWTPendingRestore::Get().Claim(Claimed))
  {
    return;
  }

  const FString MapPackagePath =
      UIWTCheckpointCapture::GetMapPackagePath(&InWorld);
  if (!Claimed.MapPackagePath.IsEmpty() &&
      !Claimed.MapPackagePath.Equals(MapPackagePath, ESearchCase::IgnoreCase))
  {
    FUIWTPendingRestore::Get() = Claimed;
    UE_LOG(LogUIWidgetToolPlugin, Warning,
           TEXT("World '%s' declined a restore queued for '%s'."),
           *MapPackagePath, *Claimed.MapPackagePath);
    return;
  }

  Request = MoveTemp(Claimed);

  SetPaused(InWorld, true);

  if (!Request.CheckpointFile.IsEmpty())
  {
    if (!LoadPayload(Request.CheckpointFile))
    {
      Finish(InWorld, EState::Failed);
      return;
    }

    const FString CurrentSignature =
        UIWTCheckpointCapture::ComputeLevelSignature(&InWorld);
    if (!Header.LevelSignature.IsEmpty() &&
        Header.LevelSignature != CurrentSignature)
    {
      Report.bLevelSignatureMismatch = true;
      Report.AddMessage(FString::Printf(
          TEXT("Level signature changed since capture (%s -> %s); restored "
               "state may not line up with the current map."),
          *Header.LevelSignature, *CurrentSignature));
    }
  }

  ApplyPlayerTransform(InWorld);

  State = EState::WaitingForStreaming;
  StateEnteredSeconds = FPlatformTime::Seconds();

  TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
      FTickerDelegate::CreateUObject(this,
                                     &UUIWTCheckpointRestoreSubsystem::Tick),
      0.f);
}

void UUIWTCheckpointRestoreSubsystem::Deinitialize()
{
  StopTicker();
  Super::Deinitialize();
}

void UUIWTCheckpointRestoreSubsystem::StopTicker()
{
  if (TickerHandle.IsValid())
  {
    FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
    TickerHandle.Reset();
  }
}

void UUIWTCheckpointRestoreSubsystem::SetPaused(UWorld &InWorld, bool bPaused)
{
  if (bPaused == bPausedByRestore)
  {
    return;
  }
  InWorld.bDebugPauseExecution = bPaused;
  bPausedByRestore = bPaused;
}

bool UUIWTCheckpointRestoreSubsystem::LoadPayload(const FString &InPayloadPath)
{
  FUIWTCheckpointLoadOptions Options =
      GetDefault<UUIWidgetPreviewObjectManagerSettings>()->MakeLoadOptions();
  Options.ExpectedCheckpointId = Request.CheckpointId;
  Options.ExpectedMapPackagePath = Request.MapPackagePath;

  FUIWTCheckpointResult Result;
  if (!UIWTCheckpointCodec::Load(InPayloadPath, Options, Header, Payload,
                                 Result))
  {
    UE_LOG(LogUIWidgetToolPlugin, Error,
           TEXT("Restore aborted before touching the world: [%s] %s"),
           *Result.GetErrorCode(), *Result.Message);
    Report.AddMessage(
        FString::Printf(TEXT("[%s] %s"), *Result.GetErrorCode(), *Result.Message));
    return false;
  }
  return true;
}

// Moves each pawn to where its player's record left it, before the first
// frame renders, so the restore does not visibly teleport the camera later.
void UUIWTCheckpointRestoreSubsystem::ApplyPlayerTransform(UWorld &InWorld)
{
  if (Payload.Actors.Num() == 0)
  {
    return;
  }

  UIWTCheckpointCapture::ForEachPlayerActor(
      InWorld, [this](int32 PlayerIndex, AActor *Actor)
      {
        APawn *Pawn = Cast<APawn>(Actor);
        if (!IsValid(Pawn)) {
          return;
        }
        for (const FUIWTActorRecord &Record : Payload.Actors) {
          if (Record.PlayerIndex != PlayerIndex) {
            continue;
          }
          const UClass *RecordClass =
              FindObject<UClass>(nullptr, *Record.ClassPath);
          if (RecordClass && Pawn->IsA(RecordClass)) {
            Pawn->SetActorTransform(Record.Transform, false, nullptr,
                                    ETeleportType::TeleportPhysics);
            return;
          }
        } });
}

void UUIWTCheckpointRestoreSubsystem::ReconcileActors(UWorld &InWorld)
{
  TMap<FGuid, AActor *> ByInstanceGuid;
  TMap<FString, AActor *> ByPath;
  TMap<int32, TArray<AActor *>> ByPlayerIndex;

  UIWTCheckpointCapture::ForEachPlayerActor(
      InWorld, [&ByPlayerIndex](int32 PlayerIndex, AActor *Actor)
      { ByPlayerIndex.FindOrAdd(PlayerIndex).Add(Actor); });

  for (TActorIterator<AActor> It(&InWorld); It; ++It)
  {
    AActor *Actor = *It;
    if (!IsValid(Actor))
    {
      continue;
    }
    const FGuid InstanceGuid =
        UIWTCheckpointArchive::GetActorInstanceGuid(Actor);
    if (InstanceGuid.IsValid())
    {
      ByInstanceGuid.Add(InstanceGuid, Actor);
    }
    ByPath.Add(UWorld::RemovePIEPrefix(Actor->GetPathName()), Actor);
  }

  TSet<AActor *> Claimed;

  for (const FUIWTActorRecord &Record : Payload.Actors)
  {
    AActor *Matched = nullptr;

    if (Record.PlayerIndex != INDEX_NONE)
    {
      if (const TArray<AActor *> *Group =
              ByPlayerIndex.Find(Record.PlayerIndex))
      {
        UClass *RecordClass = FindObject<UClass>(nullptr, *Record.ClassPath);
        for (AActor *Candidate : *Group)
        {
          if (RecordClass && Candidate->IsA(RecordClass) &&
              !Claimed.Contains(Candidate))
          {
            Matched = Candidate;
            break;
          }
        }
      }
    }
    else if (!Record.bRuntimeSpawned)
    {
      if (Record.ActorInstanceGuid.IsValid())
      {
        Matched = ByInstanceGuid.FindRef(Record.ActorInstanceGuid);
      }
      if (!Matched)
      {
        Matched = ByPath.FindRef(Record.ActorPath);
      }
    }

    if (Matched)
    {
      Claimed.Add(Matched);
      Bindings.RecordIdToActor.Add(Record.RecordId, Matched);
      ++Report.MatchedActors;
      continue;
    }

    if (!Record.bRuntimeSpawned)
    {
      ++Report.UnmatchedRecords;
      Report.AddMessage(FString::Printf(
          TEXT("Level actor '%s' from the checkpoint is not in this world%s."),
          *Record.ActorPath,
          Report.bStreamingTimedOut ? TEXT(" (streaming timed out)")
                                    : TEXT("")));
      continue;
    }

    UClass *SpawnClass = LoadObject<UClass>(nullptr, *Record.ClassPath);
    if (!SpawnClass)
    {
      ++Report.SpawnFailures;
      Report.AddMessage(FString::Printf(
          TEXT("Class '%s' could not be loaded; its actor was not respawned."),
          *Record.ClassPath));
      continue;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride =
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    SpawnParams.bDeferConstruction = true;
    AActor *Spawned =
        InWorld.SpawnActor<AActor>(SpawnClass, Record.Transform, SpawnParams);
    if (!Spawned)
    {
      ++Report.SpawnFailures;
      Report.AddMessage(FString::Printf(
          TEXT("Spawning '%s' failed."), *Record.ClassPath));
      continue;
    }

    DeferredSpawns.Add(Spawned);
    Bindings.RecordIdToActor.Add(Record.RecordId, Spawned);
    Claimed.Add(Spawned);
    ++Report.SpawnedRuntimeActors;
  }

  for (const TPair<FGuid, AActor *> &Pair : Bindings.RecordIdToActor)
  {
    UIWTCheckpointArchive::BuildComponentKeys(
        Pair.Value, Bindings.RecordIdToComponents.Add(Pair.Key));
  }

  for (TActorIterator<AActor> It(&InWorld); It; ++It)
  {
    AActor *Actor = *It;
    if (!IsValid(Actor) || Claimed.Contains(Actor) ||
        UIWTCheckpointArchive::GetActorInstanceGuid(Actor).IsValid() ||
        !IsSafeToDestroy(Actor))
    {
      continue;
    }
    Report.AddMessage(FString::Printf(
        TEXT("Destroyed runtime actor '%s', which opted in to removal and is "
             "absent from the checkpoint."),
        *Actor->GetName()));
    Actor->Destroy();
  }
}

void UUIWTCheckpointRestoreSubsystem::ApplyRecords(UWorld &InWorld)
{
  for (const FUIWTActorRecord &Record : Payload.Actors)
  {
    AActor *Actor = Bindings.RecordIdToActor.FindRef(Record.RecordId);
    if (!IsValid(Actor))
    {
      continue;
    }

    Actor->SetActorTransform(Record.Transform, false, nullptr,
                             ETeleportType::TeleportPhysics);

    if (Record.PropertyData.Num() > 0)
    {
      UIWTCheckpointArchive::ReadObjectProperties(Actor, Record.PropertyData,
                                                  Bindings, Report);
    }

    const TMap<FName, UActorComponent *> *LiveComponents =
        Bindings.RecordIdToComponents.Find(Record.RecordId);
    for (const TPair<FName, FUIWTComponentRecord> &Pair : Record.Components)
    {
      UActorComponent *Component =
          LiveComponents ? LiveComponents->FindRef(Pair.Key) : nullptr;
      if (!IsValid(Component))
      {
        ++Report.UnmatchedComponents;
        continue;
      }

      if (USceneComponent *SceneComponent = Cast<USceneComponent>(Component))
      {
        SceneComponent->SetRelativeTransform(Pair.Value.RelativeTransform);
      }
      if (Pair.Value.PropertyData.Num() > 0)
      {
        UIWTCheckpointArchive::ReadObjectProperties(
            Component, Pair.Value.PropertyData, Bindings, Report);
      }
      ++Report.AppliedComponents;
    }
  }

  // Custom data goes last so implementers see every reflected property and
  // reference already in place.
  for (const FUIWTActorRecord &Record : Payload.Actors)
  {
    if (Record.CustomData.Num() == 0)
    {
      continue;
    }
    AActor *Actor = Bindings.RecordIdToActor.FindRef(Record.RecordId);
    if (!IsValid(Actor))
    {
      continue;
    }
    if (!UIWTImplementsSaveable(Actor))
    {
      Report.AddMessage(FString::Printf(
          TEXT("'%s' has custom checkpoint data but no longer implements the "
               "saveable interface; the data was left unapplied."),
          *Actor->GetName()));
      ++Report.CustomSectionFailures;
      continue;
    }

    TArray<uint8> &MutableData =
        const_cast<TArray<uint8> &>(Record.CustomData);
    FMemoryReader Reader(MutableData, true);
    int32 DataVersion = 0;
    TArray<uint8> Bytes;
    Reader << DataVersion;
    Reader << Bytes;
    if (Reader.IsError())
    {
      ++Report.CustomSectionFailures;
      Report.AddMessage(FString::Printf(
          TEXT("Custom data for '%s' did not parse."), *Actor->GetName()));
      continue;
    }
    IUIWidgetToolSaveable::Execute_OnCheckpointRestore(Actor, Bytes,
                                                       DataVersion);
  }
}

void UUIWTCheckpointRestoreSubsystem::FinishDeferredSpawns(UWorld &InWorld)
{
  for (AActor *Actor : DeferredSpawns)
  {
    if (IsValid(Actor))
    {
      Actor->FinishSpawning(Actor->GetActorTransform(), true);
    }
  }
  DeferredSpawns.Reset();

  UIWTCheckpointDelegates::OnRestoreSections().Broadcast(
      &InWorld, Payload.CustomSections, Report);
  Report.CustomSectionsApplied =
      FMath::Max(0, Payload.CustomSections.Num() - Report.CustomSectionFailures);
}

bool UUIWTCheckpointRestoreSubsystem::TryCreateWidget(UWorld &InWorld)
{
  if (Request.WidgetClass.IsNull())
  {
    return true;
  }

  APlayerController *PC = InWorld.GetFirstPlayerController();
  if (!IsValid(PC))
  {
    return false;
  }

  UClass *WidgetClass = Request.WidgetClass.LoadSynchronous();
  if (!WidgetClass)
  {
    Report.AddMessage(FString::Printf(
        TEXT("Widget class '%s' could not be loaded."),
        *Request.WidgetClass.ToString()));
    return true;
  }

  CreatedWidget = CreateWidget<UUserWidget>(PC, WidgetClass);
  if (!CreatedWidget)
  {
    Report.AddMessage(TEXT("CreateWidget returned null."));
    return true;
  }

  CreatedWidget->AddToViewport();
  return true;
}

void UUIWTCheckpointRestoreSubsystem::RebuildWidget()
{
  UWorld *World = GetWorld();
  if (!World || Request.WidgetClass.IsNull())
  {
    return;
  }

  if (CreatedWidget)
  {
    CreatedWidget->RemoveFromParent();
    CreatedWidget = nullptr;
  }

  TryCreateWidget(*World);
}

void UUIWTCheckpointRestoreSubsystem::Finish(UWorld &InWorld,
                                             EState InFinalState)
{
  State = InFinalState;
  StopTicker();
  SetPaused(InWorld, false);

  const bool bFailed = State == EState::Failed;
  const TCHAR *Verdict = bFailed ? TEXT("FAILED") : TEXT("done");
  UE_LOG(LogUIWidgetToolPlugin, Log, TEXT("Checkpoint restore %s: %s"), Verdict,
         *Report.ToString());
  for (const FString &Message : Report.Messages)
  {
    UE_LOG(LogUIWidgetToolPlugin, Warning, TEXT("  %s"), *Message);
  }

  if (GEngine)
  {
    const FColor Colour = bFailed                ? FColor::Red
                          : Report.HasProblems() ? FColor::Yellow
                                                 : FColor::Green;
    GEngine->AddOnScreenDebugMessage(
        -1, 10.f, Colour,
        FString::Printf(TEXT("UI Widget Tool restore %s: %s"), Verdict,
                        *Report.ToString()));
  }

  if (!bFailed && !Request.CheckpointFile.IsEmpty() && HasWidget())
  {
    OnRestoreFinished().Broadcast(&InWorld, Request);
  }

  FUIWTPendingRestore::Clear();
}

bool UUIWTCheckpointRestoreSubsystem::Tick(float DeltaTime)
{
  UWorld *World = GetWorld();
  if (!World)
  {
    return false;
  }

  const double Elapsed = FPlatformTime::Seconds() - StateEnteredSeconds;

  switch (State)
  {
  case EState::WaitingForStreaming:
  {
    const float Timeout = GetDefault<UUIWidgetPreviewObjectManagerSettings>()
                              ->StreamingConvergenceTimeoutSeconds;

    bool bConverged = true;
    if (UWorldPartitionSubsystem *WPSubsystem =
            World->GetSubsystem<UWorldPartitionSubsystem>())
    {
      bConverged = WPSubsystem->IsStreamingCompleted();
    }

    if (!bConverged && Elapsed < Timeout)
    {
      return true;
    }

    if (!bConverged)
    {
      Report.bStreamingTimedOut = true;
      Report.AddMessage(FString::Printf(
          TEXT("World Partition streaming did not converge within %.1fs; "
               "actors in cells that never streamed in will read as unmatched."),
          Timeout));
    }

    if (!Request.CheckpointFile.IsEmpty())
    {
      ReconcileActors(*World);
      ApplyRecords(*World);
      FinishDeferredSpawns(*World);
    }

    State = EState::WaitingForPlayerController;
    StateEnteredSeconds = FPlatformTime::Seconds();
    return true;
  }

  case EState::WaitingForPlayerController:
  {
    const bool bCreated = TryCreateWidget(*World);
    if (!bCreated && Elapsed <= 5.0)
    {
      return true;
    }
    if (!bCreated)
    {
      Report.AddMessage(
          TEXT("No PlayerController appeared within 5s; the widget was not "
               "created."));
    }
    Finish(*World, EState::Done);
    return false;
  }

  default:
    return false;
  }
}
