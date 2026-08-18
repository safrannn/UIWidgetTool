#include "UIWidgetToolPlugin.h"

#include "SUIWidgetManager.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "UIWidgetToolPluginStyle.h"

#include "Framework/Docking/TabManager.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#include "AssetRegistry/AssetData.h"
#include "Blueprint/UserWidget.h"
#include "ContentBrowserDelegates.h"
#include "ContentBrowserModule.h"
#include "Engine/Blueprint.h"
#include "WidgetBlueprint.h"

#define LOCTEXT_NAMESPACE "FUIWidgetToolPluginModule"

const FName
    FUIWidgetToolPluginModule::ManagerTabId(TEXT("UIWidgetToolManager"));
const FName
    FUIWidgetToolPluginModule::UpdateTabId(TEXT("UIWidgetToolUpdate"));
const FName FUIWidgetToolPluginModule::PlayTabId(TEXT("UIWidgetToolPlay"));

void FUIWidgetToolPluginModule::StartupModule() {
  FUIWidgetToolPluginStyle::Initialize();

  // Register the dockable tabs.
  FGlobalTabmanager::Get()
      ->RegisterNomadTabSpawner(
          ManagerTabId, FOnSpawnTab::CreateRaw(
                            this, &FUIWidgetToolPluginModule::SpawnManagerTab))
      .SetDisplayName(LOCTEXT("ManagerTabTitle", "UI Widget Tool"))
      .SetIcon(FSlateIcon(FUIWidgetToolPluginStyle::GetStyleSetName(),
                          "UIWidgetTool.OpenManager.Small"))
      .SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

  FGlobalTabmanager::Get()
      ->RegisterNomadTabSpawner(
          UpdateTabId, FOnSpawnTab::CreateRaw(
                           this, &FUIWidgetToolPluginModule::SpawnUpdateTab))
      .SetMenuType(ETabSpawnerMenuType::Hidden);

  FGlobalTabmanager::Get()
      ->RegisterNomadTabSpawner(
          PlayTabId, FOnSpawnTab::CreateRaw(
                         this, &FUIWidgetToolPluginModule::SpawnPlayTab))
      .SetMenuType(ETabSpawnerMenuType::Hidden);

  UToolMenus::RegisterStartupCallback(
      FSimpleMulticastDelegate::FDelegate::CreateRaw(
          this, &FUIWidgetToolPluginModule::RegisterMenus));

  RegisterContentBrowserExtension();
}

void FUIWidgetToolPluginModule::ShutdownModule() {
  UToolMenus::UnRegisterStartupCallback(this);
  UToolMenus::UnregisterOwner(this);

  if (FGlobalTabmanager::Get()->HasTabSpawner(ManagerTabId)) {
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ManagerTabId);
  }
  if (FGlobalTabmanager::Get()->HasTabSpawner(UpdateTabId)) {
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(UpdateTabId);
  }
  if (FGlobalTabmanager::Get()->HasTabSpawner(PlayTabId)) {
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(PlayTabId);
  }

  FUIWidgetToolPluginStyle::Shutdown();
}

void FUIWidgetToolPluginModule::RegisterMenus() {
  FToolMenuOwnerScoped OwnerScoped(this);

  // Toolbar button in the main editor toolbar to open the manager.
  UToolMenu *Toolbar = UToolMenus::Get()->ExtendMenu(
      "LevelEditor.LevelEditorToolBar.PlayToolBar");
  if (Toolbar) {
    FToolMenuSection &Section = Toolbar->FindOrAddSection("UIWidgetTool");
    Section.AddEntry(FToolMenuEntry::InitToolBarButton(
        "OpenUIWidgetTool", FUIAction(FExecuteAction::CreateLambda([]() {
          FGlobalTabmanager::Get()->TryInvokeTab(ManagerTabId);
        })),
        LOCTEXT("OpenToolLabel", "UI Widget Tool"),
        LOCTEXT("OpenToolTip", "Open the UI Widget testing manager"),
        FSlateIcon(FUIWidgetToolPluginStyle::GetStyleSetName(),
                   "UIWidgetTool.OpenManager")));
  }
}

