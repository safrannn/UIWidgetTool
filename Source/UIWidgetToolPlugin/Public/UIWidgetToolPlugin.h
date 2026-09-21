#pragma once

#include "CoreMinimal.h"
#include "Misc/Paths.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

class FUIWidgetToolPluginModule : public IModuleInterface
{
public:
  virtual void StartupModule() override {}
  virtual void ShutdownModule() override {}
};

namespace UIWT
{
  // The plugin's single per-developer scratch root. Checkpoints, generated
  // widget blueprints and the MCP client config all live under it.
  inline FString GetSavePath()
  {
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UIWidgetTool"));
  }
}
