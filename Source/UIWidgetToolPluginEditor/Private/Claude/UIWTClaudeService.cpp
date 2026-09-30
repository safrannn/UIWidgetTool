#include "UIWTClaudeService.h"

#include "UIWTCheckpointTypes.h"
#include "UIWTClaudeRunner.h"
#include "Core/UIWTGeneratedBlueprints.h"
#include "Core/UIWTDesignImport.h"
#include "Design/UIWTDesignRefine.h"
#include "Design/UIWTDesignToolset.h"
#include "UIWTDevToolset.h"
#include "Core/UIWTLocalSettings.h"
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
  UToolsetRegistry::RegisterToolsetClass(UUIWTDesignToolset::StaticClass());
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
  UToolsetRegistry::UnregisterToolsetClass(UUIWTDesignToolset::StaticClass());
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
    bool bInImageIsReference, FText &OutError)
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
  if (bInImageIsReference && InImage.IsValid() && InImage->SourcePng.Num() > 0 &&
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
  return StartRunInternal(InEntryId, InPrompt, InImage, InContext, FRunOptions(), OutError);
}

bool FUIWTClaudeService::StartDesignRefine(const FGuid &InEntryId,
                                           const FUIWTRefineStart &InStart,
                                           const FUIWTRunContext &InContext, FText &OutError)
{
  const UUIWTLocalSettings *LocalSettings = UUIWTLocalSettings::Get();
  FRunOptions Options;
  Options.Request = InStart.Request;
  Options.Model = LocalSettings->GetRefineModelArgument();
  Options.Effort = LocalSettings->GetRefineEffortArgument();
  Options.bNewSession = true;
  // The blueprint's reference.png stays the run's reference; the request
  // says what the attached image (maybe a crop) is.
  Options.bImageIsNotReference = true;
  Options.Refine = &InStart;
  return StartRunInternal(InEntryId, InStart.DisplayText, InStart.Image, InContext, Options,
                          OutError);
}

FString FUIWTClaudeService::RecordRefineUsage(const FUIWTActiveRun &InRun,
                                              const FUIWTClaudeRunEvent &InEvent)
{
  const FUIWTRunUsage &Usage = InEvent.Usage;
  // Without Claude Code's figures (a cancelled run, a test's run with no
  // Claude process) there's nothing to compare the estimate with.
  if (!Usage.bValid)
  {
    return FString();
  }
  auto Tokens = [](int64 InTokens)
  {
    return InTokens >= 1000000 ? FString::Printf(TEXT("%.1fM"), InTokens / 1e6)
                               : FString::Printf(TEXT("%lldK"), (InTokens + 500) / 1000);
  };
  auto Dollars = [](double InDollars)
  { return InDollars >= 0.0 ? FString::Printf(TEXT("$%.2f"), InDollars) : FString(TEXT("-")); };

  // import-tree.md → Build and test → Cost estimate check: runs compared
  // with their estimates, to adjust the per-node constants.
  const FString File = FPaths::ConvertRelativePathToFull(
      FPaths::Combine(UIWT::GetSavePath(), TEXT("AIPassUsage.csv")));
  FString Row;
  if (!IFileManager::Get().FileExists(*File))
  {
    Row = TEXT("time,blueprint,source,scope,nodes,rounds,model,success,estimated_read,"
               "estimated_cached,estimated_written,estimated_usd,input,cache_creation,"
               "cache_read,output,usd\n");
  }
  Row += FString::Printf(
      TEXT("%s,%s,%s,\"%s\",%d,%d,%s,%d,%lld,%lld,%lld,%.4f,%lld,%lld,%lld,%lld,%.4f\n"),
      *FDateTime::UtcNow().ToIso8601(), *InRun.BlueprintPath.GetAssetName(), *InRun.RefineSource,
      *InRun.RefineScope.Replace(TEXT("\""), TEXT("'")), InRun.RefineNodes, InRun.RefineRounds,
      InRun.RefineModel.IsEmpty() ? TEXT("default") : *InRun.RefineModel,
      InEvent.bSuccess ? 1 : 0, InRun.EstimatedRead, InRun.EstimatedCached,
      InRun.EstimatedWritten, InRun.EstimatedDollars, Usage.InputTokens,
      Usage.CacheCreationTokens, Usage.CacheReadTokens, Usage.OutputTokens, Usage.CostUsd);
  FFileHelper::SaveStringToFile(Row, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
                                &IFileManager::Get(), FILEWRITE_Append);

  const int64 Read = Usage.InputTokens + Usage.CacheCreationTokens + Usage.CacheReadTokens;
  return FString::Printf(
      TEXT("(Used, as Claude Code reports it: %s tokens read, %s of them from the prompt "
           "cache, %s written; %s at API rates. Estimated: %s read, %s from the cache, %s "
           "written; %s.)"),
      *Tokens(Read), *Tokens(Usage.CacheReadTokens), *Tokens(Usage.OutputTokens),
      *Dollars(Usage.CostUsd), *Tokens(InRun.EstimatedRead + InRun.EstimatedCached),
      *Tokens(InRun.EstimatedCached), *Tokens(InRun.EstimatedWritten),
      *Dollars(InRun.EstimatedDollars));
}

