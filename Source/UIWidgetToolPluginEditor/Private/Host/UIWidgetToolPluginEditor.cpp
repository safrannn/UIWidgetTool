#include "UIWidgetToolPluginEditor.h"

#include "Claude/SUIWTChatSection.h"
#include "SnapshotViewer/SUIWTSnapshotViewer.h"
#include "Manager/SUIWidgetManager.h"
#include "UIWTCheckpointCapture.h"
#include "UIWTCheckpointCodec.h"
#include "UIWTCheckpointRestoreSubsystem.h"
#include "UIWTCheckpointTypes.h"
#include "Claude/UIWTClaudeService.h"
#include "UIWTCommands.h"
#include "Core/UIWTGeneratedBlueprints.h"
#include "UIWTInputProcessor.h"
#include "Core/UIWTNotify.h"
#include "Manager/UIWTPlayLauncher.h"
#include "SnapshotViewer/UIWTWidgetSnapshot.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "UIWidgetToolPluginStyle.h"

#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/Docking/SDockingTabStack.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "Misc/CoreDelegates.h"
#include "UIWidgetToolPlugin.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SSplitter.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "FUIWidgetToolPluginEditorModule"

const FName
    FUIWidgetToolPluginEditorModule::ManagerTabId(TEXT("UIWidgetToolManager"));
const FName FUIWidgetToolPluginEditorModule::ManagerPanelTabId(
    TEXT("UIWidgetToolManagerPanel"));
const FName FUIWidgetToolPluginEditorModule::UtilityPanelTabId(
    TEXT("UIWidgetToolUtilityPanel"));
const FName FUIWidgetToolPluginEditorModule::SnapshotViewerTabId(
    TEXT("UIWidgetToolSnapshotViewer"));

namespace
{
  FDelegateHandle GSnapshotEndFrameHandle;
  FString GPendingSnapshotPath;
  int32 GPendingSnapshotFrames = 0;
  constexpr int32 RestoreSnapshotSettleFrames = 3;

  // Width the UI Widget Tool column opens at; the snapshot viewer gets the
  // rest. The layout only takes proportions, so this is applied once the
  // docking area has a size, by rewriting the coefficients of the two
  // side-by-side nodes holding the tabs (the splitter reads them live). Left
  // at the layout's proportions when the area is too narrow to give the
  // column this much and the viewer any.
  constexpr float InitialManagerPanelWidth = 800.f;
  constexpr float MinSnapshotViewerWidth = 200.f;

  // The docking nodes (stacks and splitters) from a tab's stack up to the
  // area, innermost first. Nodes keep their parent private, so this walks
  // the widget tree.
  TArray<TSharedRef<SDockingNode>>
  GetDockingNodeChain(const TSharedPtr<SDockTab> &InTab)
  {
    TArray<TSharedRef<SDockingNode>> Chain;
    TSharedPtr<SWidget> Widget;
    if (InTab.IsValid())
    {
      Widget = InTab->GetParentDockTabStack();
    }
    while (Widget.IsValid())
    {
      const FName Type = Widget->GetType();
      if (Type == TEXT("SDockingTabStack") || Type == TEXT("SDockingSplitter"))
      {
        Chain.Add(StaticCastSharedRef<SDockingNode>(Widget.ToSharedRef()));
      }
      Widget = Widget->GetParentWidget();
    }
    return Chain;
  }

