#include "UIWTClaudeService.h"

#include "UIWTCheckpointTypes.h"
#include "UIWTClaudeRunner.h"
#include "Core/UIWTGeneratedBlueprints.h"
#include "UIWTLocalSettings.h"
#include "Core/UIWTNotify.h"
#include "UIWTToolset.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "UIWidgetToolPlugin.h"

#include "Editor.h"
#include "IModelContextProtocolModule.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "WidgetBlueprint.h"

#define LOCTEXT_NAMESPACE "FUIWTClaudeService"

namespace
{
  TUniquePtr<FUIWTClaudeService> GClaudeService;
}

// ---------------------------------------------------------------------------
// Lifetime

FUIWTClaudeService::FUIWTClaudeService()
{
  // The toolset registry is an editor subsystem, so it does not exist yet at
  // Default loading phase; register once the engine is up.
  if (GEditor)
  {
    RegisterToolset();
  }
  else
  {
    FCoreDelegates::GetOnPostEngineInit().AddRaw(
        this, &FUIWTClaudeService::RegisterToolset);
  }
}

FUIWTClaudeService::~FUIWTClaudeService()
{
  FCoreDelegates::GetOnPostEngineInit().RemoveAll(this);
  // Not CancelRun: the runner's Finished callback is bound raw to this
  // object, and the runner outlives Runner.Reset() while its drain thread
  // holds it. Abandon unbinds and stops the ticker before that can fire.
  if (Runner.IsValid())
  {
    Runner->Abandon();
  }
  Runner.Reset();
  DisconnectMcp();
  UnregisterToolset();
}

void FUIWTClaudeService::Create()
{
  check(!GClaudeService.IsValid());
  GClaudeService = MakeUnique<FUIWTClaudeService>();
}

void FUIWTClaudeService::Destroy()
{
  GClaudeService.Reset();
}

FUIWTClaudeService &FUIWTClaudeService::Get()
{
  check(GClaudeService.IsValid());
  return *GClaudeService;
}

FUIWTClaudeService *FUIWTClaudeService::TryGet()
{
  return GClaudeService.Get();
}

// ---------------------------------------------------------------------------
// Toolset registration

void FUIWTClaudeService::RegisterToolset()
{
  if (bToolsetRegistered || !GEditor)
  {
    return;
  }
  UToolsetRegistry::RegisterToolsetClass(UUIWTToolset::StaticClass());
  bToolsetRegistered = true;
  if (IModelContextProtocolModule *Mcp = IModelContextProtocolModule::Get())
  {
    Mcp->RefreshTools();
  }
}

void FUIWTClaudeService::UnregisterToolset()
{
  if (!bToolsetRegistered)
  {
    return;
  }
  UToolsetRegistry::UnregisterToolsetClass(UUIWTToolset::StaticClass());
  bToolsetRegistered = false;
}

// ---------------------------------------------------------------------------
// MCP server

FString FUIWTClaudeService::WriteMcpConfig(FText &OutError) const
{
  const int32 Port = UUIWTLocalSettings::Get()->McpServerPort;

  TSharedRef<FJsonObject> Server = MakeShared<FJsonObject>();
  Server->SetStringField(TEXT("type"), TEXT("http"));
  Server->SetStringField(
      TEXT("url"), FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), Port));
  TSharedRef<FJsonObject> Servers = MakeShared<FJsonObject>();
  Servers->SetObjectField(TEXT("unreal-mcp"), Server);
  TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
  Root->SetObjectField(TEXT("mcpServers"), Servers);

  FString Json;
  const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
  FJsonSerializer::Serialize(Root, Writer);

  const FString Path = FPaths::ConvertRelativePathToFull(
      FPaths::Combine(UIWT::GetSavePath(), TEXT("mcp.json")));
  if (!FFileHelper::SaveStringToFile(Json, *Path))
  {
    OutError = FText::Format(LOCTEXT("McpConfigWriteFailed",
                                     "Could not write {0}."),
                             FText::FromString(Path));
    return FString();
  }
  return Path;
}

