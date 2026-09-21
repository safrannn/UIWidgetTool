#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "UIWidgetToolPluginStyle.h"

class FUIWTCommands : public TCommands<FUIWTCommands>
{
public:
  FUIWTCommands();

  virtual void RegisterCommands() override;

  TSharedPtr<FUICommandInfo> OpenManager;

  TSharedPtr<FUICommandInfo> CaptureCheckpoint;
};