  void ApplyInitialPanelWidth(const TSharedRef<SWidget> &InDockArea,
                              const TWeakPtr<FTabManager> &InTabManager,
                              FName InLeftTabId, FName InRightTabId)
  {
    InDockArea->RegisterActiveTimer(
        0.f,
        FWidgetActiveTimerDelegate::CreateLambda(
            [WeakArea = TWeakPtr<SWidget>(InDockArea), InTabManager,
             InLeftTabId, InRightTabId](double, float)
            {
              const TSharedPtr<SWidget> Area = WeakArea.Pin();
              const TSharedPtr<FTabManager> TabManager = InTabManager.Pin();
              if (!Area.IsValid() || !TabManager.IsValid())
              {
                return EActiveTimerReturnType::Stop;
              }

              const float Width = Area->GetTickSpaceGeometry().GetLocalSize().X;
              if (Width <= 0.f)
              {
                // Not laid out yet.
                return EActiveTimerReturnType::Continue;
              }

              const float RightWidth = Width - InitialManagerPanelWidth;
              if (RightWidth < MinSnapshotViewerWidth)
              {
                return EActiveTimerReturnType::Stop;
              }

              // The two nodes, one above each tab, that sit in the same
              // splitter: the left column and the viewer's stack.
              const TArray<TSharedRef<SDockingNode>> LeftChain =
                  GetDockingNodeChain(
                      TabManager->FindExistingLiveTab(FTabId(InLeftTabId)));
              const TArray<TSharedRef<SDockingNode>> RightChain =
                  GetDockingNodeChain(
                      TabManager->FindExistingLiveTab(FTabId(InRightTabId)));
              for (const TSharedRef<SDockingNode> &Left : LeftChain)
              {
                for (const TSharedRef<SDockingNode> &Right : RightChain)
                {
                  if (Left != Right &&
                      Left->GetParentWidget() == Right->GetParentWidget())
                  {
                    Left->SetSizeCoefficient(InitialManagerPanelWidth);
                    Right->SetSizeCoefficient(RightWidth);
                    return EActiveTimerReturnType::Stop;
                  }
                }
              }
              return EActiveTimerReturnType::Stop;
            }));
  }

  void CancelQueuedSnapshot()
  {
    if (GSnapshotEndFrameHandle.IsValid())
    {
      FCoreDelegates::OnEndFrame.Remove(GSnapshotEndFrameHandle);
      GSnapshotEndFrameHandle.Reset();
    }
    GPendingSnapshotPath.Reset();
    GPendingSnapshotFrames = 0;
  }

  bool TakeQueuedSnapshot()
  {
    const FString SnapshotPath = MoveTemp(GPendingSnapshotPath);
    CancelQueuedSnapshot();
    if (SnapshotPath.IsEmpty())
    {
      return false;
    }

    UIWTWidgetSnapshot::FSnapshotResult Result;
    if (!UIWTWidgetSnapshot::TakeSnapshot(SnapshotPath, Result))
    {
      UE_LOG(LogUIWidgetToolPlugin, Warning,
             TEXT("Widget snapshot skipped for '%s': %s"), *SnapshotPath,
             *Result.Message);
      return false;
    }

    UE_LOG(LogUIWidgetToolPlugin, Log,
           TEXT("Widget snapshot written to '%s' (PIE world: %s). %s"),
           *Result.FilePath,
           Result.RootDescription.IsEmpty() ? TEXT("<unknown>")
                                            : *Result.RootDescription,
           *Result.Message);
    return true;
  }

}

FUIWidgetToolPluginEditorModule &FUIWidgetToolPluginEditorModule::Get()
{
  return FModuleManager::GetModuleChecked<FUIWidgetToolPluginEditorModule>(
      "UIWidgetToolPluginEditor");
}

