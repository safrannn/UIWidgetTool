#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"

class AActor;
class UWorld;

struct UIWIDGETTOOLPLUGIN_API FUIWTCaptureResult {
  FGuid CheckpointId;
  FString PayloadPath;
  FString SidecarPath;
  FString MapPackagePath;

  int32 ActorCount = 0;
  int32 SkippedActors = 0;
  int64 UncompressedBytes = 0;
  int64 CompressedBytes = 0;

  FString Message;
};

namespace UIWTCheckpointCapture {
UIWIDGETTOOLPLUGIN_API bool CanCaptureWorld(const UWorld *InWorld,
                                            FText &OutReason);

UIWIDGETTOOLPLUGIN_API bool CaptureWorld(UWorld *InWorld,
                                         const FString &InDisplayName,
                                         FUIWTCaptureResult &OutResult);

UIWIDGETTOOLPLUGIN_API FString GetMapPackagePath(const UWorld *InWorld);

UIWIDGETTOOLPLUGIN_API FString ComputeLevelSignature(const UWorld *InWorld);

// Visits each local player's controller, player state, pawn and HUD with the
// player's ordinal. Those actors are respawned by every session, so a record
// identifies them by ordinal and class rather than by instance; capture and
// restore both number players through this function so they agree.
UIWIDGETTOOLPLUGIN_API void
ForEachPlayerActor(UWorld &InWorld,
                   TFunctionRef<void(int32 PlayerIndex, AActor *Actor)> InVisit);

}
