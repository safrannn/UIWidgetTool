#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

class FTabManager;
class FUICommandList;
class FUIWTInputProcessor;
class SDockTab;
class SUIWTChatSection;
class SUIWTSnapshotViewer;
class SUIWidgetManager;

class FUIWidgetToolPluginEditorModule : public IModuleInterface
{
public:
  virtual void StartupModule() override;
  virtual void ShutdownModule() override;

  static FUIWidgetToolPluginEditorModule &Get();

  static const FName ManagerTabId; // id registered in global tab manager.
  static const FName ManagerPanelTabId;
  static const FName SnapshotViewerTabId;

  bool LoadSnapshotInViewer(const FString &InSnapshotPath, FText &OutError);

  static void CaptureCheckpointNow();

  void RefreshManager();

private:
  void RegisterMenus();

  TSharedRef<SDockTab> SpawnManagerTab(const class FSpawnTabArgs &Args);
  TSharedRef<SDockTab> SpawnManagerPanelTab(const class FSpawnTabArgs &Args);
  TSharedRef<SDockTab> SpawnSnapshotViewerTab(const class FSpawnTabArgs &Args);
  void OnManagerTabClosed(TSharedRef<SDockTab> ClosedTab);
  TSharedRef<SUIWidgetManager> MakeManagerWidget();
  void OnManagerSelectionChanged();
  void PushSelectionToViewer();
  void SyncSnapshotToViewer();

  // === Editor event hooks ===
  void OnBeginPIE(bool bIsSimulating);
  void OnEndPIE(bool bIsSimulating);
  void RegisterInputProcessor();
  void UnregisterInputProcessor();
  void OnBlueprintCompiled();
  void OnRestoreFinished(UWorld *InWorld,
                         const struct FUIWTPendingRestore &InRequest);
  TSharedPtr<SUIWTSnapshotViewer> SnapshotViewerWidget;
  FString SyncedSnapshotPath;

  static void QueueWidgetSnapshot(const FString &InSnapshotPath,
                                  int32 InSettleFrames = 0);

  // === States ===
  TSharedPtr<FTabManager> ManagerTabManager;
  TSharedPtr<SUIWidgetManager> ManagerWidget;
  TSharedPtr<SUIWTChatSection> ChatSection;
  TSharedPtr<FUICommandList> CommandList;
  TSharedPtr<FUIWTInputProcessor> InputProcessor;
};