void FUIWidgetToolPluginEditorModule::StartupModule()
{
  UIWTGenerated::RegisterMountPoint();

  FUIWidgetToolPluginStyle::Initialize();
  FUIWTCommands::Register();

  CommandList = MakeShared<FUICommandList>();
  CommandList->MapAction(
      FUIWTCommands::Get().OpenManager,
      FExecuteAction::CreateLambda([]()
                                   { FGlobalTabmanager::Get()->TryInvokeTab(ManagerTabId); }));
  CommandList->MapAction(
      FUIWTCommands::Get().CaptureCheckpoint,
      FExecuteAction::CreateStatic(
          &FUIWidgetToolPluginEditorModule::CaptureCheckpointNow),
      FCanExecuteAction::CreateLambda([]()
                                      {
                                        if (!GEditor)
                                        {
                                          return false;
                                        }
                                        FText Reason;
                                        return UIWTCheckpointCapture::CanCaptureWorld(
                                            GEditor->PlayWorld, Reason); }));

  FGlobalTabmanager::Get()
      ->RegisterNomadTabSpawner(
          ManagerTabId,
          FOnSpawnTab::CreateRaw(
              this, &FUIWidgetToolPluginEditorModule::SpawnManagerTab))
      .SetDisplayName(LOCTEXT("ManagerTabTitle", "UI Widget Tool"))
      .SetIcon(FSlateIcon(FUIWidgetToolPluginStyle::GetStyleSetName(),
                          "UIWidgetTool.OpenManager"))
      .SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

  UToolMenus::RegisterStartupCallback(
      FSimpleMulticastDelegate::FDelegate::CreateRaw(
          this, &FUIWidgetToolPluginEditorModule::RegisterMenus));

  FEditorDelegates::BeginPIE.AddRaw(
      this, &FUIWidgetToolPluginEditorModule::OnBeginPIE);
  FEditorDelegates::EndPIE.AddRaw(this,
                                  &FUIWidgetToolPluginEditorModule::OnEndPIE);

  if (GEditor)
  {
    GEditor->OnBlueprintCompiled().AddRaw(
        this, &FUIWidgetToolPluginEditorModule::OnBlueprintCompiled);
  }
  UUIWTCheckpointRestoreSubsystem::OnRestoreFinished().AddRaw(
      this, &FUIWidgetToolPluginEditorModule::OnRestoreFinished);

  UIWTPlayLauncher::Startup();

  FUIWTClaudeService::Create();
  FUIWTClaudeService::Get().OnEntriesChanged().AddRaw(
      this, &FUIWidgetToolPluginEditorModule::RefreshManager);
  FUIWTClaudeService::Get().OnChatChanged().AddRaw(
      this, &FUIWidgetToolPluginEditorModule::PushSelectionToViewer);
}

void FUIWidgetToolPluginEditorModule::ShutdownModule()
{
  if (FUIWTClaudeService *Service = FUIWTClaudeService::TryGet())
  {
    Service->OnEntriesChanged().RemoveAll(this);
    Service->OnChatChanged().RemoveAll(this);
  }
  FUIWTClaudeService::Destroy();

  UIWTPlayLauncher::Shutdown();
  CancelQueuedSnapshot();

  ManagerTabManager.Reset();

  UnregisterInputProcessor();

  FEditorDelegates::BeginPIE.RemoveAll(this);
  FEditorDelegates::EndPIE.RemoveAll(this);
  UUIWTCheckpointRestoreSubsystem::OnRestoreFinished().RemoveAll(this);
  if (GEditor)
  {
    GEditor->OnBlueprintCompiled().RemoveAll(this);
  }

  UToolMenus::UnRegisterStartupCallback(this);
  UToolMenus::UnregisterOwner(this);

  if (FGlobalTabmanager::Get()->HasTabSpawner(ManagerTabId))
  {
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ManagerTabId);
  }

  CommandList.Reset();
  FUIWTCommands::Unregister();
  FUIWidgetToolPluginStyle::Shutdown();

  if (!IsEngineExitRequested())
  {
    UIWTGenerated::UnregisterMountPoint();
  }
}

void FUIWidgetToolPluginEditorModule::RegisterMenus()
{
  FToolMenuOwnerScoped OwnerScoped(this);

  UToolMenu *Toolbar = UToolMenus::Get()->ExtendMenu(
      "LevelEditor.LevelEditorToolBar.PlayToolBar");
  if (!Toolbar)
    return;

  FToolMenuSection &Section = Toolbar->FindOrAddSection("UIWidgetTool");

  FToolMenuEntry &OpenManagerEntry =
      Section.AddEntry(FToolMenuEntry::InitToolBarButton(
          FUIWTCommands::Get().OpenManager, TAttribute<FText>(),
          TAttribute<FText>(),
          FSlateIcon(FUIWidgetToolPluginStyle::GetStyleSetName(),
                     "UIWidgetTool.OpenManager")));
  OpenManagerEntry.SetCommandList(CommandList);

  FToolMenuEntry &CaptureEntry =
      Section.AddEntry(FToolMenuEntry::InitToolBarButton(
          FUIWTCommands::Get().CaptureCheckpoint, TAttribute<FText>(),
          TAttribute<FText>(),
          FSlateIcon(FAppStyle::Get().GetStyleSetName(), "Icons.Save")));
  CaptureEntry.SetCommandList(CommandList);
}

