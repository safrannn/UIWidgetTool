#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

class FUIWidgetToolPluginModule : public IModuleInterface {
public:
  virtual void StartupModule() override;
  virtual void ShutdownModule() override;

  static const FName ManagerTabId;
  static const FName PreviewTabId;

  static void OpenPreviewTab(const FGuid &EntryId);

private:
  void RegisterMenus();

  void RegisterContentBrowserExtension();
  void OnAddToTool(TArray<FAssetData> SelectedAssets);
  TSharedRef<class SDockTab> SpawnManagerTab(const class FSpawnTabArgs &Args);
  TSharedRef<class SDockTab> SpawnPreviewTab(const class FSpawnTabArgs &Args);

  static FGuid PendingPreviewId;

  TSharedPtr<class SUIWidgetManager> ManagerWidget;
};