bool FUIWTClaudeService::ConnectMcp(FText &OutError)
{
  if (bMcpConnected)
  {
    return true;
  }

  ClaudeExecutable = FUIWTClaudeRunner::FindExecutable(
      UUIWTLocalSettings::Get()->ClaudeExecutable.FilePath, OutError);
  if (ClaudeExecutable.IsEmpty())
  {
    return false;
  }

  if (WriteMcpConfig(OutError).IsEmpty())
  {
    return false;
  }

  IModelContextProtocolModule *Mcp = IModelContextProtocolModule::Get();
  if (!Mcp)
  {
    OutError = LOCTEXT("McpModuleMissing",
                       "The ModelContextProtocol plugin is not loaded.");
    return false;
  }
  RegisterToolset();
  Mcp->StartServer(static_cast<uint32>(UUIWTLocalSettings::Get()->McpServerPort));
  Mcp->RefreshTools();
  bMcpConnected = true;
  UE_LOG(LogUIWidgetToolPlugin, Log,
         TEXT("MCP server started on port %d; claude at %s"),
         UUIWTLocalSettings::Get()->McpServerPort, *ClaudeExecutable);
  return true;
}

void FUIWTClaudeService::DisconnectMcp()
{
  if (!bMcpConnected)
  {
    return;
  }
  CancelRun();
  if (IModelContextProtocolModule *Mcp = IModelContextProtocolModule::Get())
  {
    Mcp->StopServer();
  }
  bMcpConnected = false;
}

// ---------------------------------------------------------------------------
// Chat state and runs

FUIWTChatState &FUIWTClaudeService::GetChatState(const FGuid &InEntryId)
{
  if (const TSharedRef<FUIWTChatState> *Found = ChatStates.Find(InEntryId))
  {
    return **Found;
  }
  return *ChatStates.Add(InEntryId, MakeShared<FUIWTChatState>());
}

const FUIWTChatState *
FUIWTClaudeService::FindChatState(const FGuid &InEntryId) const
{
  const TSharedRef<FUIWTChatState> *Found = ChatStates.Find(InEntryId);
  return Found ? &Found->Get() : nullptr;
}

void FUIWTClaudeService::ForgetChatState(const FGuid &InEntryId)
{
  if (ChatStates.Remove(InEntryId) > 0)
  {
    ChatChanged.Broadcast();
  }
}

const FUIWTActiveRun *FUIWTClaudeService::GetActiveRun() const
{
  return ActiveRun.Get();
}

bool FUIWTClaudeService::IsRunInFlight() const
{
  return Runner.IsValid() && Runner->IsRunning();
}

void FUIWTClaudeService::AppendMessage(const FGuid &InEntryId,
                                       EUIWTChatRole InRole,
                                       const FString &InText)
{
  FUIWTChatMessage Message;
  Message.Role = InRole;
  Message.Text = InText;
  GetChatState(InEntryId).Messages.Add(MoveTemp(Message));
  ChatChanged.Broadcast();
}

// Keeps a single trailing "working..." line per run.
void FUIWTClaudeService::ReplaceStatus(const FGuid &InEntryId,
                                       const FString &InText)
{
  TArray<FUIWTChatMessage> &Messages = GetChatState(InEntryId).Messages;
  if (Messages.Num() > 0 && Messages.Last().Role == EUIWTChatRole::Status)
  {
    if (InText.IsEmpty())
    {
      Messages.Pop();
    }
    else
    {
      Messages.Last().Text = InText;
    }
    ChatChanged.Broadcast();
    return;
  }
  if (!InText.IsEmpty())
  {
    AppendMessage(InEntryId, EUIWTChatRole::Status, InText);
  }
}