void FUIWidgetToolPluginEditorModule::CaptureCheckpointNow()
{
  if (!GEditor)
    return;
  UWorld *PlayWorld = GEditor->PlayWorld;
  FText Reason;
  if (!UIWTCheckpointCapture::CanCaptureWorld(PlayWorld, Reason))
  {
    UIWTNotify::Show(Reason, false);
    return;
  }

  // Empty name: the capture stores "<MapName>_<CapturedAtUtc>".
  FUIWTCaptureResult Result;
  if (!UIWTCheckpointCapture::CaptureWorld(PlayWorld, FString(), Result))
  {
    UIWTNotify::Show(
        FText::Format(LOCTEXT("CaptureFailed", "Checkpoint capture failed: {0}"),
                      FText::FromString(Result.Message)),
        false);
    return;
  }

  QueueWidgetSnapshot(
      UIWTCheckpointCodec::MakeSnapshotPathFromSidecar(Result.SidecarPath));

  UIWTNotify::Show(
      FText::Format(
          LOCTEXT("CaptureOk",
                  "Captured {0} actors from {1} ({2} KB). {3}"),
          FText::AsNumber(Result.ActorCount),
          FText::FromString(Result.MapPackagePath),
          FText::AsNumber(Result.CompressedBytes / 1024),
          FText::FromString(Result.Message)),
      true);

  Get().RefreshManager();
}

// snapshot taken at the end of the frame.
void FUIWidgetToolPluginEditorModule::QueueWidgetSnapshot(
    const FString &InSnapshotPath, int32 InSettleFrames)
{
  CancelQueuedSnapshot();
  if (InSnapshotPath.IsEmpty())
  {
    return;
  }

  GPendingSnapshotPath = InSnapshotPath;
  GPendingSnapshotFrames = FMath::Max(0, InSettleFrames);
  // A file the viewer already shows has a new timestamp now, which is what
  // the sync keys its reload on. A file that did not exist before reaches
  // the viewer through the manager's directory watcher instead.
  GSnapshotEndFrameHandle = FCoreDelegates::OnEndFrame.AddLambda(
      []
      {
        if (GPendingSnapshotFrames-- > 0)
        {
          return;
        }
        if (TakeQueuedSnapshot())
        {
          Get().SyncSnapshotToViewer();
        }
      });
}

void FUIWidgetToolPluginEditorModule::OnRestoreFinished(
    UWorld *, const FUIWTPendingRestore &InRequest)
{
  if (!GetDefault<UUIWidgetPreviewObjectManagerSettings>()
           ->bRefreshSnapshotOnRestore)
  {
    return;
  }
  QueueWidgetSnapshot(UIWTCheckpointCodec::MakeSnapshotPathFromSidecar(
                          InRequest.CheckpointSidecarFile),
                      RestoreSnapshotSettleFrames);
}

void FUIWidgetToolPluginEditorModule::RefreshManager()
{
  if (ManagerWidget.IsValid())
  {
    ManagerWidget->RefreshAll();
  }
}

void FUIWidgetToolPluginEditorModule::OnBeginPIE(bool bIsSimulating)
{
  FUIWTClaudeService::Get().CancelRun();
  RegisterInputProcessor();
  if (UToolMenus *ToolMenus = UToolMenus::Get())
    ToolMenus->RefreshAllWidgets();
}

void FUIWidgetToolPluginEditorModule::OnEndPIE(bool bIsSimulating)
{
  CancelQueuedSnapshot();
  UnregisterInputProcessor();
  if (UToolMenus *ToolMenus = UToolMenus::Get())
  {
    ToolMenus->RefreshAllWidgets();
  }
  RefreshManager();
}

