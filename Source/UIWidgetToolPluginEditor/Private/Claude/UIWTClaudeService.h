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

// An AI pass on a design import (import-tree.md → Claude pass), as the chat
// panel starts it.
struct FUIWTRefineStart
{
  // What the chat shows as the user's message.
  FString DisplayText;
  // The request (UIWTDesignRefine::Prepare).
  FString Request;
  // The reference, or its crop to the scope. It isn't saved over the
  // blueprint's reference.png.
  TSharedPtr<const FUIWTPromptImage> Image;
  // For the usage record: the source, the scope, its node count and the
  // round limit.
  FString Source;
  FString Scope;
  int32 Nodes = 0;
  int32 Rounds = 0;
  // The scope is the sidecar's changedNodes: cleared when the pass succeeds.
  bool bChangedParts = false;
};

// An image-reading run (import-image.md, section 1), as the chat panel
// starts it: Claude reads the image into a design tree with CutImageNode and
// WriteDesignTree, and each write imports or merges.
struct FUIWTImageReadStart
{
  FString DisplayText;
  // UIWTImageReader::BuildRequest.
  FString Request;
  // The image, attached; the run's reference is the cached copy.
  TSharedPtr<const FUIWTPromptImage> Image;
  // Saved/UIWidgetTool/Image/<name>_<crc>, with reference.png in it.
  FString CacheDir;
  FString SourceFile;
  FString Crc;
  // Where the first write imports to; empty: the default folder and
  // WBP_<root name>.
  FString TargetFolder;
  FString BlueprintName;
  // Reference pixels per design pixel.
  double Scale = 1.0;
  // For the estimate and the usage record.
  int32 Nodes = 0;
  int32 Rounds = 0;
  // The AI pass's work is in the same request.
  bool bAIPass = false;
  // Tests import unsaved into /Temp.
  bool bSave = true;
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
  // An AI pass on the entry's blueprint, which a design import made
  // (import-tree.md → Claude pass): a new session with the pass's request,
  // its own model and effort settings, and the reference (or its crop)
  // attached. At the end the renames it made are written into the design
  // sidecar, and what it used is added to the chat and to
  // Saved/UIWidgetTool/AIPassUsage.csv next to its estimate.
  bool StartDesignRefine(const FGuid &InEntryId, const FUIWTRefineStart &InStart,
                         const FUIWTRunContext &InContext, FText &OutError);
  // An image-reading run on an entry that has no blueprint yet: the first
  // WriteDesignTree gives it one. A new session, the AI pass's model and
  // effort settings, the image attached; usage recorded like a pass's.
  bool StartImageRead(const FGuid &InEntryId, const FUIWTImageReadStart &InStart,
                      const FUIWTRunContext &InContext, FText &OutError);
  void CancelRun();

  // Test hooks for the UIWT.RegisterDevTools console command: a run with no
  // Claude process, so the run's tools can be called over MCP directly.
  bool DevBeginRun(const FGuid &InEntryId, UWidgetBlueprint *InBlueprint,
                   const TSharedPtr<const FUIWTPromptImage> &InImage,
                   FText &OutError);
  void DevEndRun(bool bInSuccess);
  // The same for an image-reading run (its tools are called directly).
  bool DevBeginImageRead(const FGuid &InEntryId, const FUIWTImageReadStart &InStart,
                         FText &OutError);

  // --- MCP server.

  bool IsMcpConnected() const { return bMcpConnected; }
  bool ConnectMcp(FText &OutError);
  void DisconnectMcp();

private:
  // What differs between a chat run and an AI pass.
  struct FRunOptions
  {
    // Replaces the prompt as the request Claude gets; the chat still shows
    // the prompt.
    FString Request;
    // Override the settings' --model and --effort when set.
    FString Model;
    FString Effort;
    // Starts a new session instead of resuming the entry's.
    bool bNewSession = false;
    // The attached image isn't the run's reference: it's neither saved as
    // reference.png nor described as it; the request says what it is.
    bool bImageIsNotReference = false;
    // A design AI pass; its details for the run.
    const FUIWTRefineStart *Refine = nullptr;
    // An image-reading run; its details for the run.
    const FUIWTImageReadStart *ImageRead = nullptr;
  };

  // The run state an image-reading run starts with.
  static void InitImageRead(FUIWTActiveRun &InOutRun, const FUIWTImageReadStart &InStart,
                            const FString &InModel);

  bool StartRunInternal(const FGuid &InEntryId, const FString &InPrompt,
                        const TSharedPtr<const FUIWTPromptImage> &InImage,
                        const FUIWTRunContext &InContext, const FRunOptions &InOptions,
                        FText &OutError);

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
  // and saves InImage's original as the reference unless it isn't one.
  bool PrepareRunDirectory(FUIWTActiveRun &InOutRun,
                           const TSharedPtr<const FUIWTPromptImage> &InImage,
                           bool bInImageIsReference, FText &OutError);
  // An AI pass's usage next to its estimate: a line for the chat, and a row
  // in Saved/UIWidgetTool/AIPassUsage.csv.
  static FString RecordRefineUsage(const FUIWTActiveRun &InRun,
                                   const FUIWTClaudeRunEvent &InEvent);
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