bool FUIWTClaudeService::SetEntryNote(const FGuid &InEntryId,
                                    const FString &InNote)
{
  UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();
  FWidgetPreviewObject *Entry = Settings->FindWidgetPreviewObject(InEntryId);
  if (!Entry || Entry->IsOriginal())
  {
    return false;
  }
  Entry->Note = InNote;
  Settings->SaveWidgetPreviewObjects();
  EntriesChanged.Broadcast();
  return true;
}

bool FUIWTClaudeService::StartRun(const FGuid &InEntryId,
                                  const FString &InPrompt,
                                  const FUIWTRunContext &InContext,
                                  FText &OutError)
{
  if (IsRunInFlight())
  {
    OutError = LOCTEXT("RunInFlight", "A run is already in flight.");
    return false;
  }
  if (!bMcpConnected)
  {
    OutError = LOCTEXT("RunNotConnected", "Press Connect MCP first.");
    return false;
  }
  if (GEditor && GEditor->PlayWorld)
  {
    OutError = LOCTEXT("RunDuringPIE", "Stop Play In Editor first.");
    return false;
  }

  UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();
  const FWidgetPreviewObject *Entry =
      Settings->FindWidgetPreviewObject(InEntryId);
  if (!Entry)
  {
    OutError = LOCTEXT("RunNoEntry", "The entry no longer exists.");
    return false;
  }
  if (Entry->IsOriginal())
  {
    OutError = LOCTEXT("RunOriginal",
                       "Originals are never edited. Duplicate the entry first.");
    return false;
  }

  UWidgetBlueprint *Blueprint =
      UIWTGenerated::FindWidgetBlueprint(Entry->WidgetClass);
  if (!Blueprint)
  {
    OutError = LOCTEXT("RunNoBlueprint",
                       "The entry's blueprint copy could not be loaded.");
    return false;
  }

  // An editor open on the copy would be mutated underneath; close it,
  // saving unsaved work first so the run starts from what the user sees.
  if (UAssetEditorSubsystem *Editors =
          GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
  {
    if (Editors->FindEditorForAsset(Blueprint, false) &&
        Blueprint->GetOutermost()->IsDirty())
    {
      FText SaveError;
      if (!UIWTGenerated::SaveWidgetBlueprint(Blueprint, SaveError))
      {
        OutError = FText::Format(
            LOCTEXT("RunUnsavedCopy",
                    "The copy has unsaved changes that could not be saved "
                    "({0}). Save or revert it, then send again."),
            SaveError);
        return false;
      }
    }
    Editors->CloseAllEditorsForAsset(Blueprint);
  }

  const FWidgetPreviewObject *Root =
      Settings->FindWidgetPreviewObject(Entry->SourceEntryId);

  TUniquePtr<FUIWTActiveRun> Run = MakeUnique<FUIWTActiveRun>();
  Run->EntryId = InEntryId;
  Run->BlueprintPath = FSoftObjectPath(Blueprint);
  if (Root && !Root->WidgetClass.IsNull())
  {
    if (UWidgetBlueprint *RootBlueprint =
            UIWTGenerated::FindWidgetBlueprint(Root->WidgetClass))
    {
      Run->OriginalBlueprintPath = FSoftObjectPath(RootBlueprint);
    }
  }
  Run->LevelPackagePath = InContext.LevelPackagePath;
  Run->CheckpointDisplay = InContext.CheckpointDisplay;
  Run->PickedWidget = InContext.PickedWidget;

  FUIWTClaudeRunRequest Request;
  Request.Executable = ClaudeExecutable;
  // A resumed session already has the instructions in its context, so only
  // the turn that opens one carries them.
  const FString SessionId = GetChatState(InEntryId).SessionId;
  Request.Prompt = SessionId.IsEmpty()
                       ? UUIWTAgentSkill::GetInstructionsText() +
                             TEXT("\n\n---\nUser request:\n") + InPrompt
                       : InPrompt;
  Request.SessionId = SessionId;
  Request.WorkingDirectory =
      FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
  Request.McpConfigPath = WriteMcpConfig(OutError);
  Request.TimeoutSeconds =
      static_cast<float>(UUIWTLocalSettings::Get()->RunTimeoutSeconds);
  if (Request.McpConfigPath.IsEmpty())
  {
    return false;
  }

  Runner = MakeShared<FUIWTClaudeRunner>();
  ActiveRun = MoveTemp(Run);
  if (!Runner->Start(Request,
                     FOnUIWTClaudeRunEvent::CreateRaw(
                         this, &FUIWTClaudeService::OnRunEvent),
                     OutError))
  {
    ActiveRun.Reset();
    Runner.Reset();
    return false;
  }

  AppendMessage(InEntryId, EUIWTChatRole::User, InPrompt);
  ReplaceStatus(InEntryId, TEXT("working..."));
  return true;
}

void FUIWTClaudeService::CancelRun()
{
  if (Runner.IsValid() && Runner->IsRunning())
  {
    Runner->Cancel();
  }
}

void FUIWTClaudeService::OnRunEvent(const FUIWTClaudeRunEvent &InEvent)
{
  if (!ActiveRun.IsValid())
  {
    return;
  }
  const FGuid EntryId = ActiveRun->EntryId;

  switch (InEvent.Type)
  {
  case FUIWTClaudeRunEvent::EType::SessionStarted:
    GetChatState(EntryId).SessionId = InEvent.SessionId;
    break;
  case FUIWTClaudeRunEvent::EType::ToolCall:
    ReplaceStatus(EntryId,
                  FString::Printf(TEXT("working... %s"), *InEvent.Text));
    break;
  case FUIWTClaudeRunEvent::EType::Finished:
    FinishRun(InEvent);
    break;
  }
}

void FUIWTClaudeService::FinishRun(const FUIWTClaudeRunEvent &InEvent)
{
  const TUniquePtr<FUIWTActiveRun> Run = MoveTemp(ActiveRun);
  if (!Run.IsValid())
  {
    return;
  }
  const FGuid EntryId = Run->EntryId;
  ReplaceStatus(EntryId, FString());

  if (!InEvent.SessionId.IsEmpty())
  {
    GetChatState(EntryId).SessionId = InEvent.SessionId;
  }

  if (InEvent.bSuccess)
  {
    AppendMessage(EntryId, EUIWTChatRole::Assistant,
                  InEvent.Text.IsEmpty() ? TEXT("(no reply)") : InEvent.Text);
    EntriesChanged.Broadcast();
    return;
  }

  // Failed, cancelled or timed out: disk is the last good state.
  FString Reason;
  if (InEvent.bCancelled)
  {
    Reason = TEXT("Cancelled.");
  }
  else if (InEvent.bTimedOut)
  {
    Reason = TEXT("Timed out.");
  }
  else if (InEvent.ExitCode != 0)
  {
    Reason = FString::Printf(TEXT("Claude Code exited with code %d."),
                             InEvent.ExitCode);
  }
  else
  {
    Reason = TEXT("The run reported an error.");
  }
  if (!InEvent.Text.IsEmpty())
  {
    Reason += TEXT("\n") + InEvent.Text;
  }

  FText ReloadError;
  if (UWidgetBlueprint *Blueprint =
          Cast<UWidgetBlueprint>(Run->BlueprintPath.TryLoad()))
  {
    if (UIWTGenerated::ReloadWidgetBlueprint(Blueprint, ReloadError))
    {
      Reason += TEXT("\nThe copy was restored from disk.");
    }
    else
    {
      Reason += TEXT("\nThe copy could not be restored from disk: ") +
                ReloadError.ToString();
    }
  }

  AppendMessage(EntryId, EUIWTChatRole::Error, Reason);
  if (InEvent.ExitCode != 0 && !InEvent.bCancelled && !InEvent.bTimedOut)
  {
    UIWTNotify::Show(
        LOCTEXT("RunFailedToast",
                "The Claude run failed. If this is the first run, make sure "
                "you are logged in: run claude in a terminal."),
        false);
  }
  EntriesChanged.Broadcast();
}

#undef LOCTEXT_NAMESPACE