// add keyboard hook for capture during PIE
void FUIWidgetToolPluginEditorModule::RegisterInputProcessor()
{
  if (InputProcessor.IsValid() || !CommandList.IsValid() ||
      !FSlateApplication::IsInitialized())
    return;

  InputProcessor = MakeShared<FUIWTInputProcessor>(CommandList.ToSharedRef());
  FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor);
}

// remove keyboard hook when not in PIE
void FUIWidgetToolPluginEditorModule::UnregisterInputProcessor()
{
  if (!InputProcessor.IsValid())
    return;

  if (FSlateApplication::IsInitialized())
  {
    FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
  }
  InputProcessor.Reset();
}

// live widget reload during PIE
void FUIWidgetToolPluginEditorModule::OnBlueprintCompiled()
{
  if (!GEditor || !GEditor->PlayWorld)
    return;

  if (UUIWTCheckpointRestoreSubsystem *Subsystem =
          GEditor->PlayWorld->GetSubsystem<UUIWTCheckpointRestoreSubsystem>())
  {
    if (Subsystem->HasWidget())
    {
      Subsystem->RebuildWidget();
    }
  }
}

TSharedRef<SDockTab>
FUIWidgetToolPluginEditorModule::SpawnManagerPanelTab(const FSpawnTabArgs &)
{
  TSharedRef<SDockTab> Panel = SNew(SDockTab).TabRole(ETabRole::PanelTab);
  GetOrMakeManagerWidget();
  Panel->SetContent(ManagerWidget.ToSharedRef());
  return Panel;
}

// The panel is built by the manager, so whichever of the two tabs spawns
// first makes it.
TSharedRef<SDockTab>
FUIWidgetToolPluginEditorModule::SpawnUtilityPanelTab(const FSpawnTabArgs &)
{
  // Nothing in the tool reopens it, so it cannot be closed; it can still be
  // dragged and docked anywhere.
  TSharedRef<SDockTab> Panel =
      SNew(SDockTab)
          .TabRole(ETabRole::PanelTab)
          .OnCanCloseTab_Lambda([] { return false; });
  Panel->SetContent(GetOrMakeManagerWidget().MakeUtilityPanel());
  return Panel;
}

SUIWidgetManager &FUIWidgetToolPluginEditorModule::GetOrMakeManagerWidget()
{
  if (!ManagerWidget.IsValid())
  {
    MakeManagerWidget();
  }
  return *ManagerWidget;
}

