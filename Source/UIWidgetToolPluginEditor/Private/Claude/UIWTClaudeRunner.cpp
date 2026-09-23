#include "UIWTClaudeRunner.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "UIWTCheckpointTypes.h"
#include "UIWTPromptImage.h"

#define LOCTEXT_NAMESPACE "UIWidgetTool"

namespace
{
  constexpr float TickIntervalSeconds = 0.1f;
  // How long Abandon waits for the drain thread after killing the process.
  constexpr double AbandonWaitSeconds = 2.0;

  // The three tool-search meta-tools the Unreal server advertises; every
  // real tool goes through call_tool, so exposure is controlled server-side.
  const TCHAR *const AllowedTools =
      TEXT("mcp__unreal-mcp__call_tool,mcp__unreal-mcp__list_toolsets,"
           "mcp__unreal-mcp__describe_toolset");

  FString Quote(const FString &In)
  {
    return FString::Printf(TEXT("\"%s\""), *In);
  }

  bool IsShellScript(const FString &InPath)
  {
    return InPath.EndsWith(TEXT(".cmd"), ESearchCase::IgnoreCase) ||
           InPath.EndsWith(TEXT(".bat"), ESearchCase::IgnoreCase);
  }

  // One stream-json user message (image block, then text) on a single line,
  // as `--input-format stream-json` expects. WritePipe adds the newline.
  FString MakeStreamJsonPrompt(const FString &InText,
                               const FUIWTPromptImage &InImage)
  {
    TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
    Source->SetStringField(TEXT("type"), TEXT("base64"));
    Source->SetStringField(TEXT("media_type"), InImage.MediaType);
    Source->SetStringField(TEXT("data"), InImage.Base64Data);
    TSharedRef<FJsonObject> ImageBlock = MakeShared<FJsonObject>();
    ImageBlock->SetStringField(TEXT("type"), TEXT("image"));
    ImageBlock->SetObjectField(TEXT("source"), Source);

    TSharedRef<FJsonObject> TextBlock = MakeShared<FJsonObject>();
    TextBlock->SetStringField(TEXT("type"), TEXT("text"));
    TextBlock->SetStringField(TEXT("text"), InText);

    TArray<TSharedPtr<FJsonValue>> Content;
    Content.Add(MakeShared<FJsonValueObject>(ImageBlock));
    Content.Add(MakeShared<FJsonValueObject>(TextBlock));
    TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
    Message->SetStringField(TEXT("role"), TEXT("user"));
    Message->SetArrayField(TEXT("content"), Content);
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("type"), TEXT("user"));
    Root->SetObjectField(TEXT("message"), Message);

    FString Json;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
        Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(
                &Json);
    FJsonSerializer::Serialize(Root, Writer);
    return Json;
  }
}

FUIWTClaudeRunner::~FUIWTClaudeRunner()
{
  if (TickHandle.IsValid())
  {
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
  }
  if (ProcessHandle.IsValid())
  {
    if (FPlatformProcess::IsProcRunning(ProcessHandle))
    {
      FPlatformProcess::TerminateProc(ProcessHandle, true);
    }
    FPlatformProcess::CloseProc(ProcessHandle);
  }
  FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
  FPlatformProcess::ClosePipe(StdinRead, StdinWrite);
}

FString FUIWTClaudeRunner::FindExecutable(const FString &InOverride,
                                          FText &OutError)
{
  IFileManager &Files = IFileManager::Get();
  if (!InOverride.IsEmpty())
  {
    if (Files.FileExists(*InOverride))
    {
      return FPaths::ConvertRelativePathToFull(InOverride);
    }
    OutError = FText::Format(
        LOCTEXT("ClaudeOverrideMissing",
                "Claude executable not found at {0} (UI Widget Tool (local) "
                "settings)."),
        FText::FromString(InOverride));
    return FString();
  }

  TArray<FString> Dirs;
  // Native installer and npm global install locations first.
  Dirs.Add(FPaths::Combine(
      FPlatformProcess::UserHomeDir(), TEXT(".local"), TEXT("bin")));
  const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
  if (!AppData.IsEmpty())
  {
    Dirs.Add(FPaths::Combine(AppData, TEXT("npm")));
  }
  // ParseIntoArray resets its output, so PATH goes through its own array.
  TArray<FString> PathDirs;
  FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"))
      .ParseIntoArray(PathDirs, TEXT(";"), true);
  Dirs.Append(MoveTemp(PathDirs));

  static const TCHAR *const Names[] = {TEXT("claude.exe"), TEXT("claude.cmd"),
                                       TEXT("claude")};
  for (const FString &Dir : Dirs)
  {
    for (const TCHAR *Name : Names)
    {
      const FString Candidate = FPaths::Combine(Dir, Name);
      if (Files.FileExists(*Candidate))
      {
        return FPaths::ConvertRelativePathToFull(Candidate);
      }
    }
  }

  OutError = LOCTEXT("ClaudeNotFound",
                     "Claude Code was not found on PATH. Install it, or set "
                     "the executable path in Editor Preferences > Plugins > "
                     "UI Widget Tool (local).");
  return FString();
}

