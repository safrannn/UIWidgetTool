#include "UIWTClaudeService.h"

#include "UIWTCheckpointTypes.h"
#include "UIWTClaudeRunner.h"
#include "Core/UIWTGeneratedBlueprints.h"
#include "UIWTDevToolset.h"
#include "UIWTLocalSettings.h"
#include "UIWTPromptImage.h"
#include "Core/UIWTNotify.h"
#include "UIWTToolset.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "UIWidgetToolPlugin.h"

#include "Editor.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "IModelContextProtocolModule.h"
#include "IPAddress.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ModelContextProtocolServer.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Settings/EditorLoadingSavingSettings.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "WidgetBlueprint.h"

#define LOCTEXT_NAMESPACE "FUIWTClaudeService"

namespace
{
  TUniquePtr<FUIWTClaudeService> GClaudeService;

  // How long the port probe waits for a connection. Loopback connects in
  // well under a millisecond; a refused one never completes, so this is
  // also how long a free port takes to report.
  constexpr double PortProbeSeconds = 0.25;

  // True when something already accepts connections on 127.0.0.1:InPort,
  // which is the address Claude Code is told to use.
  bool IsLoopbackPortInUse(int32 InPort)
  {
    ISocketSubsystem *Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!Sockets)
    {
      return false;
    }
    FSocket *Socket = Sockets->CreateSocket(
        NAME_Stream, TEXT("UIWidgetTool MCP port probe"),
        FNetworkProtocolTypes::IPv4);
    if (!Socket)
    {
      return false;
    }
    const TSharedRef<FInternetAddr> Address =
        Sockets->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
    Address->SetLoopbackAddress();
    Address->SetPort(InPort);
    Socket->SetNonBlocking(true);
    Socket->Connect(*Address);
    const bool bInUse =
        Socket->Wait(ESocketWaitConditions::WaitForWrite,
                     FTimespan::FromSeconds(PortProbeSeconds)) &&
        Socket->GetConnectionState() == SCS_Connected;
    Socket->Close();
    Sockets->DestroySocket(Socket);
    return bInUse;
  }
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
  ResumeAutoCreateAssets();
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
  UToolsetRegistry::RegisterToolsetClass(UUIWTDevToolset::StaticClass());
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
  UToolsetRegistry::UnregisterToolsetClass(UUIWTDevToolset::StaticClass());
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
  const int32 Port = UUIWTLocalSettings::Get()->McpServerPort;
  const FModelContextProtocolServer *Server = Mcp->GetServer();
  const bool bAlreadyOurs = Server && Server->IsServerRunning() &&
                            Server->GetServerPort() == static_cast<uint32>(Port);
  // StartServer cannot report a port that is taken: the HTTP server logs the
  // bind failure and carries on, and Claude would then talk to whatever else
  // is listening there.
  if (!bAlreadyOurs && IsLoopbackPortInUse(Port))
  {
    OutError = FText::Format(
        LOCTEXT("McpPortInUse",
                "Port {0} is already in use by another program. Choose "
                "another MCP Server Port in UI Widget Tool (local) settings."),
        FText::AsNumber(Port, &FNumberFormattingOptions::DefaultNoGrouping()));
    return false;
  }
  RegisterToolset();
  Mcp->StartServer(static_cast<uint32>(Port));
  Server = Mcp->GetServer();
  if (!Server || !Server->IsServerRunning())
  {
    Mcp->StopServer();
    OutError = LOCTEXT("McpStartFailed",
                       "The MCP server could not be started; see the Output "
                       "Log (LogModelContextProtocol).");
    return false;
  }
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
  // A run still in flight keeps using its files. Otherwise the reference,
  // renders and zooms go; assets and anything else in the widget's folder
  // stay, and deleting a generated widget removes its folder anyway.
  if (!ActiveRun.IsValid() || ActiveRun->EntryId != InEntryId)
  {
    IFileManager &Files = IFileManager::Get();
    Files.DeleteDirectory(*GetLegacyRunDirectory(InEntryId), false, true);
    const FUIWTChatState *State = FindChatState(InEntryId);
    if (State && !State->RunDirectory.IsEmpty())
    {
      for (const TCHAR *Pattern :
           {TEXT("reference.png"), TEXT("render_*.png"), TEXT("compare_*.png"),
            TEXT("zoom_*.png")})
      {
        TArray<FString> Names;
        Files.FindFiles(Names, *(State->RunDirectory / Pattern), true, false);
        for (const FString &Name : Names)
        {
          Files.Delete(*(State->RunDirectory / Name), false, true, true);
        }
      }
    }
  }
  if (ChatStates.Remove(InEntryId) > 0)
  {
    ChatChanged.Broadcast();
  }
}