TSharedRef<SDockTab>
FUIWidgetToolPluginEditorModule::SpawnManagerTab(const FSpawnTabArgs &)
{
  TSharedRef<SDockTab> Tab =
      SNew(SDockTab)
          .TabRole(ETabRole::NomadTab)
          .OnTabClosed(SDockTab::FOnTabClosedCallback::CreateRaw(
              this, &FUIWidgetToolPluginEditorModule::OnManagerTabClosed));

  ManagerTabManager = FGlobalTabmanager::Get()->NewTabManager(Tab);

  const FSlateIcon TabIcon(FUIWidgetToolPluginStyle::GetStyleSetName(),
                           "UIWidgetTool.OpenManager");

  ManagerTabManager
      ->RegisterTabSpawner(
          ManagerPanelTabId,
          FOnSpawnTab::CreateRaw(
              this, &FUIWidgetToolPluginEditorModule::SpawnManagerPanelTab))
      .SetDisplayName(LOCTEXT("ManagerPanelTitle", "UI Widget Tool"))
      .SetIcon(TabIcon);

  ManagerTabManager
      ->RegisterTabSpawner(
          UtilityPanelTabId,
          FOnSpawnTab::CreateRaw(
              this, &FUIWidgetToolPluginEditorModule::SpawnUtilityPanelTab))
      .SetDisplayName(LOCTEXT("UtilityPanelTitle", "Utility"))
      .SetIcon(TabIcon);

  ManagerTabManager
      ->RegisterTabSpawner(
          SnapshotViewerTabId,
          FOnSpawnTab::CreateRaw(
              this, &FUIWidgetToolPluginEditorModule::SpawnSnapshotViewerTab))
      .SetDisplayName(LOCTEXT("SnapshotViewerTitle", "Snapshot Viewer"))
      .SetIcon(TabIcon);

  const TSharedRef<FTabManager::FLayout> Layout =
      FTabManager::NewLayout("UIWidgetToolPanelLayout_v5")
          ->AddArea(
              FTabManager::NewPrimaryArea()
                  ->SetOrientation(Orient_Horizontal)
                  ->Split(
                      FTabManager::NewSplitter()
                          ->SetOrientation(Orient_Vertical)
                          ->SetSizeCoefficient(0.6f)
                          ->Split(FTabManager::NewStack()
                                      ->SetSizeCoefficient(0.5f)
                                      ->AddTab(ManagerPanelTabId,
                                               ETabState::OpenedTab))
                          ->Split(FTabManager::NewStack()
                                      ->SetSizeCoefficient(0.5f)
                                      ->AddTab(UtilityPanelTabId,
                                               ETabState::OpenedTab)))
                  ->Split(FTabManager::NewStack()
                              ->SetSizeCoefficient(0.4f)
                              ->AddTab(SnapshotViewerTabId,
                                       ETabState::OpenedTab)));

  TSharedPtr<SWidget> Content = ManagerTabManager->RestoreFrom(Layout, nullptr);
  if (Content.IsValid())
  {
    Tab->SetContent(Content.ToSharedRef());
    ApplyInitialPanelWidth(Content.ToSharedRef(), ManagerTabManager,
                           ManagerPanelTabId, SnapshotViewerTabId);
  }
  else
  {
    UE_LOG(LogUIWidgetToolPlugin, Warning,
           TEXT("Could not restore the UI Widget Tool panel layout; falling "
                "back to the bare manager widget."));
    ManagerTabManager.Reset();
    SUIWidgetManager &Manager = GetOrMakeManagerWidget();
    Tab->SetContent(SNew(SSplitter).Orientation(Orient_Vertical) +
                    SSplitter::Slot()[ManagerWidget.ToSharedRef()] +
                    SSplitter::Slot()[Manager.MakeUtilityPanel()]);
  }

  return Tab;
}

void FUIWidgetToolPluginEditorModule::OnManagerTabClosed(
    TSharedRef<SDockTab> ClosedTab)
{
  ManagerTabManager.Reset();
  ManagerWidget.Reset();
  ChatSection.Reset();
  SnapshotViewerWidget.Reset();
  SyncedSnapshotPath.Reset();
}

TSharedRef<SDockTab>
FUIWidgetToolPluginEditorModule::SpawnSnapshotViewerTab(const FSpawnTabArgs &)
{
  TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::PanelTab);
  Tab->SetContent(
      SAssignNew(SnapshotViewerWidget, SUIWTSnapshotViewer).OwnerTab(Tab));
  PushSelectionToViewer();
  SyncSnapshotToViewer();
  return Tab;
}

TSharedRef<SUIWidgetManager> FUIWidgetToolPluginEditorModule::MakeManagerWidget()
{
  ChatSection =
      SNew(SUIWTChatSection)
          .SelectedEntryId_Lambda(
              [this]
              {
                return ManagerWidget.IsValid()
                           ? ManagerWidget->GetSelectedEntryId()
                           : FGuid();
              })
          .GetRunContext_Lambda(
              [this]
              {
                FUIWTRunContext Context;
                if (ManagerWidget.IsValid())
                {
                  ManagerWidget->DescribeEntryForRun(
                      ManagerWidget->GetSelectedEntryId(),
                      Context.LevelPackagePath, Context.CheckpointDisplay);
                }
                if (SnapshotViewerWidget.IsValid())
                {
                  Context.PickedWidget = SnapshotViewerWidget->GetPickedWidget();
                }
                return Context;
              });

  TSharedRef<SUIWidgetManager> Manager =
      SAssignNew(ManagerWidget, SUIWidgetManager)
          .UtilityPanelContent()[ChatSection.ToSharedRef()]
          .IsRunInFlight_Lambda(
              []
              {
                const FUIWTClaudeService *Service =
                    FUIWTClaudeService::TryGet();
                return Service && Service->IsRunInFlight();
              })
          .OnSelectionChanged_Raw(
              this, &FUIWidgetToolPluginEditorModule::OnManagerSelectionChanged)
          .OnOpenSnapshot_Raw(
              this, &FUIWidgetToolPluginEditorModule::LoadSnapshotInViewer)
          .OnDesignBlueprintChanged_Lambda(
              [this](UWidgetBlueprint *Blueprint,
                     const TArray<FString> &AcceptedAssetPaths)
              {
                if (SnapshotViewerWidget.IsValid())
                {
                  SnapshotViewerWidget->SetDesignBlueprint(Blueprint,
                                                           AcceptedAssetPaths);
                }
              })
          .OnEntryDeleted_Lambda(
              [](const FGuid &EntryId)
              {
                if (FUIWTClaudeService *Service = FUIWTClaudeService::TryGet())
                {
                  Service->ForgetChatState(EntryId);
                }
              });

  OnManagerSelectionChanged();
  return Manager;
}

