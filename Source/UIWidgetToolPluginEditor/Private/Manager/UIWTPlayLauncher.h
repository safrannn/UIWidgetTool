#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"
#include "UIWidgetPreviewObjectManagerSettings.h"

struct FUIWTPlayRequest
{
  FString MapPackagePath;

  FGuid CheckpointId;
  FString CheckpointSidecarPath;
  FString CheckpointPayloadPath;

  TSoftClassPtr<UUserWidget> WidgetClass;
};

namespace UIWTPlayLauncher
{
  bool RequestPlay(const FUIWTPlayRequest &InRequest, FText &OutError);

  void Startup();
  void Shutdown();
}
