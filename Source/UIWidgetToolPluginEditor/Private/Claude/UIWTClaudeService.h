#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "UIWTChatTypes.h"

class FUIWTClaudeRunner;
class UWidgetBlueprint;
struct FUIWTClaudeRunEvent;

DECLARE_MULTICAST_DELEGATE(FOnUIWTChatChanged);
DECLARE_MULTICAST_DELEGATE(FOnUIWTEntriesChanged);

// What a run is about, gathered by the caller: level and checkpoint from the
// manager entry, picked widget from the snapshot viewer. The service never
// reads the widgets itself.
struct FUIWTRunContext
{
  FString LevelPackagePath;
  FString CheckpointDisplay;
  FString PickedWidget;
};

// Chat state, headless Claude Code runs and the editor's MCP server. Lives
// for the editor module's lifetime, not the manager widget's, so chat history
// survives the tab being closed and reopened.
class FUIWTClaudeService
{
public:
  FUIWTClaudeService();
  ~FUIWTClaudeService();

  // The owning module creates the service in StartupModule and destroys it
  // in ShutdownModule.
  static void Create();
  static void Destroy();
  // Asserts when the service does not exist.
  static FUIWTClaudeService &Get();
  // Null once the service is gone. For widget destructors, which can run
  // after the module has shut down while a dock tab still holds the widget.
  static FUIWTClaudeService *TryGet();

  // --- Chat state.

  FUIWTChatState &GetChatState(const FGuid &InEntryId);
  const FUIWTChatState *FindChatState(const FGuid &InEntryId) const;
  // Drops an entry's history and session once the entry is gone. A run still in
  // flight against it keeps working: GetChatState recreates on demand.
  void ForgetChatState(const FGuid &InEntryId);
  // Fires on the game thread whenever any entry's history changes.
  FOnUIWTChatChanged &OnChatChanged() { return ChatChanged; }
  // Fires on the game thread when a run changed something the manager list
  // shows: an entry note, or the entry's blueprint at the end of a run.
  FOnUIWTEntriesChanged &OnEntriesChanged() { return EntriesChanged; }

  // --- Runs.

  const FUIWTActiveRun *GetActiveRun() const;
  // For the run's tools, which record the files and textures they make.
  FUIWTActiveRun *GetActiveRunMutable();
  bool IsRunInFlight() const;
  // Starts a headless Claude Code run against the entry. Fails with OutError
  // (and no side effects) when the entry has no blueprint, PIE is up, the
  // server is not connected, or a run is already in flight. A blueprint not
  // yet in its own folder is moved there first. InImage is optional; with
  // it, InPrompt may be empty.
  bool StartRun(const FGuid &InEntryId, const FString &InPrompt,
                const TSharedPtr<const FUIWTPromptImage> &InImage,
                const FUIWTRunContext &InContext, FText &OutError);
  void CancelRun();

  // Test hooks for the UIWT.RegisterDevTools console command: a run with no
  // Claude process, so the run's tools can be called over MCP directly.
  bool DevBeginRun(const FGuid &InEntryId, UWidgetBlueprint *InBlueprint,
                   const TSharedPtr<const FUIWTPromptImage> &InImage,
                   FText &OutError);
  void DevEndRun(bool bInSuccess);

  // --- MCP server.

  bool IsMcpConnected() const { return bMcpConnected; }
  bool ConnectMcp(FText &OutError);
  void DisconnectMcp();

private:
  void RegisterToolset();
  void UnregisterToolset();

  void OnRunEvent(const FUIWTClaudeRunEvent &InEvent);
  void FinishRun(const FUIWTClaudeRunEvent &InEvent);
  // Moves the blueprint into its own folder if it is not there yet, and
  // points every entry that used it at the new path.
  bool MoveBlueprintToOwnFolder(UWidgetBlueprint *InBlueprint,
                                FText &OutError);
  // Where run files went before widgets had their own folder.
  static FString GetLegacyRunDirectory(const FGuid &InEntryId);
  // Fills the run's folders, clears the previous run's renders and zooms,
  // and saves InImage's original as the reference.
  bool PrepareRunDirectory(FUIWTActiveRun &InOutRun,
                           const TSharedPtr<const FUIWTPromptImage> &InImage,
                           FText &OutError);
  // The editor imports image files that appear under Content/ as new
  // assets. A run writes its loose files next to the widget, so that is
  // off while it runs, and back on once the files it wrote last are past
  // the import threshold. In memory only; the setting's saved value is
  // never touched.
  void PauseAutoCreateAssets();
  void ResumeAutoCreateAssetsLater();
  void ResumeAutoCreateAssets();
  // After a failed run: deletes the textures it created and reloads the ones
  // it changed, so disk is again the last good state. Appends to OutReason.
  static void RestoreRunTextures(const FUIWTActiveRun &InRun,
                                 FString &OutReason);
  void AppendMessage(const FGuid &InEntryId, EUIWTChatRole InRole,
                     const FString &InText,
                     TSharedPtr<const FUIWTPromptImage> InImage = nullptr);
  void ReplaceStatus(const FGuid &InEntryId, const FString &InText);
  FString WriteMcpConfig(FText &OutError) const;

  TMap<FGuid, TSharedRef<FUIWTChatState>> ChatStates;
  FOnUIWTChatChanged ChatChanged;
  FOnUIWTEntriesChanged EntriesChanged;
  TUniquePtr<FUIWTActiveRun> ActiveRun;
  TSharedPtr<FUIWTClaudeRunner> Runner;
  FString ClaudeExecutable;
  bool bMcpConnected = false;
  bool bToolsetRegistered = false;
  bool bAutoCreatePaused = false;
  FTSTicker::FDelegateHandle ResumeAutoCreateHandle;
};