const FUIWTActiveRun *FUIWTClaudeService::GetActiveRun() const
{
  return ActiveRun.Get();
}

FUIWTActiveRun *FUIWTClaudeService::GetActiveRunMutable()
{
  return ActiveRun.Get();
}

FString FUIWTClaudeService::GetLegacyRunDirectory(const FGuid &InEntryId)
{
  return FPaths::ConvertRelativePathToFull(
      FPaths::Combine(UIWT::GetSavePath(), TEXT("Runs"),
                      InEntryId.ToString(EGuidFormats::Digits)));
}

bool FUIWTClaudeService::MoveBlueprintToOwnFolder(UWidgetBlueprint *InBlueprint,
                                                  FText &OutError)
{
  const TSoftClassPtr<UUserWidget> OldClass(InBlueprint->GeneratedClass);
  bool bMoved = false;
  if (!UIWTGenerated::MoveToOwnFolder(InBlueprint, bMoved, OutError))
  {
    return false;
  }
  if (!bMoved || !InBlueprint->GeneratedClass)
  {
    return true;
  }
  // Entries may share one blueprint; repoint every entry that used it.
  UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();
  for (FWidgetPreviewObject &Entry : Settings->WidgetPreviewObjects)
  {
    if (Entry.WidgetClass == OldClass)
    {
      Entry.WidgetClass = TSoftClassPtr<UUserWidget>(InBlueprint->GeneratedClass);
    }
  }
  Settings->SaveWidgetPreviewObjects();
  EntriesChanged.Broadcast();
  return true;
}

bool FUIWTClaudeService::PrepareRunDirectory(
    FUIWTActiveRun &InOutRun, const TSharedPtr<const FUIWTPromptImage> &InImage,
    FText &OutError)
{
  IFileManager &Files = IFileManager::Get();
  InOutRun.ContentFolder = FPackageName::GetLongPackagePath(
      InOutRun.BlueprintPath.GetLongPackageName());
  InOutRun.RunDirectory = UIWTGenerated::GetFolderOnDisk(InOutRun.ContentFolder);
  if (InOutRun.RunDirectory.IsEmpty() ||
      !Files.MakeDirectory(*InOutRun.RunDirectory, true))
  {
    OutError = FText::Format(LOCTEXT("RunDirFailed", "Could not create {0}."),
                             FText::FromString(InOutRun.RunDirectory.IsEmpty()
                                                   ? InOutRun.ContentFolder
                                                   : InOutRun.RunDirectory));
    return false;
  }
  GetChatState(InOutRun.EntryId).RunDirectory = InOutRun.RunDirectory;
  if (!UIWTGenerated::IsGeneratedPath(InOutRun.ContentFolder))
  {
    PauseAutoCreateAssets();
  }

  // An earlier run's reference and files, from before widgets had their own
  // folder.
  const FString LegacyDirectory = GetLegacyRunDirectory(InOutRun.EntryId);
  if (Files.DirectoryExists(*LegacyDirectory))
  {
    TArray<FString> Names;
    Files.FindFiles(Names, *(LegacyDirectory / TEXT("*")), true, false);
    for (const FString &Name : Names)
    {
      if (!Files.FileExists(*(InOutRun.RunDirectory / Name)))
      {
        Files.Move(*(InOutRun.RunDirectory / Name), *(LegacyDirectory / Name),
                   false, true);
      }
    }
    Files.DeleteDirectory(*LegacyDirectory, false, true);
  }

  // Renders and zooms describe a blueprint that may have changed since; the
  // reference and files Claude wrote itself stay.
  for (const TCHAR *Pattern :
       {TEXT("render_*.png"), TEXT("compare_*.png"), TEXT("zoom_*.png")})
  {
    TArray<FString> Stale;
    Files.FindFiles(Stale, *(InOutRun.RunDirectory / Pattern), true, false);
    for (const FString &Name : Stale)
    {
      Files.Delete(*(InOutRun.RunDirectory / Name), false, true, true);
    }
  }

  const FString ReferencePath = InOutRun.RunDirectory / TEXT("reference.png");
  if (InImage.IsValid() && InImage->SourcePng.Num() > 0 &&
      !FFileHelper::SaveArrayToFile(InImage->SourcePng, *ReferencePath))
  {
    OutError = FText::Format(LOCTEXT("RunReferenceFailed",
                                     "Could not write {0}."),
                             FText::FromString(ReferencePath));
    return false;
  }
  InOutRun.ReferenceImagePath =
      Files.FileExists(*ReferencePath) ? ReferencePath : FString();
  return true;
}

