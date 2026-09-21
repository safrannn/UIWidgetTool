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

  // Builds the manager with its chat section and wires the two to each
  // other and to the viewer. Both manager spawn paths go through here.
  TSharedRef<SUIWidgetManager> MakeManagerWidget();
  void OnManagerSelectionChanged();
  // Also bound to the chat service: a run edits the copy while it talks, so
  // every chat message re-pushes the selected blueprint to the viewer.
  void PushSelectionToViewer();
  // Loads the selected entry's snapshot into an open viewer, unless the
  // viewer already shows it. Never opens the tab: that is the button's job.
  void SyncSnapshotToViewer();

  // Writes the snapshot at end of frame, after InSettleFrames further frames
  // have passed, then syncs the viewer so a file it already shows is
  // reloaded.
  static void QueueWidgetSnapshot(const FString &InSnapshotPath,
                                  int32 InSettleFrames = 0);

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

  // === States ===
  TSharedPtr<FTabManager> ManagerTabManager;
  TSharedPtr<SUIWidgetManager> ManagerWidget;
  TSharedPtr<SUIWTChatSection> ChatSection;
  TSharedPtr<FUICommandList> CommandList;
  TSharedPtr<FUIWTInputProcessor> InputProcessor;
};
