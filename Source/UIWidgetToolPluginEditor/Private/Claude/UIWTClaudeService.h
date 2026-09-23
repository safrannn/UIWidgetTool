#pragma once

#include "CoreMinimal.h"
#include "UIWTChatTypes.h"

class FUIWTClaudeRunner;
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
  bool IsRunInFlight() const;
  // Starts a headless Claude Code run against the entry. Fails with OutError
  // (and no side effects) when the entry has no blueprint, PIE is up, the
  // server is not connected, or a run is already in flight. InImage is
  // optional; with it, InPrompt may be empty.
  bool StartRun(const FGuid &InEntryId, const FString &InPrompt,
                const TSharedPtr<const FUIWTPromptImage> &InImage,
                const FUIWTRunContext &InContext, FText &OutError);
  void CancelRun();

  // --- MCP server.

  bool IsMcpConnected() const { return bMcpConnected; }
  bool ConnectMcp(FText &OutError);
  void DisconnectMcp();

private:
  void RegisterToolset();
  void UnregisterToolset();

  void OnRunEvent(const FUIWTClaudeRunEvent &InEvent);
  void FinishRun(const FUIWTClaudeRunEvent &InEvent);
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
};