void FUIWTClaudeService::RestoreRunTextures(const FUIWTActiveRun &InRun,
                                            FString &OutReason)
{
  TArray<UObject *> Created;
  TArray<UPackage *> Changed;
  for (const FSoftObjectPath &Path : InRun.Textures)
  {
    UTexture2D *Texture = Cast<UTexture2D>(Path.ResolveObject());
    if (!Texture || !Texture->GetOutermost()->IsDirty())
    {
      continue;
    }
    if (FPackageName::DoesPackageExist(Texture->GetOutermost()->GetName()))
    {
      Changed.Add(Texture->GetOutermost());
    }
    else
    {
      Created.Add(Texture);
    }
  }
  if (Created.Num() > 0)
  {
    const int32 Deleted = ObjectTools::ForceDeleteObjects(Created, false);
    OutReason += FString::Printf(
        TEXT("\nDeleted %d of the %d unsaved textures the run created."),
        Deleted, Created.Num());
  }
  if (Changed.Num() > 0)
  {
    FText ReloadError;
    if (UPackageTools::ReloadPackages(
            Changed, ReloadError, EReloadPackagesInteractionMode::AssumePositive))
    {
      OutReason += FString::Printf(
          TEXT("\nRestored %d changed textures from disk."), Changed.Num());
    }
    else
    {
      OutReason += TEXT("\nChanged textures could not be restored from disk: ") +
                   ReloadError.ToString();
    }
  }
}

bool FUIWTClaudeService::IsRunInFlight() const
{
  return Runner.IsValid() && Runner->IsRunning();
}

