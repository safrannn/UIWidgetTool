#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

class FUIWidgetToolPluginModule : public IModuleInterface {
public:
  virtual void StartupModule() override;
  virtual void ShutdownModule() override;

  static const FName ManagerTabId;
  static const FName UpdateTabId;
  static const FName PlayTabId;

  static void OpenUpdateTab();
  static void OpenPlayTab();

private:
  void RegisterMenus();

  void RegisterContentBrowserExtension();
  void OnAddToTool(TArray<FAssetData> SelectedAssets);
  TSharedRef<class SDockTab> SpawnManagerTab(const class FSpawnTabArgs &Args);
  TSharedRef<class SDockTab> SpawnUpdateTab(const class FSpawnTabArgs &Args);
  TSharedRef<class SDockTab> SpawnPlayTab(const class FSpawnTabArgs &Args);

  TSharedPtr<class SUIWidgetManager> ManagerWidget;
};