bool FUIWTClaudeRunner::Start(const FUIWTClaudeRunRequest &InRequest,
                              FOnUIWTClaudeRunEvent InOnEvent, FText &OutError)
{
  check(IsInGameThread());
  if (bRunning)
  {
    OutError = LOCTEXT("RunBusy", "A run is already in flight.");
    return false;
  }

  FString Args;
  Args += TEXT("-p --verbose --output-format stream-json");
  Args += TEXT(" --mcp-config ") + Quote(InRequest.McpConfigPath);
  Args += TEXT(" --strict-mcp-config");
  Args += TEXT(" --allowedTools ") + Quote(AllowedTools);
  if (InRequest.Image.IsValid())
  {
    Args += TEXT(" --input-format stream-json");
  }
  if (!InRequest.SessionId.IsEmpty())
  {
    Args += TEXT(" --resume ") + Quote(InRequest.SessionId);
  }

  FString Url = InRequest.Executable;
  if (IsShellScript(Url))
  {
    // cmd.exe strips the outermost quotes after /c, so wrap the whole
    // command line in an extra pair.
    Args = FString::Printf(TEXT("/c \"%s %s\""), *Quote(Url), *Args);
    Url = TEXT("cmd.exe");
  }

  if (!FPlatformProcess::CreatePipe(StdoutRead, StdoutWrite) ||
      !FPlatformProcess::CreatePipe(StdinRead, StdinWrite,
                                    /*bWritePipeLocal=*/true))
  {
    OutError = LOCTEXT("RunPipeFailed", "Could not create process pipes.");
    return false;
  }

  ProcessHandle = FPlatformProcess::CreateProc(
      *Url, *Args, /*bLaunchDetached=*/false, /*bLaunchHidden=*/true,
      /*bLaunchReallyHidden=*/true, /*OutProcessID=*/nullptr, 0,
      *InRequest.WorkingDirectory, StdoutWrite, StdinRead);
  if (!ProcessHandle.IsValid())
  {
    FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
    FPlatformProcess::ClosePipe(StdinRead, StdinWrite);
    StdoutRead = StdoutWrite = StdinRead = StdinWrite = nullptr;
    OutError = FText::Format(
        LOCTEXT("RunSpawnFailed", "Could not start {0}."),
        FText::FromString(InRequest.Executable));
    return false;
  }

  UE_LOG(LogUIWidgetToolPlugin, Log, TEXT("Claude run started: %s %s"), *Url,
         *Args);

  // The child inherited the read end; our copy has to go now, or claude never
  // sees the EOF that ends the prompt. The prompt itself is written by its
  // own thread: one larger than the pipe buffer blocks until claude reads it,
  // the game thread is what services its MCP calls, and the drain thread has
  // to keep reading meanwhile in case claude writes before it has read all
  // of stdin.
  FPlatformProcess::ClosePipe(StdinRead, nullptr);
  StdinRead = nullptr;
  PendingPrompt = InRequest.Image.IsValid()
                      ? MakeStreamJsonPrompt(InRequest.Prompt, *InRequest.Image)
                      : InRequest.Prompt;

  OnEvent = MoveTemp(InOnEvent);
  TimeoutSeconds = InRequest.TimeoutSeconds;
  StartTime = FPlatformTime::Seconds();
  bRunning = true;
  bDrainDone = false;
  bCancelled = false;
  bTimedOut = false;
  bSawResult = false;
  bResultIsError = false;
  ResultText.Reset();
  ResultSessionId.Reset();

  bStdinDone = false;

  TSharedRef<FUIWTClaudeRunner> Self = AsShared();
  Async(EAsyncExecution::Thread, [Self]() { Self->WritePromptAndCloseStdin(); });
  Async(EAsyncExecution::Thread, [Self]() { Self->DrainThread(); });

  TickHandle = FTSTicker::GetCoreTicker().AddTicker(
      FTickerDelegate::CreateSP(this, &FUIWTClaudeRunner::Tick),
      TickIntervalSeconds);
  return true;
}

