#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"

class FUIWTCommands : public TCommands<FUIWTCommands>
{
public:
  FUIWTCommands();

  virtual void RegisterCommands() override;

  TSharedPtr<FUICommandInfo> OpenManager;

  TSharedPtr<FUICommandInfo> CaptureCheckpoint;
};