void FUIWidgetToolPluginEditorModule::OnManagerSelectionChanged()
{
  if (ChatSection.IsValid())
  {
    ChatSection->Refresh();
  }
  PushSelectionToViewer();
  SyncSnapshotToViewer();
}

void FUIWidgetToolPluginEditorModule::PushSelectionToViewer()
{
  if (ManagerWidget.IsValid())
  {
    ManagerWidget->PushSelectionToViewer();
  }
}

void FUIWidgetToolPluginEditorModule::SyncSnapshotToViewer()
{
  if (!ManagerWidget.IsValid() || !SnapshotViewerWidget.IsValid())
  {
    return;
  }

  const FString SnapshotPath = ManagerWidget->GetSelectedSnapshotPath();
  if (SnapshotPath.IsEmpty())
  {
    if (!SyncedSnapshotPath.IsEmpty() &&
        SnapshotViewerWidget->GetLoadedSnapshotPath() == SyncedSnapshotPath)
    {
      SnapshotViewerWidget->ClearSnapshot();
    }
    SyncedSnapshotPath.Reset();
    return;
  }

  if (SnapshotViewerWidget->IsShowingSnapshot(SnapshotPath))
  {
    SyncedSnapshotPath = SnapshotPath;
    return;
  }

  FText Error;
  if (SnapshotViewerWidget->LoadSnapshot(SnapshotPath, Error))
  {
    SyncedSnapshotPath = SnapshotPath;
  }
  else
  {
    UE_LOG(LogUIWidgetToolPlugin, Warning,
           TEXT("Could not load the selected entry's snapshot '%s': %s"),
           *SnapshotPath, *Error.ToString());
  }
}

bool FUIWidgetToolPluginEditorModule::LoadSnapshotInViewer(
    const FString &InSnapshotPath, FText &OutError)
{
  if (InSnapshotPath.IsEmpty() ||
      !IFileManager::Get().FileExists(*InSnapshotPath))
  {
    OutError = LOCTEXT("SnapshotMissing",
                       "That checkpoint's snapshot is no longer on disk.");
    return false;
  }

  if (!ManagerTabManager.IsValid())
  {
    OutError = LOCTEXT("NoViewerTab",
                       "The UI Widget Tool panel is not open.");
    return false;
  }

  ManagerTabManager->TryInvokeTab(FTabId(SnapshotViewerTabId));

  if (!SnapshotViewerWidget.IsValid())
  {
    OutError = LOCTEXT("NoViewerWidget",
                       "The snapshot viewer could not be opened.");
    return false;
  }

  if (!SnapshotViewerWidget->LoadSnapshot(InSnapshotPath, OutError))
  {
    return false;
  }
  // The manager's button loads the selected entry's snapshot, so it counts
  // as synced: moving to an entry without one clears it like any other.
  SyncedSnapshotPath = InSnapshotPath;
  return true;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUIWidgetToolPluginEditorModule, UIWidgetToolPluginEditor)