void FUIWTClaudeRunner::Cancel()
{
  if (!bRunning || bCancelled)
  {
    return;
  }
  bCancelled = true;
  if (ProcessHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcessHandle))
  {
    FPlatformProcess::TerminateProc(ProcessHandle, true);
  }
}

void FUIWTClaudeRunner::Abandon()
{
  check(IsInGameThread());

  // The owner is going away: nothing may call back into it. Finish is never
  // reached (no ticker), and queued HandleLine tasks drop their events
  // because bRunning is false.
  OnEvent.Unbind();
  if (TickHandle.IsValid())
  {
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    TickHandle.Reset();
  }
  if (!bRunning)
  {
    return;
  }
  bCancelled = true;
  bRunning = false;

  if (ProcessHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcessHandle))
  {
    FPlatformProcess::TerminateProc(ProcessHandle, true);
  }

  // The drain and stdin threads hold references to this runner and run code
  // from this module; give them a moment to notice the process is gone so
  // they are not still executing when the module unloads.
  const double Deadline = FPlatformTime::Seconds() + AbandonWaitSeconds;
  while ((!bDrainDone || !bStdinDone) && FPlatformTime::Seconds() < Deadline)
  {
    FPlatformProcess::Sleep(0.01f);
  }
  if (!bDrainDone || !bStdinDone)
  {
    UE_LOG(LogUIWidgetToolPlugin, Warning,
           TEXT("Claude run abandoned but its pipe threads did not finish "
                "within %.0f s."),
           AbandonWaitSeconds);
  }
}

// Runs on its own thread: closing our write end is the EOF claude waits for
// before it starts working. Killing the process makes a blocked write fail,
// so Cancel and Abandon never leave this stuck.
void FUIWTClaudeRunner::WritePromptAndCloseStdin()
{
  if (StdinWrite)
  {
    FPlatformProcess::WritePipe(StdinWrite, PendingPrompt);
    PendingPrompt.Reset();
    FPlatformProcess::ClosePipe(nullptr, StdinWrite);
    StdinWrite = nullptr;
  }
  bStdinDone = true;
}

void FUIWTClaudeRunner::DrainThread()
{
  FString Buffer;
  auto Pump = [&]()
  {
    const FString Chunk = FPlatformProcess::ReadPipe(StdoutRead);
    if (Chunk.IsEmpty())
    {
      return false;
    }
    Buffer += Chunk;
    int32 NewlineIndex;
    while (Buffer.FindChar(TEXT('\n'), NewlineIndex))
    {
      FString Line = Buffer.Left(NewlineIndex);
      Buffer.RightChopInline(NewlineIndex + 1);
      Line.TrimEndInline();
      if (!Line.IsEmpty())
      {
        HandleLine(Line);
      }
    }
    return true;
  };

  while (FPlatformProcess::IsProcRunning(ProcessHandle))
  {
    if (!Pump())
    {
      FPlatformProcess::Sleep(0.05f);
    }
  }
  // Whatever the process wrote between the last pump and exit.
  while (Pump())
  {
  }
  Buffer.TrimEndInline();
  if (!Buffer.IsEmpty())
  {
    HandleLine(Buffer);
  }
  bDrainDone = true;
}

