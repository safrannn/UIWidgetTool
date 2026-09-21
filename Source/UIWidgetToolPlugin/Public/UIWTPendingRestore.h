#pragma once

#include "Blueprint/UserWidget.h"
#include "CoreMinimal.h"

struct UIWIDGETTOOLPLUGIN_API FUIWTPendingRestore {
  static FUIWTPendingRestore &Get();

  static void Clear();

  bool IsSet() const { return bPending; }

  FString CheckpointFile;

  // The checkpoint's sidecar, which names its snapshot file; empty when
  // CheckpointFile is.
  FString CheckpointSidecarFile;

  FGuid CheckpointId;

  FString MapPackagePath;

  TSoftClassPtr<UUserWidget> WidgetClass;

  bool bPending = false;

  bool Claim(FUIWTPendingRestore &OutClaimed);
};