void FUIWTClaudeService::AppendMessage(
    const FGuid &InEntryId, EUIWTChatRole InRole, const FString &InText,
    TSharedPtr<const FUIWTPromptImage> InImage)
{
  FUIWTChatMessage Message;
  Message.Role = InRole;
  Message.Text = InText;
  Message.Image = MoveTemp(InImage);
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

bool FUIWTClaudeService::StartRun(
    const FGuid &InEntryId, const FString &InPrompt,
    const TSharedPtr<const FUIWTPromptImage> &InImage,
    const FUIWTRunContext &InContext, FText &OutError)
{
  if (InPrompt.IsEmpty() && !InImage.IsValid())
  {
    OutError = LOCTEXT("RunEmptyPrompt", "Type a prompt or attach an image.");
    return false;
  }
  if (IsRunInFlight())
  {
    OutError = LOCTEXT("RunInFlight", "A run is already in flight.");
    return false;
  }
  if (!bMcpConnected)
  {
    OutError = LOCTEXT("RunNotConnected", "Connection to MCP required.");
    return false;
  }
  if (GEditor && GEditor->PlayWorld)
  {
    OutError = LOCTEXT("RunDuringPIE", "Stop Play In Editor first.");
    return false;
  }

  UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();
  FWidgetPreviewObject *Entry = Settings->FindWidgetPreviewObject(InEntryId);
  if (!Entry)
  {
    OutError = LOCTEXT("RunNoEntry", "The entry no longer exists.");
    return false;
  }

  // An entry without a widget gets an empty one to build in.
  UWidgetBlueprint *Blueprint = nullptr;
  if (Entry->WidgetClass.IsNull())
  {
    FText CreateError;
    Blueprint = UIWTGenerated::CreateEmptyWidgetBlueprint(CreateError);
    if (!Blueprint)
    {
      OutError = FText::Format(
          LOCTEXT("RunCreateBlueprintFailed",
                  "Could not create a widget for the entry: {0}"),
          CreateError);
      return false;
    }
    Entry->WidgetClass = TSoftClassPtr<UUserWidget>(Blueprint->GeneratedClass);
    Entry->WidgetName = Blueprint->GetName();
    Settings->SaveWidgetPreviewObjects();
    EntriesChanged.Broadcast();
  }
  else
  {
    Blueprint = UIWTGenerated::FindWidgetBlueprint(Entry->WidgetClass);
  }
  if (!Blueprint)
  {
    OutError = LOCTEXT("RunNoBlueprint",
                       "The entry's blueprint could not be loaded.");
    return false;
  }

  // An editor open on the blueprint would be mutated underneath; close it,
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
            LOCTEXT("RunUnsavedBlueprint",
                    "The blueprint has unsaved changes that could not be saved "
                    "({0}). Save or revert it, then send again."),
            SaveError);
        return false;
      }
    }
    Editors->CloseAllEditorsForAsset(Blueprint);
  }
  if (!MoveBlueprintToOwnFolder(Blueprint, OutError))
  {
    return false;
  }

  TUniquePtr<FUIWTActiveRun> Run = MakeUnique<FUIWTActiveRun>();
  Run->EntryId = InEntryId;
  Run->BlueprintPath = FSoftObjectPath(Blueprint);
  Run->LevelPackagePath = InContext.LevelPackagePath;
  Run->CheckpointDisplay = InContext.CheckpointDisplay;
  Run->PickedWidget = InContext.PickedWidget;
  if (!PrepareRunDirectory(*Run, InImage, OutError))
  {
    ResumeAutoCreateAssetsLater();
    return false;
  }
  const UUIWTLocalSettings *LocalSettings = UUIWTLocalSettings::Get();

  FUIWTClaudeRunRequest Request;
  Request.Executable = ClaudeExecutable;
  // A resumed session already has the instructions in its context, so only
  // the turn that opens one carries them.
  const FString SessionId = GetChatState(InEntryId).SessionId;
  FString UserRequest =
      InPrompt.IsEmpty() ? FString(TEXT("See the attached image.")) : InPrompt;
  if (InImage.IsValid() && InImage->Size.X > 0)
  {
    // Claude measures in the pixels it receives; say what they map to.
    UserRequest += FString::Printf(
        TEXT("\n\n(Attached image: %d x %d pixels"), InImage->Size.X,
        InImage->Size.Y);
    if (InImage->SourceSize != InImage->Size)
    {
      UserRequest += FString::Printf(
          TEXT(", shrunk from the original %d x %d: multiply what you "
               "measure by %.3f to get design pixels"),
          InImage->SourceSize.X, InImage->SourceSize.Y,
          static_cast<double>(InImage->SourceSize.X) / InImage->Size.X);
    }
    UserRequest += FString::Printf(
        TEXT(". The full-resolution original, %d x %d, is saved as %s; the "
             "image tools take its pixel coordinates.)"),
        InImage->SourceSize.X, InImage->SourceSize.Y,
        *Run->ReferenceImagePath);
  }
  // Every turn, since the setting can change between turns of a session.
  UserRequest += UUIWTAgentSkill::GetAccessText(LocalSettings->PermissionMode,
                                                Run->RunDirectory);
  const FString &RolledBackReason = GetChatState(InEntryId).RolledBackReason;
  if (!SessionId.IsEmpty() && !RolledBackReason.IsEmpty())
  {
    UserRequest = FString::Printf(
                      TEXT("(Your previous turn did not finish: %s The editor "
                           "discarded everything it had not saved and "
                           "restored the blueprint and its textures from "
                           "disk, so edits you made in that turn after the "
                           "last SaveWidgetBlueprint are gone. Export the "
                           "tree again before editing.)\n\n"),
                      *RolledBackReason) +
                  UserRequest;
  }
  Request.Prompt = SessionId.IsEmpty()
                       ? UUIWTAgentSkill::GetInstructionsText() +
                             TEXT("\n\n---\nUser request:\n") + UserRequest
                       : UserRequest;
  Request.Image = InImage;
  Request.SessionId = SessionId;
  Request.WorkingDirectory =
      FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
  Request.McpConfigPath = WriteMcpConfig(OutError);
  Request.PermissionMode = LocalSettings->GetPermissionModeArgument();
  Request.Model = LocalSettings->GetModelArgument();
  Request.Effort = LocalSettings->GetEffortArgument();
  Request.TimeoutSeconds =
      static_cast<float>(LocalSettings->RunTimeoutSeconds);
  if (Request.McpConfigPath.IsEmpty())
  {
    ResumeAutoCreateAssetsLater();
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
    ResumeAutoCreateAssetsLater();
    return false;
  }

  // Told once; a failure of this turn sets it again.
  GetChatState(InEntryId).RolledBackReason.Reset();
  AppendMessage(InEntryId, EUIWTChatRole::User, InPrompt, InImage);
  ReplaceStatus(InEntryId, TEXT("working..."));
  return true;
}

