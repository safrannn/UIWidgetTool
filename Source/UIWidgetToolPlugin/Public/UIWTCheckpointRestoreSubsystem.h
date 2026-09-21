#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/WorldSubsystem.h"
#include "UIWTCheckpointArchive.h"
#include "UIWTCheckpointTypes.h"
#include "UIWTPendingRestore.h"
#include "UIWTRestoreReport.h"

#include "UIWTCheckpointRestoreSubsystem.generated.h"

class UUserWidget;

// Fires from the PIE world once a checkpoint restore has finished with its
// widget on screen: the restored state under the widget class as it is now.
// Silent for a failed restore, a play with no checkpoint, or a widget that
// was never created, so listeners can act without re-checking those.
DECLARE_MULTICAST_DELEGATE_TwoParams(FUIWTOnRestoreFinished, UWorld *,
                                     const FUIWTPendingRestore &);

UCLASS()
class UIWIDGETTOOLPLUGIN_API UUIWTCheckpointRestoreSubsystem
    : public UWorldSubsystem {
  GENERATED_BODY()

public:
  virtual bool ShouldCreateSubsystem(UObject *Outer) const override;
  virtual void OnWorldBeginPlay(UWorld &InWorld) override;
  virtual void Deinitialize() override;

  void RebuildWidget();

  bool HasWidget() const { return CreatedWidget != nullptr; }

  static FUIWTOnRestoreFinished &OnRestoreFinished();

private:
  enum class EState : uint8 {
    Idle,
    WaitingForStreaming,
    WaitingForPlayerController,
    Done,
    Failed,
  };

  bool Tick(float DeltaTime);
  void StopTicker();

  bool LoadPayload(const FString &InPayloadPath);

  void ApplyPlayerTransform(UWorld &InWorld);
  void ReconcileActors(UWorld &InWorld);
  void ApplyRecords(UWorld &InWorld);
  void FinishDeferredSpawns(UWorld &InWorld);

  bool TryCreateWidget(UWorld &InWorld);
  void Finish(UWorld &InWorld, EState InFinalState);

  void SetPaused(UWorld &InWorld, bool bPaused);

  FUIWTPendingRestore Request;
  FUIWTCheckpointHeader Header;
  FUIWTCheckpointPayload Payload;
  FUIWTRestoreReport Report;

  FUIWTRestoreRefContext Bindings;

  UPROPERTY()
  TArray<TObjectPtr<AActor>> DeferredSpawns;

  UPROPERTY()
  TObjectPtr<UUserWidget> CreatedWidget;

  FTSTicker::FDelegateHandle TickerHandle;

  EState State = EState::Idle;

  double StateEnteredSeconds = 0.0;

  bool bPausedByRestore = false;
};