bool FUIWTClaudeService::StartRunInternal(
    const FGuid &InEntryId, const FString &InPrompt,
    const TSharedPtr<const FUIWTPromptImage> &InImage,
    const FUIWTRunContext &InContext, const FRunOptions &InOptions, FText &OutError)
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

  // An entry without a widget gets an empty one to build in, except in an
  // image-reading run, whose first WriteDesignTree makes it.
  UWidgetBlueprint *Blueprint = nullptr;
  if (!InOptions.ImageRead && Entry->WidgetClass.IsNull())
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
  else if (!InOptions.ImageRead)
  {
    Blueprint = UIWTGenerated::FindWidgetBlueprint(Entry->WidgetClass);
  }
  if (!Blueprint && !InOptions.ImageRead)
  {
    OutError = LOCTEXT("RunNoBlueprint",
                       "The entry's blueprint could not be loaded.");
    return false;
  }

  // An editor open on the blueprint would be mutated underneath; close it,
  // saving unsaved work first so the run starts from what the user sees.
  UAssetEditorSubsystem *Editors =
      Blueprint ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
  if (Editors)
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
  if (Blueprint && !MoveBlueprintToOwnFolder(Blueprint, OutError))
  {
    return false;
  }

  TUniquePtr<FUIWTActiveRun> Run = MakeUnique<FUIWTActiveRun>();
  Run->EntryId = InEntryId;
  if (Blueprint)
  {
    Run->BlueprintPath = FSoftObjectPath(Blueprint);
  }
  Run->LevelPackagePath = InContext.LevelPackagePath;
  Run->CheckpointDisplay = InContext.CheckpointDisplay;
  Run->PickedWidget = InContext.PickedWidget;
  if (const FUIWTRefineStart *Refine = InOptions.Refine)
  {
    Run->bDesignRefine = true;
    Run->WidgetsBefore = UIWTDesignRefine::SnapshotWidgets(Blueprint);
    Run->bRefineChangedParts = Refine->bChangedParts;
    Run->RefineSource = Refine->Source;
    Run->RefineScope = Refine->Scope;
    Run->RefineModel = InOptions.Model;
    Run->RefineNodes = Refine->Nodes;
    Run->RefineRounds = Refine->Rounds;
    const UUIWTLocalSettings *PassSettings = UUIWTLocalSettings::Get();
    const UIWTDesignRefine::FEstimate Estimate = UIWTDesignRefine::Estimate(
        Refine->Nodes, Refine->Rounds,
        UIWTDesignRefine::PricesFor(PassSettings->RefineModel, PassSettings->RefineCustomModel));
    Run->EstimatedRead = Estimate.InputTokens;
    Run->EstimatedCached = Estimate.CachedTokens;
    Run->EstimatedWritten = Estimate.OutputTokens;
    Run->EstimatedDollars = Estimate.Dollars;
  }
  if (InOptions.ImageRead)
  {
    InitImageRead(*Run, *InOptions.ImageRead, InOptions.Model);
    GetChatState(InEntryId).RunDirectory = Run->RunDirectory;
  }
  else if (!PrepareRunDirectory(*Run, InImage, !InOptions.bImageIsNotReference, OutError))
  {
    ResumeAutoCreateAssetsLater();
    return false;
  }
  const UUIWTLocalSettings *LocalSettings = UUIWTLocalSettings::Get();

  FUIWTClaudeRunRequest Request;
  Request.Executable = ClaudeExecutable;
  // A resumed session already has the instructions in its context, so only
  // the turn that opens one carries them.
  const FString SessionId =
      InOptions.bNewSession ? FString() : GetChatState(InEntryId).SessionId;
  FString UserRequest =
      !InOptions.Request.IsEmpty() ? InOptions.Request
      : InPrompt.IsEmpty()         ? FString(TEXT("See the attached image."))
                                   : InPrompt;
  if (InImage.IsValid() && InImage->Size.X > 0 && !InOptions.bImageIsNotReference)
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
  Request.Model =
      InOptions.Model.IsEmpty() ? LocalSettings->GetModelArgument() : InOptions.Model;
  Request.Effort =
      InOptions.Effort.IsEmpty() ? LocalSettings->GetEffortArgument() : InOptions.Effort;
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

  // Told once; a failure of this turn sets it again. A new session starts
  // clean, and the runner reports its id.
  GetChatState(InEntryId).RolledBackReason.Reset();
  if (InOptions.bNewSession)
  {
    GetChatState(InEntryId).SessionId.Reset();
  }
  AppendMessage(InEntryId, EUIWTChatRole::User, InPrompt, InImage);
  ReplaceStatus(InEntryId, TEXT("working..."));
  return true;
}