bool FUIWTClaudeService::DevBeginRun(
    const FGuid &InEntryId, UWidgetBlueprint *InBlueprint,
    const TSharedPtr<const FUIWTPromptImage> &InImage, FText &OutError)
{
  if (IsRunInFlight() || ActiveRun.IsValid())
  {
    OutError = LOCTEXT("DevRunBusy", "A run is already active.");
    return false;
  }
  if (!InBlueprint || !MoveBlueprintToOwnFolder(InBlueprint, OutError))
  {
    return false;
  }
  TUniquePtr<FUIWTActiveRun> Run = MakeUnique<FUIWTActiveRun>();
  Run->EntryId = InEntryId;
  Run->BlueprintPath = FSoftObjectPath(InBlueprint);
  if (!PrepareRunDirectory(*Run, InImage, OutError))
  {
    ResumeAutoCreateAssetsLater();
    return false;
  }
  ActiveRun = MoveTemp(Run);
  return true;
}

void FUIWTClaudeService::DevEndRun(bool bInSuccess)
{
  if (IsRunInFlight())
  {
    return;
  }
  FUIWTClaudeRunEvent Event;
  Event.Type = FUIWTClaudeRunEvent::EType::Finished;
  Event.bSuccess = bInSuccess;
  Event.bCancelled = !bInSuccess;
  Event.ExitCode = 0;
  Event.Text = TEXT("Ended by DevEndRun.");
  FinishRun(Event);
}

void FUIWTClaudeService::PauseAutoCreateAssets()
{
  if (ResumeAutoCreateHandle.IsValid())
  {
    FTSTicker::GetCoreTicker().RemoveTicker(ResumeAutoCreateHandle);
    ResumeAutoCreateHandle.Reset();
  }
  UEditorLoadingSavingSettings *Settings =
      GetMutableDefault<UEditorLoadingSavingSettings>();
  if (!bAutoCreatePaused && Settings->bAutoCreateAssets)
  {
    Settings->bAutoCreateAssets = false;
    bAutoCreatePaused = true;
  }
}

void FUIWTClaudeService::ResumeAutoCreateAssetsLater()
{
  if (!bAutoCreatePaused || ResumeAutoCreateHandle.IsValid())
  {
    return;
  }
  // Files are imported once unchanged for the threshold; the margin covers
  // the monitor's own polling.
  const float Delay =
      GetDefault<UEditorLoadingSavingSettings>()->AutoReimportThreshold + 5.f;
  ResumeAutoCreateHandle = FTSTicker::GetCoreTicker().AddTicker(
      FTickerDelegate::CreateLambda(
          [this](float)
          {
            ResumeAutoCreateHandle.Reset();
            ResumeAutoCreateAssets();
            return false;
          }),
      Delay);
}

void FUIWTClaudeService::ResumeAutoCreateAssets()
{
  if (ResumeAutoCreateHandle.IsValid())
  {
    FTSTicker::GetCoreTicker().RemoveTicker(ResumeAutoCreateHandle);
    ResumeAutoCreateHandle.Reset();
  }
  if (bAutoCreatePaused)
  {
    GetMutableDefault<UEditorLoadingSavingSettings>()->bAutoCreateAssets = true;
    bAutoCreatePaused = false;
  }
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
  ResumeAutoCreateAssetsLater();

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
  GetChatState(EntryId).RolledBackReason = Reason;
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
      Reason += TEXT("\nThe blueprint was restored from disk.");
    }
    else
    {
      Reason += TEXT("\nThe blueprint could not be restored from disk: ") +
                ReloadError.ToString();
    }
  }
  // After the blueprint, which no longer references them.
  RestoreRunTextures(*Run, Reason);

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