// Runs on the drain thread. Only the pieces of Claude Code's stream-json the
// panel shows are decoded; everything else is ignored.
void FUIWTClaudeRunner::HandleLine(const FString &InLine)
{
  TSharedPtr<FJsonObject> Object;
  const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InLine);
  if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
  {
    UE_LOG(LogUIWidgetToolPlugin, Verbose, TEXT("claude: %s"), *InLine);
    return;
  }

  const FString Type = Object->GetStringField(TEXT("type"));
  TArray<FUIWTClaudeRunEvent> Events;

  if (Type == TEXT("system"))
  {
    FString SessionId;
    if (Object->TryGetStringField(TEXT("session_id"), SessionId) &&
        !SessionId.IsEmpty())
    {
      FUIWTClaudeRunEvent &Event = Events.AddDefaulted_GetRef();
      Event.Type = FUIWTClaudeRunEvent::EType::SessionStarted;
      Event.SessionId = SessionId;
    }
  }
  else if (Type == TEXT("assistant"))
  {
    const TSharedPtr<FJsonObject> *Message;
    const TArray<TSharedPtr<FJsonValue>> *Content;
    if (Object->TryGetObjectField(TEXT("message"), Message) &&
        (*Message)->TryGetArrayField(TEXT("content"), Content))
    {
      for (const TSharedPtr<FJsonValue> &Block : *Content)
      {
        const TSharedPtr<FJsonObject> *BlockObject;
        if (!Block->TryGetObject(BlockObject))
        {
          continue;
        }
        // Only tool_use blocks feed the status line; intermediate text is
        // ignored, the final reply arrives with the result line.
        if ((*BlockObject)->GetStringField(TEXT("type")) != TEXT("tool_use"))
        {
          continue;
        }
        FUIWTClaudeRunEvent &Event = Events.AddDefaulted_GetRef();
        Event.Type = FUIWTClaudeRunEvent::EType::ToolCall;
        Event.Text = (*BlockObject)->GetStringField(TEXT("name"));
        // call_tool wraps the real tool; show "Toolset.Tool" instead, or just
        // the tool when it is a top-level one.
        const TSharedPtr<FJsonObject> *Input;
        FString Inner;
        if ((*BlockObject)->TryGetObjectField(TEXT("input"), Input) &&
            (*Input)->TryGetStringField(TEXT("tool_name"), Inner))
        {
          FString Toolset;
          (*Input)->TryGetStringField(TEXT("toolset_name"), Toolset);
          Event.Text =
              Toolset.IsEmpty() ? Inner : Toolset + TEXT(".") + Inner;
        }
      }
    }
  }
  else if (Type == TEXT("result"))
  {
    bSawResult = true;
    Object->TryGetStringField(TEXT("result"), ResultText);
    Object->TryGetStringField(TEXT("session_id"), ResultSessionId);
    Object->TryGetBoolField(TEXT("is_error"), bResultIsError);
  }

  if (Events.Num() == 0)
  {
    return;
  }
  TWeakPtr<FUIWTClaudeRunner> WeakThis = AsShared();
  AsyncTask(ENamedThreads::GameThread, [WeakThis, Events = MoveTemp(Events)]()
            {
              const TSharedPtr<FUIWTClaudeRunner> This = WeakThis.Pin();
              if (!This.IsValid() || !This->bRunning)
              {
                return;
              }
              for (const FUIWTClaudeRunEvent &Event : Events)
              {
                This->OnEvent.ExecuteIfBound(Event);
              }
            });
}

bool FUIWTClaudeRunner::Tick(float)
{
  if (!bRunning)
  {
    TickHandle.Reset();
    return false;
  }
  if (bDrainDone)
  {
    Finish();
    TickHandle.Reset();
    return false;
  }
  if (!bCancelled && !bTimedOut &&
      FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
  {
    bTimedOut = true;
    if (ProcessHandle.IsValid() &&
        FPlatformProcess::IsProcRunning(ProcessHandle))
    {
      FPlatformProcess::TerminateProc(ProcessHandle, true);
    }
  }
  return true;
}

void FUIWTClaudeRunner::Finish()
{
  check(IsInGameThread());

  FUIWTClaudeRunEvent Event;
  Event.Type = FUIWTClaudeRunEvent::EType::Finished;
  Event.bCancelled = bCancelled;
  Event.bTimedOut = bTimedOut;
  FPlatformProcess::GetProcReturnCode(ProcessHandle, &Event.ExitCode);
  Event.SessionId = ResultSessionId;
  Event.Text = ResultText;
  Event.bSuccess = !bCancelled && !bTimedOut && Event.ExitCode == 0 &&
                   bSawResult && !bResultIsError;

  FPlatformProcess::CloseProc(ProcessHandle);
  ProcessHandle.Reset();
  FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
  StdoutRead = StdoutWrite = nullptr;

  bRunning = false;
  UE_LOG(LogUIWidgetToolPlugin, Log,
         TEXT("Claude run finished: exit %d, success %d, cancelled %d, "
              "timed out %d"),
         Event.ExitCode, Event.bSuccess, Event.bCancelled, Event.bTimedOut);

  FOnUIWTClaudeRunEvent Delegate = MoveTemp(OnEvent);
  OnEvent.Unbind();
  Delegate.ExecuteIfBound(Event);
}

#undef LOCTEXT_NAMESPACE