bool FUIWTClaudeService::StartImageRead(const FGuid &InEntryId,
                                        const FUIWTImageReadStart &InStart,
                                        const FUIWTRunContext &InContext, FText &OutError)
{
  const UUIWTLocalSettings *LocalSettings = UUIWTLocalSettings::Get();
  FRunOptions Options;
  Options.Request = InStart.Request;
  Options.Model = LocalSettings->GetRefineModelArgument();
  Options.Effort = LocalSettings->GetRefineEffortArgument();
  Options.bNewSession = true;
  Options.bImageIsNotReference = true;
  Options.ImageRead = &InStart;
  return StartRunInternal(InEntryId, InStart.DisplayText, InStart.Image, InContext, Options,
                          OutError);
}

void FUIWTClaudeService::InitImageRead(FUIWTActiveRun &InOutRun, const FUIWTImageReadStart &InStart,
                                       const FString &InModel)
{
  InOutRun.bImageRead = true;
  InOutRun.ImageCacheDir = InStart.CacheDir;
  InOutRun.ImageSourceFile = InStart.SourceFile;
  InOutRun.ImageCrc = InStart.Crc;
  InOutRun.ImageTargetFolder = InStart.TargetFolder;
  InOutRun.ImageBlueprintName = InStart.BlueprintName;
  InOutRun.ImageScale = InStart.Scale;
  InOutRun.bImageSave = InStart.bSave;
  // The run's own files go to the cache folder until there's a blueprint,
  // and its reference is the cached image.
  InOutRun.RunDirectory = InStart.CacheDir;
  InOutRun.ReferenceImagePath = InStart.CacheDir / TEXT("reference.png");
  // Renames are synced and usage recorded as for a design pass.
  InOutRun.bDesignRefine = true;
  InOutRun.RefineSource = TEXT("image");
  InOutRun.RefineScope = InStart.bAIPass ? TEXT("reading + pass") : TEXT("reading");
  InOutRun.RefineModel = InModel;
  InOutRun.RefineNodes = InStart.Nodes;
  InOutRun.RefineRounds = InStart.Rounds;
  const UUIWTLocalSettings *Settings = UUIWTLocalSettings::Get();
  const UIWTDesignRefine::FEstimate Estimate = UIWTDesignRefine::EstimateReading(
      InStart.Nodes, InStart.Rounds,
      UIWTDesignRefine::PricesFor(Settings->RefineModel, Settings->RefineCustomModel),
      InStart.bAIPass);
  InOutRun.EstimatedRead = Estimate.InputTokens;
  InOutRun.EstimatedCached = Estimate.CachedTokens;
  InOutRun.EstimatedWritten = Estimate.OutputTokens;
  InOutRun.EstimatedDollars = Estimate.Dollars;
}

