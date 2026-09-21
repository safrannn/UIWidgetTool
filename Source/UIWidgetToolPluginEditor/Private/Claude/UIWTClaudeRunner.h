#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include "Templates/Atomic.h"

struct FUIWTClaudeRunRequest
{
  // Full path to claude.exe / claude.cmd.
  FString Executable;
  // Sent on stdin, never on the command line.
  FString Prompt;
  // Resumes a previous session when set.
  FString SessionId;
  // Must be the project root every time: Claude Code keys sessions by cwd.
  FString WorkingDirectory;
  // Path to the mcp.json written under Saved/.
  FString McpConfigPath;
  float TimeoutSeconds = 600.f;
};

struct FUIWTClaudeRunEvent
{
  enum class EType : uint8
  {
    SessionStarted,
    ToolCall,
    Finished
  };

  EType Type = EType::Finished;
  FString Text;
  FString SessionId;
  // Finished only.
  bool bSuccess = false;
  bool bCancelled = false;
  bool bTimedOut = false;
  int32 ExitCode = -1;
};

DECLARE_DELEGATE_OneParam(FOnUIWTClaudeRunEvent, const FUIWTClaudeRunEvent &);

// Spawns `claude -p` headless and reports its stream-json output.
//
// The game thread never waits on the process: the editor's MCP server
// services Claude's tool calls from a ticker on the game thread, so any
// blocking wait here would deadlock the run. Stdout is drained on a worker
// thread; events are posted back to the game thread; completion is noticed
// by a ticker.
class FUIWTClaudeRunner : public TSharedFromThis<FUIWTClaudeRunner>
{
public:
  ~FUIWTClaudeRunner();

  bool Start(const FUIWTClaudeRunRequest &InRequest,
             FOnUIWTClaudeRunEvent InOnEvent, FText &OutError);

  void Cancel();

  // Tears the run down without reporting it: unbinds the event delegate,
  // stops the ticker, kills the process and waits (bounded) for the drain
  // thread, so nothing of ours runs after the owner is gone. For the owner's
  // destructor; Cancel is the user-facing path and still delivers Finished.
  void Abandon();

  bool IsRunning() const { return bRunning; }

  // Locates claude on PATH (or validates an explicit path). Empty on
  // failure with OutError set.
  static FString FindExecutable(const FString &InOverride, FText &OutError);

private:
  void DrainThread();
  void WritePromptAndCloseStdin();
  void HandleLine(const FString &InLine);
  bool Tick(float DeltaTime);
  void Finish();

  FProcHandle ProcessHandle;
  void *StdoutRead = nullptr;
  void *StdoutWrite = nullptr;
  void *StdinRead = nullptr;
  void *StdinWrite = nullptr;

  // Handed to the drain thread by Start, which launches it.
  FString PendingPrompt;

  FOnUIWTClaudeRunEvent OnEvent;
  FTSTicker::FDelegateHandle TickHandle;
  double StartTime = 0.0;
  float TimeoutSeconds = 600.f;

  TAtomic<bool> bRunning{false};
  TAtomic<bool> bDrainDone{false};
  TAtomic<bool> bCancelled{false};
  TAtomic<bool> bTimedOut{false};

  // Set by the worker from the "result" line; read on the game thread after
  // the drain has finished.
  FString ResultText;
  FString ResultSessionId;
  bool bResultIsError = false;
  bool bSawResult = false;
};