void FUIWidgetToolPluginModule::RegisterContentBrowserExtension() {
  FContentBrowserModule &CBModule =
      FModuleManager::LoadModuleChecked<FContentBrowserModule>(
          "ContentBrowser");

  TArray<FContentBrowserMenuExtender_SelectedAssets> &Extenders =
      CBModule.GetAllAssetViewContextMenuExtenders();

  Extenders.Add(FContentBrowserMenuExtender_SelectedAssets::CreateLambda(
      [this](
          const TArray<FAssetData> &SelectedAssets) -> TSharedRef<FExtender> {
        TSharedRef<FExtender> Extender = MakeShared<FExtender>();

        // Only offer the action if all selected assets are Widget Blueprints.
        bool bAllWidgets = SelectedAssets.Num() > 0;
        for (const FAssetData &Asset : SelectedAssets) {
          if (Asset.AssetClassPath !=
              UWidgetBlueprint::StaticClass()->GetClassPathName()) {
            bAllWidgets = false;
            break;
          }
        }

        if (bAllWidgets) {
          TArray<FAssetData> Captured = SelectedAssets;
          Extender->AddMenuExtension(
              "GetAssetActions", EExtensionHook::After, nullptr,
              FMenuExtensionDelegate::CreateLambda([this, Captured](
                                                       FMenuBuilder &Builder) {
                Builder.AddMenuEntry(
                    LOCTEXT("AddToTool", "Add to UI widget tool"),
                    LOCTEXT(
                        "AddToToolTip",
                        "Register this widget in the UI Widget testing tool"),
                    FSlateIcon(),
                    FUIAction(FExecuteAction::CreateRaw(
                        this, &FUIWidgetToolPluginModule::OnAddToTool,
                        Captured)));
              }));
        }

        return Extender;
      }));
}

void FUIWidgetToolPluginModule::OnAddToTool(TArray<FAssetData> SelectedAssets) {
  UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();
  if (!Settings) {
    return;
  }

  for (const FAssetData &Asset : SelectedAssets) {
    UWidgetBlueprint *WBP = Cast<UWidgetBlueprint>(Asset.GetAsset());
    if (!WBP || !WBP->GeneratedClass) {
      continue;
    }

    // Skip abstract/deprecated classes.
    if (WBP->GeneratedClass->HasAnyClassFlags(CLASS_Abstract |
                                              CLASS_Deprecated)) {
      continue;
    }

    TSoftClassPtr<UUserWidget> SoftClass(WBP->GeneratedClass);
    Settings->AddWidgetPreviewObject(SoftClass, WBP->GetName());
  }

  // Refresh the manager if it's open.
  if (ManagerWidget.IsValid()) {
    ManagerWidget->RefreshList();
  }
}

TSharedRef<SDockTab>
FUIWidgetToolPluginModule::SpawnManagerTab(const FSpawnTabArgs &) {
  TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab);
  Tab->SetContent(SAssignNew(ManagerWidget, SUIWidgetManager));
  return Tab;
}

TSharedRef<SDockTab>
FUIWidgetToolPluginModule::SpawnUpdateTab(const FSpawnTabArgs &) {
  return SNew(SDockTab)
      .TabRole(ETabRole::NomadTab)
      .Label(LOCTEXT("UpdateTabTitle", "Update"));
}

void FUIWidgetToolPluginModule::OpenUpdateTab() {
  FGlobalTabmanager::Get()->TryInvokeTab(UpdateTabId);
}

TSharedRef<SDockTab>
FUIWidgetToolPluginModule::SpawnPlayTab(const FSpawnTabArgs &) {
  return SNew(SDockTab)
      .TabRole(ETabRole::NomadTab)
      .Label(LOCTEXT("PlayTabTitle", "Play"));
}

void FUIWidgetToolPluginModule::OpenPlayTab() {
  FGlobalTabmanager::Get()->TryInvokeTab(PlayTabId);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUIWidgetToolPluginModule, UIWidgetToolPlugin)