bool FUIWTClaudeService::DevBeginImageRead(const FGuid &InEntryId,
                                           const FUIWTImageReadStart &InStart, FText &OutError)
{
  if (IsRunInFlight() || ActiveRun.IsValid())
  {
    OutError = LOCTEXT("DevImageRunBusy", "A run is already active.");
    return false;
  }
  TUniquePtr<FUIWTActiveRun> Run = MakeUnique<FUIWTActiveRun>();
  Run->EntryId = InEntryId;
  InitImageRead(*Run, InStart, FString());
  ActiveRun = MoveTemp(Run);
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
  if (!PrepareRunDirectory(*Run, InImage, true, OutError))
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
  // What an AI pass used, next to its estimate, whether or not it worked.
  const FString Usage = Run->bDesignRefine ? RecordRefineUsage(*Run, InEvent) : FString();

  if (InEvent.bSuccess)
  {
    FString Reply = InEvent.Text.IsEmpty() ? TEXT("(no reply)") : InEvent.Text;
    // The pass's renames go into the design sidecar, so re-imports keep
    // them.
    UWidgetBlueprint *Blueprint =
        Run->bDesignRefine ? Cast<UWidgetBlueprint>(Run->BlueprintPath.TryLoad()) : nullptr;
    if (Blueprint)
    {
      int32 Renamed = 0;
      FString SyncError;
      if (!UIWTDesignRefine::SyncRenames(Blueprint, Run->WidgetsBefore, Renamed, SyncError))
      {
        Reply += TEXT("\n\n(The renames couldn't be written into the design sidecar, so a "
                      "re-import will treat renamed widgets as new: ") +
                 SyncError + TEXT(")");
      }
      else if (Renamed > 0)
      {
        Reply += FString::Printf(
            TEXT("\n\n(%d renames written into the design sidecar; re-imports keep them.)"),
            Renamed);
      }
      // The changed parts are done.
      FString ClearError;
      if (Run->bRefineChangedParts &&
          !UIWTDesignImport::ClearChangedNodes(Blueprint->GetOutermost()->GetName(), ClearError))
      {
        UE_LOG(LogUIWidgetToolPlugin, Warning, TEXT("%s"), *ClearError);
      }
    }
    if (!Usage.IsEmpty())
    {
      Reply += TEXT("\n\n") + Usage;
    }
    AppendMessage(EntryId, EUIWTChatRole::Assistant, Reply);
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
  if (Run->bImageRead && Run->BlueprintPath.IsNull())
  {
    // Nothing was imported: the entry stays without a blueprint.
    IFileManager::Get().DeleteDirectory(*Run->ImageCacheDir, false, true);
    Reason += TEXT("\nNothing was imported; the entry has no blueprint.");
  }
  if (!Usage.IsEmpty())
  {
    Reason += TEXT("\n") + Usage;
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
