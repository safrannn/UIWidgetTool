#include "SUIWidgetManager.h"

#include "Blueprint/UserWidget.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "SUIWTDetailRow.h"
#include "SUIWTEntryRow.h"
#include "SUIWTManagerCells.h"
#include "UIWTCheckpointTypes.h"
#include "Core/UIWTGeneratedBlueprints.h"
#include "Core/UIWTNotify.h"
#include "Host/UIWidgetToolPluginStyle.h"
#include "UIWTPlayLauncher.h"
#include "UIWTSearchablePicker.h"
#include "WidgetBlueprint.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"
#include "UIWidgetToolPlugin.h"

#define LOCTEXT_NAMESPACE "UIWidgetTool"

namespace
{
  constexpr float ToolbarPanelHeight = 28.f;
  constexpr float SearchBoxWidth = 500.f;
  constexpr float ListButtonRowHeight = 40.f;
  constexpr float ButtonGap = 2.f;

  constexpr float WidgetColumnFillWidth = 0.34f;
  constexpr float LevelColumnFillWidth = 0.30f;
  constexpr float LevelCheckpointColumnFillWidth = 0.36f;

  constexpr float HeaderRowHeight = 24.f;
  constexpr float DesiredVisibleRows = 10.f;

  constexpr float InformationPanelWidth = 400.f;

  TOptional<EUIWidgetSortField> SortFieldForColumn(FName ColumnId)
  {
    if (ColumnId == UIWTManagerColumns::Widget)
    {
      return EUIWidgetSortField::WidgetName;
    }
    if (ColumnId == UIWTManagerColumns::Level)
    {
      return EUIWidgetSortField::Level;
    }
    if (ColumnId == UIWTManagerColumns::LevelCheckpoint)
    {
      return EUIWidgetSortField::Checkpoint;
    }
    return TOptional<EUIWidgetSortField>();
  }

  struct FSearchFieldOption
  {
    EUIWTSearchField Field;
    FText Label;
  };

  const TArray<FSearchFieldOption> &GetSearchFieldOptions()
  {
    static const TArray<FSearchFieldOption> Options = {
        {EUIWTSearchField::All, LOCTEXT("FilterAll", "All columns")},
        {EUIWTSearchField::Widget, LOCTEXT("FilterWidget", "Widget")},
        {EUIWTSearchField::Level, LOCTEXT("FilterLevel", "Level")},
        {EUIWTSearchField::Checkpoint,
         LOCTEXT("FilterCheckpoint", "Level Checkpoint")}};
    return Options;
  }

  // The package the level scan looks for referencers of. A duplicate is
  // referenced by nothing, so it scans as the entry it was duplicated from;
  // SourceEntryId is always the root, so this is one lookup.
  FName GetWidgetPackageName(const FWidgetPreviewObject &InPreviewObject)
  {
    const FWidgetPreviewObject *Root = &InPreviewObject;
    if (const FWidgetPreviewObject *Found =
            UUIWidgetPreviewObjectManagerSettings::Get()
                ->FindWidgetPreviewObject(InPreviewObject.SourceEntryId))
    {
      Root = Found;
    }
    const FString PackageName =
        Root->WidgetClass.ToSoftObjectPath().GetLongPackageName();
    return PackageName.IsEmpty() ? NAME_None : FName(*PackageName);
  }

  SHorizontalBox::FSlot::FSlotArguments ButtonSlot(TSharedRef<SWidget> Button)
  {
    SHorizontalBox::FSlot::FSlotArguments Slot = SHorizontalBox::Slot();
    Slot.AutoWidth().Padding(FMargin(0.f, 0.f, ButtonGap, 0.f))[Button];
    return Slot;
  }

  // The icon-only button the toolbar and the details rows share, so they
  // all match.
  TSharedRef<SWidget>
  MakeIconButton(TAttribute<const FSlateBrush *> Brush, TAttribute<bool> IsEnabled,
                 TAttribute<FText> ToolTip, FOnClicked OnClicked,
                 TAttribute<FSlateColor> ButtonColor = FLinearColor::White)
  {
    return SNew(SButton)
        .ContentPadding(FMargin(2.f))
        .IsEnabled(IsEnabled)
        .ToolTipText(ToolTip)
        .ButtonColorAndOpacity(ButtonColor)
        .OnClicked(OnClicked)
            [SNew(SImage).Image(Brush).ColorAndOpacity(
                FSlateColor::UseForeground())];
  }

  void NotifyWidgetPickFailed(const TSharedPtr<FAssetData> &Selection)
  {
    UIWTNotify::Show(
        FText::Format(
            LOCTEXT("PickWidgetFailed",
                    "{0} could not be loaded, so the entry was left "
                    "unchanged."),
            FText::FromName(Selection->AssetName)),
        false);
  }

}

void SUIWidgetManager::Construct(const FArguments &InArgs)
{
  UtilityPanelContent = InArgs._UtilityPanelContent.Widget;
  IsRunInFlight = InArgs._IsRunInFlight;
  OnSelectionChanged = InArgs._OnSelectionChanged;
  OnOpenSnapshot = InArgs._OnOpenSnapshot;
  OnDesignBlueprintChanged = InArgs._OnDesignBlueprintChanged;
  OnEntryDeleted = InArgs._OnEntryDeleted;

  ChildSlot.Padding(FMargin(4.f))
      [SNew(SVerticalBox) +
       SVerticalBox::Slot().FillHeight(1.f)[BuildListPanel()] +
       SVerticalBox::Slot().AutoHeight().Padding(FMargin(2.f, 6.f, 2.f, 0.f))
           [SNew(SHorizontalBox) +
            SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
                [SNew(STextBlock)
                     .Text(this,
                           &SUIWidgetManager::GetCheckpointDirectoryText)
                     .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                     .AutoWrapText(true)] +
            SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(6.f, 0.f, 0.f, 0.f))[SNew(SButton).Text(LOCTEXT("OpenSaveDirBtn", "Open Save Directory")).ToolTipText(LOCTEXT("OpenSaveDirTip", "Open the save directory in the file browser.")).OnClicked(this, &SUIWidgetManager::OnOpenSaveDirectoryClicked)]]];

  CheckpointWatcher.OnChanged = FSimpleDelegate::CreateSP(
      this, &SUIWidgetManager::OnCheckpointDirectoryChanged);
  RefreshAll();
}

TSharedRef<SWidget> SUIWidgetManager::BuildListToolbar()
{
  TSharedRef<SWidget> ButtonRow =
      SNew(SBox)
          .HeightOverride(ListButtonRowHeight)
          .VAlign(VAlign_Center)
          .Padding(FMargin(2.f))
              [SNew(SHorizontalBox) +
               ButtonSlot(MakeIconButton(
                   FAppStyle::GetBrush("Icons.AddCircle"), true,
                   LOCTEXT("NewEntryTip", "Add a new entry."),
                   FOnClicked::CreateSP(this,
                                        &SUIWidgetManager::OnAddWidgetClicked))) +
               ButtonSlot(MakeIconButton(
                   TAttribute<const FSlateBrush *>(
                       this, &SUIWidgetManager::GetUpdateButtonIcon),
                   TAttribute<bool>(this, &SUIWidgetManager::HasSelection),
                   TAttribute<FText>(this,
                                     &SUIWidgetManager::GetUpdateButtonToolTip),
                   FOnClicked::CreateSP(
                       this, &SUIWidgetManager::OnUpdateOrConfirmClicked),
                   TAttribute<FSlateColor>(
                       this, &SUIWidgetManager::GetUpdateButtonColor))) +
               // The one captioned button; same padding height as the icons.
               ButtonSlot(
                   SNew(SButton)
                       .ContentPadding(FMargin(4.f, 2.f))
                       .Text(LOCTEXT("CancelBtn", "Cancel"))
                       .ToolTipText(LOCTEXT("CancelTip", "Undo the changes."))
                       .Visibility(this,
                                   &SUIWidgetManager::GetEditOnlyVisibility)
                       .OnClicked(this,
                                  &SUIWidgetManager::OnCancelEditClicked)) +
               ButtonSlot(MakeIconButton(
                   FAppStyle::GetBrush("Icons.Duplicate"),
                   TAttribute<bool>(this,
                                    &SUIWidgetManager::CanDuplicateSelected),
                   TAttribute<FText>(this,
                                     &SUIWidgetManager::GetDuplicateToolTip),
                   FOnClicked::CreateSP(
                       this, &SUIWidgetManager::OnDuplicateSelectedClicked))) +
               ButtonSlot(MakeIconButton(
                   FUIWidgetToolPluginStyle::Get().GetBrush(
                       "UIWidgetTool.Icons.Delete"),
                   TAttribute<bool>(this, &SUIWidgetManager::HasSelection),
                   LOCTEXT("DeleteTip", "Remove the selected entry."),
                   FOnClicked::CreateSP(
                       this, &SUIWidgetManager::OnDeleteSelectedClicked))) +
               SHorizontalBox::Slot().FillWidth(1.f) +
               SHorizontalBox::Slot().AutoWidth()[MakeIconButton(
                   FAppStyle::GetBrush("Icons.Refresh"), true,
                   LOCTEXT("RefreshTip",
                           "Re-read the checkpoint directory and rescan which "
                           "levels reference each widget."),
                   FOnClicked::CreateSP(this,
                                        &SUIWidgetManager::OnRefreshClicked))]];

  TSharedRef<SWidget> FilterButton =
      SNew(SComboButton)
          .ComboButtonStyle(FAppStyle::Get(), "SimpleComboButton")
          .ToolTipText(this, &SUIWidgetManager::GetFilterToolTip)
          .OnGetMenuContent(this, &SUIWidgetManager::BuildFilterMenu)
          .ButtonContent()
              [SNew(SHorizontalBox) +
               SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 4.f, 0.f))[SNew(SImage).Image(FAppStyle::Get().GetBrush("Icons.Filter")).ColorAndOpacity(FSlateColor::UseForeground())] +
               SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                   [SNew(STextBlock).Text(LOCTEXT("FilterBtn", "Filter"))]];

  TSharedRef<SWidget> SearchRow =
      SNew(SBox)
          .HeightOverride(ToolbarPanelHeight)
          .Padding(FMargin(2.f))
              [SNew(SHorizontalBox) +
               SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, ButtonGap, 0.f))[FilterButton] +
               SHorizontalBox::Slot()
                   .AutoWidth()
                   .VAlign(VAlign_Center)
                   .Padding(FMargin(0.f, 0.f, ButtonGap, 0.f))
                       [SNew(STextBlock)
                            .Text(this,
                                  &SUIWidgetManager::GetSearchFieldLabel)
                            .Visibility(
                                this,
                                &SUIWidgetManager::
                                    GetSearchFieldLabelVisibility)
                            .ToolTipText(LOCTEXT(
                                "FilterScopeLabelTip",
                                "The column the search box is currently "
                                "matching against. Change it from the Filter "
                                "menu."))] +
               SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                   [SNew(SBox).WidthOverride(SearchBoxWidth)
                        [SNew(SSearchBox)
                             .HintText(LOCTEXT("SearchHint", "Search"))
                             .ToolTipText(LOCTEXT(
                                 "SearchTip",
                                 "Filter entries by widget name, level, or "
                                 "checkpoint. Levels also match on their full "
                                 "package path, not only the shortened name "
                                 "the cell shows."))
                             .OnTextChanged(
                                 this,
                                 &SUIWidgetManager::OnSearchTextChanged)]]];

  return SNew(SVerticalBox) +
         SVerticalBox::Slot().AutoHeight()[ButtonRow] +
         SVerticalBox::Slot().AutoHeight()[SearchRow];
}

TSharedRef<SWidget> SUIWidgetManager::BuildListPanel()
{
  auto MakeColumn = [this](FName ColumnId, const FText &Label,
                           float FillWidth) -> SHeaderRow::FColumn::FArguments
  {
    return SHeaderRow::Column(ColumnId)
        .DefaultLabel(Label)
        .FillWidth(FillWidth)
        .SortMode(TAttribute<EColumnSortMode::Type>::CreateSP(
            this, &SUIWidgetManager::GetColumnSortMode, ColumnId))
        .OnSort(this, &SUIWidgetManager::OnColumnSortModeChanged);
  };

  const float DesiredListHeight =
      DesiredVisibleRows * SUIWTEntryRow::Height + HeaderRowHeight;

  return SNew(SVerticalBox) +
         SVerticalBox::Slot().AutoHeight()[BuildListToolbar()] +
         SVerticalBox::Slot().FillHeight(1.f)
             [SNew(SBox).MinDesiredHeight(DesiredListHeight)
                  [SNew(SOverlay) +
                   SOverlay::Slot()
                       [SAssignNew(ListView,
                                   SListView<TSharedPtr<FUIWTManagerEntry>>)
                            .ListItemsSource(&Entries)
                            .ScrollbarVisibility(EVisibility::Visible)
                            .ConsumeMouseWheel(
                                EConsumeMouseWheel::WhenScrollingPossible)
                            .OnGenerateRow(this,
                                           &SUIWidgetManager::OnGenerateRow)
                            .SelectionMode(ESelectionMode::Single)
                            .OnSelectionChanged(
                                this,
                                &SUIWidgetManager::OnListSelectionChanged)
                            .HeaderRow(
                                SNew(SHeaderRow) +
                                MakeColumn(UIWTManagerColumns::Widget,
                                           LOCTEXT("HWidget", "Widget"),
                                           WidgetColumnFillWidth) +
                                MakeColumn(UIWTManagerColumns::Level,
                                           LOCTEXT("HLevel", "Level"),
                                           LevelColumnFillWidth) +
                                MakeColumn(UIWTManagerColumns::LevelCheckpoint,
                                           LOCTEXT("HLevelCheckpoint",
                                                   "Level Checkpoint"),
                                           LevelCheckpointColumnFillWidth))] +
                   SOverlay::Slot()
                       .HAlign(HAlign_Center)
                       .VAlign(VAlign_Center)
                           [SNew(STextBlock)
                                .Visibility(
                                    this,
                                    &SUIWidgetManager::
                                        GetEmptySearchMessageVisibility)
                                .Text(this,
                                      &SUIWidgetManager::
                                          GetEmptySearchMessageText)
                                .ColorAndOpacity(
                                    FSlateColor::UseSubduedForeground())]]];
}

TSharedRef<SWidget> SUIWidgetManager::BuildSelectedEntryDetails()
{
  const TAttribute<FText> WidgetText = TAttribute<FText>::CreateSP(
      this, &SUIWidgetManager::GetSelectedCellText, UIWTManagerColumns::Widget);
  const TAttribute<FText> LevelText = TAttribute<FText>::CreateSP(
      this, &SUIWidgetManager::GetSelectedCellText, UIWTManagerColumns::Level);
  const TAttribute<FText> CheckpointText(
      this, &SUIWidgetManager::GetSelectedCheckpointText);

  auto MakeRow = [this](const FText &Label, TSharedRef<SWidget> Value)
      -> TSharedRef<SWidget>
  {
    return SNew(SUIWTDetailRow)
        .Label(Label)
        .NameFraction(this, &SUIWidgetManager::GetDetailNameFraction)
        .OnNameFractionChanged(this,
                               &SUIWidgetManager::SetDetailNameFraction)[Value];
  };

  // A value with one icon button after it.
  auto WithIconButton = [](TSharedRef<SWidget> Value, const FSlateBrush *Icon,
                           TAttribute<bool> IsEnabled, TAttribute<FText> ToolTip,
                           FOnClicked OnClicked) -> TSharedRef<SWidget>
  {
    return SNew(SHorizontalBox) +
           SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)[Value] +
           SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(4.f, 0.f, 0.f, 0.f))[MakeIconButton(Icon, IsEnabled, ToolTip, OnClicked)];
  };
  auto PluginIcon = [](FName IconName)
  { return FUIWidgetToolPluginStyle::Get().GetBrush(IconName); };

  // === Widget ===
  // A new entry with no widget picked shows a name box instead: confirming
  // creates its empty blueprint under the name typed there.
  TSharedRef<SWidget> WidgetValue = WithIconButton(
      SNew(SUIWTCopyableCell)
          .CopyText(WidgetText)
          .OnDoubleClicked(this, &SUIWidgetManager::BeginWidgetNameEdit)
              [SAssignNew(WidgetNameCell, SUIWTNameEditCell)
                   .OnCommitted(this, &SUIWidgetManager::RenameWidget)
                   .HintText(LOCTEXT("WidgetRenameHint", "New blueprint name"))
                       [SNew(SVerticalBox) +
                        SVerticalBox::Slot().AutoHeight()
                            [SNew(STextBlock)
                                 .Text(WidgetText)
                                 .AutoWrapText(true)
                                 .ColorAndOpacity(FSlateColor::UseForeground())
                                 .Visibility(this,
                                             &SUIWidgetManager::
                                                 GetWidgetTextVisibility)] +
                        SVerticalBox::Slot().AutoHeight()
                            [SAssignNew(NewWidgetNameBox, SEditableTextBox)
                                 .Text(this,
                                       &SUIWidgetManager::GetNewWidgetNameText)
                                 .HintText(LOCTEXT("NewWidgetNameHint",
                                                   "new widget name"))
                                 .ToolTipText(LOCTEXT(
                                     "NewWidgetNameTip",
                                     "The name of the widget blueprint "
                                     "created for this entry when it is "
                                     "confirmed. Leave empty for "
                                     "WBP_NewWidget. Enter confirms."))
                                 .Visibility(this,
                                             &SUIWidgetManager::
                                                 GetNewWidgetNameVisibility)
                                 .OnTextChanged(
                                     this,
                                     &SUIWidgetManager::OnNewWidgetNameChanged)
                                 .OnTextCommitted(
                                     this, &SUIWidgetManager::
                                               OnNewWidgetNameCommitted)]]],
      PluginIcon("UIWidgetTool.Icons.Blueprint"),
      TAttribute<bool>(this, &SUIWidgetManager::SelectedEntryHasWidget),
      LOCTEXT("BlueprintTip",
              "Open widget blueprint editor."),
      FOnClicked::CreateSP(this, &SUIWidgetManager::OnOpenBlueprintClicked));

  TSharedRef<SWidget> WidgetActionsValue =
      SNew(SHorizontalBox) +
      ButtonSlot(MakeIconButton(
          PluginIcon("UIWidgetTool.Icons.OpenFile"),
          TAttribute<bool>(this, &SUIWidgetManager::SelectedEntryHasWidget),
          LOCTEXT("RevealWidgetFileTip",
                  "Open widget blueprint file in file browser."),
          FOnClicked::CreateSP(this, &SUIWidgetManager::OnRevealWidgetFileClicked)));

  // === Checkpoints ===
  // Both pickers are filled in by RebuildPanelPickers.
  TSharedRef<SWidget> LevelValue = WithIconButton(
      SNew(SUIWTCopyableCell)
          .CopyText(LevelText)
              [SAssignNew(PanelLevelPickerBox, SBox)
                   .IsEnabled(this, &SUIWidgetManager::CanPanelPick)],
      PluginIcon("UIWidgetTool.Icons.OpenLevel"),
      TAttribute<bool>(this, &SUIWidgetManager::CanOpenSelectedLevel),
      TAttribute<FText>(this, &SUIWidgetManager::GetOpenLevelToolTip),
      FOnClicked::CreateSP(this, &SUIWidgetManager::OnOpenLevelClicked));

  // Rename is on a double-click of the picker, the Actions row's icon and the
  // right-click menu.
  TSharedRef<SWidget> CheckpointValue = WithIconButton(
      SNew(SUIWTCopyableCell)
          .CopyText(CheckpointText)
          .OnRename(this, &SUIWidgetManager::BeginCheckpointNameEdit)
          .CanRename(this, &SUIWidgetManager::CanRenameSelectedCheckpoint)
              [SAssignNew(CheckpointNameCell, SUIWTNameEditCell)
                   .OnCommitted(this, &SUIWidgetManager::RenameCheckpoint)
                   .HintText(LOCTEXT("RenameHint",
                                     "Leave empty for <map>_<captured at>"))
                       [SAssignNew(PanelCheckpointPickerBox, SBox)
                            .IsEnabled(this, &SUIWidgetManager::CanPanelPick)]],
      FAppStyle::GetBrush("Icons.Play"),
      TAttribute<bool>(this, &SUIWidgetManager::CanPlaySelected),
      TAttribute<FText>(this, &SUIWidgetManager::GetPlayToolTip),
      FOnClicked::CreateSP(this, &SUIWidgetManager::OnPlaySelectedClicked));

  TSharedRef<SWidget> ActionsValue =
      SNew(SHorizontalBox) +
      ButtonSlot(MakeIconButton(
          PluginIcon("UIWidgetTool.Icons.Rename"),
          TAttribute<bool>(this, &SUIWidgetManager::CanRenameSelectedCheckpoint),
          LOCTEXT("RenameCheckpointTip",
                  "Rename this checkpoint."),
          FOnClicked::CreateSPLambda(this,
                                     [this]
                                     {
                                       BeginCheckpointNameEdit();
                                       return FReply::Handled();
                                     }))) +
      ButtonSlot(MakeIconButton(
          FAppStyle::GetBrush("Icons.BrowseContent"),
          TAttribute<bool>(this, &SUIWidgetManager::CanSnapshotSelected),
          TAttribute<FText>(this, &SUIWidgetManager::GetSnapshotToolTip),
          FOnClicked::CreateSP(this,
                               &SUIWidgetManager::OnSnapshotSelectedClicked))) +
      ButtonSlot(MakeIconButton(
          FAppStyle::GetBrush("Icons.FolderOpen"),
          TAttribute<bool>(this, &SUIWidgetManager::CanRevealCheckpointFile),
          TAttribute<FText>(this,
                            &SUIWidgetManager::GetRevealCheckpointFileToolTip),
          FOnClicked::CreateSP(this,
                               &SUIWidgetManager::OnRevealCheckpointFileClicked)));

  // === Note ===
  TSharedRef<SWidget> NoteValue =
      SAssignNew(NoteBox, SEditableTextBox)
          .Text(this, &SUIWidgetManager::GetSelectedNoteEditText)
          .HintText(this, &SUIWidgetManager::GetNoteHintText)
          .IsReadOnly(this, &SUIWidgetManager::IsNoteReadOnly)
          .RevertTextOnEscape(true)
          .ToolTipText(LOCTEXT("NoteBoxTip",
                               "A note on this entry, shown in the list and "
                               "matched by the search. Enter or clicking away "
                               "saves it; Esc discards the change."))
          .OnTextChanged(this, &SUIWidgetManager::OnNoteTextChanged)
          .OnTextCommitted(this, &SUIWidgetManager::OnNoteTextCommitted);

  return SNew(SVerticalBox) +
         SVerticalBox::Slot().AutoHeight()[UIWTDetails::MakeCategory(
             LOCTEXT("DetailsWidgetCategory", "Widget"),
             {MakeRow(LOCTEXT("DetailWidget", "Widget"), WidgetValue),
              MakeRow(LOCTEXT("DetailActions", "Actions"), WidgetActionsValue)})] +
         SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 1.f, 0.f, 0.f))
             [UIWTDetails::MakeCategory(
                 LOCTEXT("DetailsCheckpointsCategory", "Checkpoints"),
                 {MakeRow(LOCTEXT("DetailLevel", "Level"), LevelValue),
                  MakeRow(LOCTEXT("DetailCheckpoint", "Checkpoint"),
                          CheckpointValue),
                  MakeRow(LOCTEXT("DetailActions", "Checkpoint Actions"), ActionsValue)})] +
         SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 1.f, 0.f, 0.f))
             [UIWTDetails::MakeCategory(
                 LOCTEXT("DetailsNoteCategory", "Note"),
                 {MakeRow(LOCTEXT("DetailNote", "Note"), NoteValue)})];
}

TSharedRef<SWidget> SUIWidgetManager::MakeUtilityPanel()
{
  TSharedRef<SWidget> Panel = BuildUtilityPanel();
  // The pickers are only filled in on a selection change or refresh.
  RebuildPanelPickers();
  return Panel;
}

TSharedRef<SWidget> SUIWidgetManager::BuildUtilityPanel()
{
  return SNew(SHorizontalBox) +
         SHorizontalBox::Slot().AutoWidth().Padding(
             FMargin(2.f, 2.f, 4.f, 2.f))
             [SNew(SBorder)
                  .BorderImage(FAppStyle::GetBrush("DetailsView.GridLine"))
                  .Padding(0.f)
                      [SNew(SBox)
                           .WidthOverride(InformationPanelWidth)
                               [SNew(SScrollBox) +
                                SScrollBox::Slot()
                                    [BuildSelectedEntryDetails()]]]] +
         SHorizontalBox::Slot().FillWidth(1.f).Padding(
             FMargin(4.f, 2.f, 2.f, 2.f))
             [SNew(SBorder)
                  .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
                  .Padding(FMargin(6.f))[UtilityPanelContent]];
}

void SUIWidgetManager::OnCheckpointDirectoryChanged()
{
  CheckpointIndex.Rebuild();
  RefreshList();
}

FText SUIWidgetManager::GetCheckpointDirectoryText() const
{
  const FString &Directory = CheckpointIndex.GetDirectory();
  const int32 Invalid = CheckpointIndex.NumInvalid();

  if (!CheckpointIndex.DirectoryExists())
  {
    return FText::Format(
        LOCTEXT("CheckpointDirMissing",
                "Checkpoints: {0} (does not exist yet - it is created by the "
                "first capture)"),
        FText::FromString(Directory));
  }
  if (Invalid > 0)
  {
    return FText::Format(
        LOCTEXT("CheckpointDirWithErrors",
                "Checkpoints: {0} - {1} indexed, {2} unreadable"),
        FText::FromString(Directory),
        FText::AsNumber(CheckpointIndex.GetEntries().Num() - Invalid),
        FText::AsNumber(Invalid));
  }
  return FText::Format(
      LOCTEXT("CheckpointDir", "Checkpoints: {0} - {1} indexed"),
      FText::FromString(Directory),
      FText::AsNumber(CheckpointIndex.GetEntries().Num()));
}

void SUIWidgetManager::RefreshAll()
{
  CheckpointIndex.Rebuild();
  CheckpointWatcher.Watch(CheckpointIndex.GetDirectory());
  RunLevelScan();
  RefreshList();
}

void SUIWidgetManager::RunLevelScan()
{
  const UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();

  TArray<FUIWTLevelScanRequest> Requests;
  Requests.Reserve(Settings->WidgetPreviewObjects.Num());
  for (const FWidgetPreviewObject &PreviewObject : Settings->WidgetPreviewObjects)
  {
    FUIWTLevelScanRequest &Request = Requests.AddDefaulted_GetRef();
    Request.EntryId = PreviewObject.Id;
    const TSharedPtr<FAssetData> Pending =
        EditSession.GetPendingWidget(PreviewObject.Id);
    Request.WidgetPackageName = Pending.IsValid()
                                    ? Pending->PackageName
                                    : GetWidgetPackageName(PreviewObject);
  }

  ScanResults.Reset();
  for (FUIWTLevelScanResult &Result : UIWTLevelScan::Scan(Requests))
  {
    const FGuid EntryId = Result.EntryId;
    ScanResults.Add(EntryId, MoveTemp(Result));
  }

  for (const FGuid &EditingId : EditSession.GetEditingIds())
  {
    EditSession.BuildLevelOptions(EditingId, ScanResults, CheckpointIndex);
  }
}

void SUIWidgetManager::OnListSelectionChanged(
    TSharedPtr<FUIWTManagerEntry> Item, ESelectInfo::Type SelectInfo)
{
  if (!Item.IsValid() && SelectInfo == ESelectInfo::Direct)
  {
    return;
  }

  if (!Item.IsValid() && !EditSession.IsEmpty())
  {
    ConfirmEditingEntries();
    return;
  }

  SelectedEntryId = Item.IsValid() ? Item->Id : FGuid();
  NotifySelectionChanged();
}

void SUIWidgetManager::ConfirmEditingEntries()
{
  for (const FGuid &Id : EditSession.GetEditingIds())
  {
    OnConfirmClicked(Id);
  }
}

void SUIWidgetManager::ConfirmEditIfEditing(const FGuid &InEntryId)
{
  if (EditSession.IsEditing(InEntryId))
  {
    OnConfirmClicked(InEntryId);
  }
  // A typed name that could not be used leaves the entry editing; the run
  // must not start from that, so fall back to the default name.
  if (EditSession.IsEditing(InEntryId))
  {
    NewWidgetNames.Remove(InEntryId);
    OnConfirmClicked(InEntryId);
  }
}

void SUIWidgetManager::RestoreSelection(const FGuid &InEntryId,
                                        bool bScrollIntoView)
{
  if (!ListView.IsValid())
  {
    return;
  }

  TSharedPtr<FUIWTManagerEntry> Target = FindEntry(InEntryId);
  const bool bRestored = Target.IsValid();

  if (!Target.IsValid() && Entries.Num() > 0)
  {
    Target = Entries[0];
  }

  if (!Target.IsValid())
  {
    ListView->ClearSelection();
    SelectedEntryId = FGuid();
    NotifySelectionChanged();
    return;
  }

  ListView->SetSelection(Target, ESelectInfo::Direct);
  SelectedEntryId = Target->Id;
  NotifySelectionChanged();
  if (bRestored && bScrollIntoView)
  {
    ListView->RequestScrollIntoView(Target);
  }
}

SUIWidgetManager::FSelectionSignature
SUIWidgetManager::MakeSelectionSignature() const
{
  FSelectionSignature Signature;
  Signature.EntryId = SelectedEntryId;
  Signature.SnapshotPath = GetSelectedSnapshotPath();
  if (const FWidgetPreviewObject *PreviewObject = FindSelectedPreviewObject())
  {
    Signature.WidgetClass = PreviewObject->WidgetClass.ToSoftObjectPath();
  }
  return Signature;
}

// A run editing the selected copy in place moves none of these; the owner
// hears about that from the chat service instead.
void SUIWidgetManager::NotifySelectionChanged()
{
  RebuildPanelPickers();
  UpdateNewWidgetNameError();

  FSelectionSignature Signature = MakeSelectionSignature();
  if (Signature != NotifiedSelection)
  {
    NotifiedSelection = MoveTemp(Signature);
    OnSelectionChanged.ExecuteIfBound();
  }
}

TSharedPtr<FUIWTManagerEntry>
SUIWidgetManager::FindEntry(const FGuid &InEntryId) const
{
  if (!InEntryId.IsValid())
  {
    return nullptr;
  }
  const TSharedPtr<FUIWTManagerEntry> *Found = Entries.FindByPredicate(
      [&](const TSharedPtr<FUIWTManagerEntry> &Entry)
      { return Entry->Id == InEntryId; });
  return Found ? *Found : nullptr;
}

TSharedPtr<FUIWTManagerEntry> SUIWidgetManager::FindSelectedEntry() const
{
  return FindEntry(SelectedEntryId);
}

const FWidgetPreviewObject *SUIWidgetManager::FindSelectedPreviewObject() const
{
  if (!SelectedEntryId.IsValid())
  {
    return nullptr;
  }
  return UUIWidgetPreviewObjectManagerSettings::Get()->FindWidgetPreviewObject(
      SelectedEntryId);
}

void SUIWidgetManager::OnSearchTextChanged(const FText &NewText)
{
  ListQuery.SearchText = NewText.ToString().TrimStartAndEnd();
  RefreshList();
}

FText SUIWidgetManager::GetSearchFieldLabel() const
{
  for (const FSearchFieldOption &Option : GetSearchFieldOptions())
  {
    if (Option.Field == ListQuery.SearchField)
    {
      return Option.Label;
    }
  }
  return FText::GetEmpty();
}

EVisibility SUIWidgetManager::GetSearchFieldLabelVisibility() const
{
  return ListQuery.SearchField == EUIWTSearchField::All ? EVisibility::Collapsed
                                                        : EVisibility::Visible;
}

FText SUIWidgetManager::GetFilterToolTip() const
{
  return FText::Format(
      LOCTEXT("FilterTip", "Choose which column the search box matches "
                           "against.\nCurrently: {0}"),
      GetSearchFieldLabel());
}

bool SUIWidgetManager::IsSearchField(EUIWTSearchField InField) const
{
  return ListQuery.SearchField == InField;
}

void SUIWidgetManager::SetSearchField(EUIWTSearchField InField)
{
  if (ListQuery.SearchField == InField)
  {
    return;
  }
  ListQuery.SearchField = InField;
  if (!ListQuery.SearchText.IsEmpty())
  {
    RefreshList();
  }
}

TSharedRef<SWidget> SUIWidgetManager::BuildFilterMenu()
{
  FMenuBuilder MenuBuilder(true, nullptr);

  MenuBuilder.BeginSection("SearchField",
                           LOCTEXT("SearchFieldSection", "Search in"));

  for (const FSearchFieldOption &Option : GetSearchFieldOptions())
  {
    MenuBuilder.AddMenuEntry(
        Option.Label, FText::GetEmpty(), FSlateIcon(),
        FUIAction(FExecuteAction::CreateSP(
                      this, &SUIWidgetManager::SetSearchField, Option.Field),
                  FCanExecuteAction(),
                  FIsActionChecked::CreateSP(
                      this, &SUIWidgetManager::IsSearchField, Option.Field)),
        NAME_None, EUserInterfaceActionType::RadioButton);
  }

  MenuBuilder.EndSection();
  return MenuBuilder.MakeWidget();
}

EVisibility SUIWidgetManager::GetEmptySearchMessageVisibility() const
{
  return Entries.Num() == 0 ? EVisibility::HitTestInvisible
                            : EVisibility::Collapsed;
}

FText SUIWidgetManager::GetEmptySearchMessageText() const
{
  if (ListQuery.SearchText.IsEmpty())
  {
    return LOCTEXT("NoEntries",
                   "No widget entries yet. Use New to add one.");
  }
  return FText::Format(
      LOCTEXT("NoSearchMatches", "No entries match \"{0}\" in {1}."),
      FText::FromString(ListQuery.SearchText), GetSearchFieldLabel());
}

void SUIWidgetManager::RefreshList()
{
  const TSharedPtr<FUIWTManagerEntry> PreviousEntry = FindSelectedEntry();
  const bool bScrollSelectionIntoView =
      !PreviousEntry.IsValid() ||
      (ListView.IsValid() && ListView->IsItemVisible(PreviousEntry));

  FUIWTManagerListQuery Query = ListQuery;
  Query.PinnedEntryId = PreviousEntry.IsValid() ? PreviousEntry->Id : FGuid();
  Entries = UIWTManagerListBuilder::Build(
      UUIWidgetPreviewObjectManagerSettings::Get()->WidgetPreviewObjects,
      CheckpointIndex, ScanResults, EditSession, Query);

  if (ListView.IsValid())
  {
    ListView->RequestListRefresh();
  }
  RestoreSelection(Query.PinnedEntryId, bScrollSelectionIntoView);
}

EColumnSortMode::Type
SUIWidgetManager::GetColumnSortMode(FName ColumnId) const
{
  const TOptional<EUIWidgetSortField> Field = SortFieldForColumn(ColumnId);
  if (!Field.IsSet() || Field.GetValue() != ListQuery.SortField)
  {
    return EColumnSortMode::None;
  }
  return ListQuery.bSortAscending ? EColumnSortMode::Ascending
                                  : EColumnSortMode::Descending;
}

void SUIWidgetManager::OnColumnSortModeChanged(
    EColumnSortPriority::Type, const FName &ColumnId,
    EColumnSortMode::Type NewSortMode)
{
  const TOptional<EUIWidgetSortField> Field = SortFieldForColumn(ColumnId);
  if (!Field.IsSet())
  {
    return;
  }

  const bool bThirdClick = Field.GetValue() == ListQuery.SortField &&
                           !ListQuery.bSortAscending &&
                           NewSortMode == EColumnSortMode::Ascending;
  ListQuery.SortField = bThirdClick ? EUIWidgetSortField::None : Field.GetValue();
  ListQuery.bSortAscending = NewSortMode != EColumnSortMode::Descending;
  RefreshList();
}

TSharedRef<ITableRow>
SUIWidgetManager::OnGenerateRow(TSharedPtr<FUIWTManagerEntry> Item,
                                const TSharedRef<STableViewBase> &Owner)
{
  return SNew(SUIWTEntryRow, Owner).Entry(Item).Owner(this);
}

FReply SUIWidgetManager::OnUpdateClicked(FGuid Id)
{
  SelectedEntryId = FindEntry(Id).IsValid() ? Id : FGuid();

  EndFieldPick(Id);
  EditSession.Begin(Id);

  RunLevelScan();

  RefreshList();
  if (SelectedEntryId != Id)
  {
    RestoreSelection(Id, true);
  }
  return FReply::Handled();
}

void SUIWidgetManager::SetPendingLevel(FGuid Id,
                                       TSharedPtr<FUIWTLevelOption> Selection)
{
  EditSession.SetPendingLevel(Id, Selection);
  RefreshList();
}

void SUIWidgetManager::SetPendingWidget(FGuid Id,
                                        TSharedPtr<FAssetData> Selection)
{
  EditSession.SetPendingWidget(Id, Selection);
  RunLevelScan();
  RefreshList();
}

void SUIWidgetManager::BeginCellEdit(FGuid Id, FName Column)
{
  if (!Id.IsValid())
  {
    return;
  }
  EditSession.RequestAutoOpenPicker(Id, Column);
  if (EditSession.IsEditing(Id))
  {
    RefreshList();
  }
  else
  {
    OnUpdateClicked(Id);
  }
}

bool SUIWidgetManager::CanRenameSelectedCheckpoint() const
{
  const TSharedPtr<FUIWTManagerEntry> Entry = FindSelectedEntry();
  return Entry.IsValid() && !EditSession.IsEditing(Entry->Id) &&
         CheckpointIndex.FindValid(Entry->CheckpointId) != nullptr;
}

void SUIWidgetManager::BeginWidgetNameEdit()
{
  const FWidgetPreviewObject *PreviewObject = FindSelectedPreviewObject();
  if (!PreviewObject || !WidgetNameCell.IsValid() ||
      EditSession.IsEditing(PreviewObject->Id))
  {
    return;
  }
  if (PreviewObject->WidgetClass.IsNull())
  {
    UIWTNotify::Show(LOCTEXT("RenameWidgetNone",
                             "No widget to rename - pick one first."),
                     false);
    return;
  }
  WidgetNameCell->BeginEdit(PreviewObject->Id, PreviewObject->WidgetName);
}

void SUIWidgetManager::RenameWidget(FGuid EntryId, const FText &NewName)
{
  UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();
  FWidgetPreviewObject *PreviewObject = Settings->FindWidgetPreviewObject(EntryId);
  if (!PreviewObject)
  {
    return;
  }

  const FString NewLabel = NewName.ToString().TrimStartAndEnd();
  if (NewLabel.IsEmpty())
  {
    UIWTNotify::Show(LOCTEXT("RenameWidgetEmpty",
                             "The widget name cannot be empty."),
                     false);
    return;
  }
  // The run may be editing this blueprint, and renaming moves its folder.
  if (IsRunInFlight.Get(false))
  {
    UIWTNotify::Show(
        LOCTEXT("RenameDuringRun",
                "A run is editing a blueprint. Wait for it to finish, or "
                "cancel it, before renaming a widget."),
        false);
    return;
  }

  UWidgetBlueprint *Blueprint =
      UIWTGenerated::FindWidgetBlueprint(PreviewObject->WidgetClass);
  if (!Blueprint)
  {
    UIWTNotify::Show(LOCTEXT("RenameWidgetGone",
                             "The entry's blueprint could not be loaded."),
                     false);
    return;
  }

  const TSoftClassPtr<UUserWidget> OldClass = PreviewObject->WidgetClass;

  FText Error;
  if (!UIWTGenerated::RenameWidgetBlueprint(Blueprint, NewLabel, Error))
  {
    UIWTNotify::Show(
        FText::Format(LOCTEXT("RenameWidgetFailed", "Could not rename: {0}"),
                      Error),
        false);
    return;
  }

  // Hand-added entries may share one blueprint; repoint every entry that
  // used it, not just the one renamed from.
  for (FWidgetPreviewObject &Other : Settings->WidgetPreviewObjects)
  {
    if (Other.WidgetClass == OldClass)
    {
      Other.WidgetName = Blueprint->GetName();
      if (Blueprint->GeneratedClass)
      {
        Other.WidgetClass = TSoftClassPtr<UUserWidget>(Blueprint->GeneratedClass);
      }
    }
  }
  Settings->SaveWidgetPreviewObjects();

  // The package name the scan and the viewer key on has changed.
  RunLevelScan();
  RefreshList();
  PushSelectionToViewer();

  UIWTNotify::Show(
      FText::Format(LOCTEXT("RenameWidgetDone", "Widget renamed to \"{0}\"."),
                    FText::FromString(PreviewObject->WidgetName)),
      true);
}

bool SUIWidgetManager::ApplyWidgetSelection(
    FWidgetPreviewObject &PreviewObject,
    const TSharedPtr<FAssetData> &Selection)
{
  if (UIWTManagerOptions::IsNoneWidgetOption(Selection))
  {
    if (PreviewObject.WidgetClass.IsNull() && PreviewObject.WidgetName.IsEmpty())
    {
      return false;
    }
    PreviewObject.WidgetName.Reset();
    PreviewObject.WidgetClass = TSoftClassPtr<UUserWidget>();
    return true;
  }

  // Both fields or neither: a name pointing at a blueprint the class does not
  // match would be saved and shown as if the pick had worked.
  const UWidgetBlueprint *WBP = Cast<UWidgetBlueprint>(Selection->GetAsset());
  if (!WBP || !WBP->GeneratedClass)
  {
    return false;
  }
  PreviewObject.WidgetName = Selection->AssetName.ToString();
  PreviewObject.WidgetClass = TSoftClassPtr<UUserWidget>(WBP->GeneratedClass);
  return true;
}

bool SUIWidgetManager::ApplyLevelSelection(
    FWidgetPreviewObject &PreviewObject,
    const TSharedPtr<FUIWTLevelOption> &Level)
{
  const FName LevelPath =
      UIWTManagerOptions::IsNoneLevelOption(Level) ? NAME_None
                                                   : Level->PackagePath;
  if (PreviewObject.LevelPackagePath == LevelPath)
  {
    return false;
  }
  PreviewObject.LevelPackagePath = LevelPath;
  return true;
}

FReply SUIWidgetManager::OnConfirmClicked(FGuid Id)
{
  bool bClassChanged = false;
  const bool bNewEntry = NewEntryIds.Contains(Id);
  const FString NewWidgetName = NewWidgetNames.FindRef(Id);
  UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();
  if (FWidgetPreviewObject *PreviewObject = Settings->FindWidgetPreviewObject(Id))
  {
    const TSharedPtr<FAssetData> Selection = EditSession.GetPendingWidget(Id);
    if (bNewEntry && UIWTManagerOptions::IsNoneWidgetOption(Selection))
    {
      bClassChanged = AssignNewWidgetBlueprint(*PreviewObject, NewWidgetName);
      // A typed name that could not be used stays in the box to be fixed.
      if (!bClassChanged && !NewWidgetName.IsEmpty())
      {
        return FReply::Handled();
      }
    }
    else
    {
      bClassChanged = ApplyWidgetSelection(*PreviewObject, Selection);
      if (!bClassChanged && !UIWTManagerOptions::IsNoneWidgetOption(Selection))
      {
        NotifyWidgetPickFailed(Selection);
      }
    }

    const bool bLevelChanged =
        ApplyLevelSelection(*PreviewObject, EditSession.GetPendingLevel(Id));
    if (bClassChanged || bLevelChanged)
    {
      Settings->SaveWidgetPreviewObjects();
    }
  }
  NewEntryIds.Remove(Id);
  NewWidgetNames.Remove(Id);
  EditSession.End(Id);

  if (bClassChanged)
  {
    RunLevelScan();
  }
  RefreshList();
  return FReply::Handled();
}

void SUIWidgetManager::BeginFieldPick(FGuid Id, FName Column)
{
  if (!Id.IsValid() || EditSession.IsEditing(Id))
  {
    return;
  }
  EndFieldPick(FieldPickEntryId);

  if (Column == UIWTManagerColumns::Widget)
  {
    FieldPickWidgetOptions =
        FUIWTEntryEditSession::MakeWidgetOptions(Id, FieldPickWidget);
  }
  else if (Column == UIWTManagerColumns::Level)
  {
    FieldPickLevelOptions = FUIWTEntryEditSession::MakeLevelOptions(
        Id, ScanResults, CheckpointIndex, nullptr, FieldPickLevel);
  }
  else if (Column != UIWTManagerColumns::LevelCheckpoint)
  {
    return;
  }

  FieldPickEntryId = Id;
  FieldPickColumn = Column;
  RefreshList();
}

void SUIWidgetManager::EndFieldPick(const FGuid &Id)
{
  if (!Id.IsValid() || FieldPickEntryId != Id)
  {
    return;
  }
  FieldPickEntryId.Invalidate();
  FieldPickColumn = NAME_None;
  FieldPickWidgetOptions.Reset();
  FieldPickWidget.Reset();
  FieldPickLevelOptions.Reset();
  FieldPickLevel.Reset();
}

void SUIWidgetManager::OnFieldPickMenuOpenChanged(bool bIsOpen, FGuid Id,
                                                  FName Column)
{
  if (bIsOpen || !IsFieldPickOpen(Id, Column))
  {
    return;
  }
  EndFieldPick(Id);
  RefreshList();
}

TSharedRef<SWidget> SUIWidgetManager::MakePicker(FGuid Id, FName Column)
{
  using UIWTSearchablePicker::MakeSearchablePicker;

  const bool bEditing = EditSession.IsEditing(Id);
  // An edit picker opens itself only when a double-click asked for it; a
  // field pick always opens, and the menu closing ends the pick.
  const bool bOpenOnShow =
      bEditing ? EditSession.TakeAutoOpenPicker(Id, Column) : true;
  const FOnIsOpenChanged OnMenuOpenChanged =
      bEditing ? FOnIsOpenChanged()
               : FOnIsOpenChanged::CreateSP(
                     this, &SUIWidgetManager::OnFieldPickMenuOpenChanged, Id,
                     Column);

  if (Column == UIWTManagerColumns::Widget)
  {
    FUIWTPickerArgs<FAssetData> Args;
    Args.ToolTip = LOCTEXT("PickWidgetTip", "Choose the widget blueprint to test.");
    Args.EmptyText =
        LOCTEXT("NoWidgetsToPick", "No widget blueprints found in the project.");
    if (bEditing)
    {
      Args.Options = EditSession.GetWidgetOptions(Id);
      Args.GetCurrent = [this, Id]
      { return EditSession.GetPendingWidget(Id); };
      Args.OnPicked = [this, Id](TSharedPtr<FAssetData> Picked)
      { SetPendingWidget(Id, Picked); };
    }
    else
    {
      Args.Options = &FieldPickWidgetOptions;
      Args.GetCurrent = [this]
      { return FieldPickWidget; };
      Args.OnPicked = [this, Id](TSharedPtr<FAssetData> Picked)
      { PickWidget(Id, Picked); };
    }
    Args.bOpenOnShow = bOpenOnShow;
    Args.OnMenuOpenChanged = OnMenuOpenChanged;
    return MakeSearchablePicker(MoveTemp(Args));
  }

  if (Column == UIWTManagerColumns::Level)
  {
    FUIWTPickerArgs<FUIWTLevelOption> Args;
    Args.ToolTip = LOCTEXT(
        "PickLevelTip",
        "Choose which level this widget is tested on. Levels that reference "
        "the widget are listed when the scan found any; otherwise every level "
        "in the project is.");
    Args.EmptyText = LOCTEXT("NoLevelsToPick", "No levels found in the project.");
    if (bEditing)
    {
      Args.Options = EditSession.GetLevelOptions(Id);
      Args.GetCurrent = [this, Id]
      { return EditSession.GetPendingLevel(Id); };
      Args.OnPicked = [this, Id](TSharedPtr<FUIWTLevelOption> Picked)
      { SetPendingLevel(Id, Picked); };
    }
    else
    {
      Args.Options = &FieldPickLevelOptions;
      Args.GetCurrent = [this]
      { return FieldPickLevel; };
      Args.OnPicked = [this, Id](TSharedPtr<FUIWTLevelOption> Picked)
      { PickLevel(Id, Picked); };
    }
    Args.bOpenOnShow = bOpenOnShow;
    Args.OnMenuOpenChanged = OnMenuOpenChanged;
    return MakeSearchablePicker(MoveTemp(Args));
  }

  // Level Checkpoint: the same picker whether editing or not, since a
  // checkpoint pick is written at once either way.
  const TSharedPtr<FUIWTManagerEntry> Entry = FindEntry(Id);
  const bool bHasLevel = !GetCheckpointPickLevel(Id).IsNone();

  TSharedRef<FUIWTCheckpointOption> Current =
      MakeShared<FUIWTCheckpointOption>();
  if (Entry.IsValid())
  {
    Current->Id = Entry->CheckpointId;
    Current->Display = Entry->CheckpointDisplay;
  }
  const TArray<TSharedPtr<FUIWTCheckpointOption>> Options =
      BuildEntryCheckpointOptions(Id);

  FUIWTPickerArgs<FUIWTCheckpointOption> Args;
  Args.Options = &Options;
  Args.ToolTip =
      bHasLevel ? LOCTEXT("PickCheckpointTip",
                          "Choose a checkpoint captured on this level.")
                : LOCTEXT("PickCheckpointNoLevelTip",
                          "Choose any checkpoint. Once a level is chosen, "
                          "only checkpoints captured on it are offered.");
  Args.EmptyText =
      bHasLevel ? LOCTEXT("NoCompatible",
                          "No checkpoints captured on this level yet.")
                : LOCTEXT("NoCheckpointsAtAll", "No checkpoints captured yet.");
  Args.GetCurrent = [Current]() -> TSharedPtr<FUIWTCheckpointOption>
  { return Current; };
  Args.OnPicked = [this, Id](TSharedPtr<FUIWTCheckpointOption> Picked)
  { AssignCheckpoint(Id, Picked.IsValid() ? Picked->Id : FGuid()); };
  Args.ButtonColor = Entry.IsValid()
                         ? UIWTManagerColumns::CheckpointCellColor(*Entry)
                         : FSlateColor::UseSubduedForeground();
  Args.bOpenOnShow = bOpenOnShow;
  Args.OnMenuOpenChanged = OnMenuOpenChanged;
  return MakeSearchablePicker(MoveTemp(Args));
}

void SUIWidgetManager::PickWidget(FGuid Id, TSharedPtr<FAssetData> Selection)
{
  UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();
  bool bClassChanged = false;
  if (FWidgetPreviewObject *PreviewObject = Settings->FindWidgetPreviewObject(Id))
  {
    bClassChanged = ApplyWidgetSelection(*PreviewObject, Selection);
    if (bClassChanged)
    {
      Settings->SaveWidgetPreviewObjects();
    }
    else if (!UIWTManagerOptions::IsNoneWidgetOption(Selection))
    {
      NotifyWidgetPickFailed(Selection);
    }
  }
  EndFieldPick(Id);

  if (bClassChanged)
  {
    RunLevelScan();
  }
  RefreshList();
}

void SUIWidgetManager::PickLevel(FGuid Id, TSharedPtr<FUIWTLevelOption> Selection)
{
  UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();
  if (FWidgetPreviewObject *PreviewObject = Settings->FindWidgetPreviewObject(Id))
  {
    if (ApplyLevelSelection(*PreviewObject, Selection))
    {
      Settings->SaveWidgetPreviewObjects();
    }
  }
  EndFieldPick(Id);
  RefreshList();
}

FReply SUIWidgetManager::OnAddWidgetClicked()
{
  UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();
  const FGuid NewId =
      Settings->AddWidgetPreviewObject(TSoftClassPtr<UUserWidget>(), FString());
  NewEntryIds.Add(NewId);

  RefreshList();
  return OnUpdateClicked(NewId);
}

bool SUIWidgetManager::AssignNewWidgetBlueprint(
    FWidgetPreviewObject &PreviewObject, const FString &InName)
{
  FText Error;
  UWidgetBlueprint *Blueprint =
      UIWTGenerated::CreateEmptyWidgetBlueprint(Error, InName);
  if (!Blueprint || !Blueprint->GeneratedClass)
  {
    UIWTNotify::Show(
        FText::Format(LOCTEXT("NewWidgetFailed",
                              "Could not create a widget for the new entry: "
                              "{0}"),
                      Error),
        false);
    return false;
  }
  PreviewObject.WidgetName = Blueprint->GetName();
  PreviewObject.WidgetClass = TSoftClassPtr<UUserWidget>(Blueprint->GeneratedClass);
  UIWTNotify::Show(
      FText::Format(LOCTEXT("NewWidgetDone", "Created {0}."),
                    FText::FromString(Blueprint->GetName())),
      true);
  return true;
}

bool SUIWidgetManager::IsNamingNewWidget() const
{
  return IsSelectedEntryEditing() && NewEntryIds.Contains(SelectedEntryId) &&
         UIWTManagerOptions::IsNoneWidgetOption(
             EditSession.GetPendingWidget(SelectedEntryId));
}

EVisibility SUIWidgetManager::GetNewWidgetNameVisibility() const
{
  return IsNamingNewWidget() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SUIWidgetManager::GetWidgetTextVisibility() const
{
  return IsNamingNewWidget() ? EVisibility::Collapsed : EVisibility::Visible;
}

FText SUIWidgetManager::GetNewWidgetNameText() const
{
  return FText::FromString(NewWidgetNames.FindRef(SelectedEntryId));
}

void SUIWidgetManager::UpdateNewWidgetNameError()
{
  if (!NewWidgetNameBox.IsValid())
  {
    return;
  }
  const FString Name = NewWidgetNames.FindRef(SelectedEntryId);
  FText Error;
  if (IsNamingNewWidget() && !Name.IsEmpty() &&
      !UIWTGenerated::CheckNewWidgetName(Name, Error))
  {
    NewWidgetNameBox->SetError(Error);
  }
  else
  {
    NewWidgetNameBox->SetError(FText::GetEmpty());
  }
}

void SUIWidgetManager::OnNewWidgetNameChanged(const FText &NewText)
{
  if (!IsNamingNewWidget())
  {
    return;
  }
  const FString Name = NewText.ToString().TrimStartAndEnd();
  if (Name.IsEmpty())
  {
    NewWidgetNames.Remove(SelectedEntryId);
  }
  else
  {
    NewWidgetNames.Add(SelectedEntryId, Name);
  }
  UpdateNewWidgetNameError();
}

void SUIWidgetManager::OnNewWidgetNameCommitted(const FText &NewText,
                                                ETextCommit::Type CommitType)
{
  if (CommitType == ETextCommit::OnEnter && IsNamingNewWidget())
  {
    OnConfirmClicked(SelectedEntryId);
  }
}

FReply SUIWidgetManager::OnRefreshClicked()
{
  RefreshAll();
  return FReply::Handled();
}

FReply SUIWidgetManager::OnOpenSaveDirectoryClicked()
{
  // Explorer ignores the relative, forward-slash path ProjectSavedDir gives;
  // it needs the full native form.
  const FString SaveDir =
      FPaths::ConvertRelativePathToFull(UIWT::GetSavePath());

  IFileManager &FileManager = IFileManager::Get();
  if (!FileManager.DirectoryExists(*SaveDir) &&
      !FileManager.MakeDirectory(*SaveDir, true))
  {
    UIWTNotify::Show(
        FText::Format(LOCTEXT("OpenSaveDirCreateFailed",
                              "Could not create the save directory {0}."),
                      FText::FromString(SaveDir)),
        false);
    return FReply::Handled();
  }

  FString NativeSaveDir = SaveDir;
  FPaths::MakePlatformFilename(NativeSaveDir);
  FPlatformProcess::ExploreFolder(*NativeSaveDir);
  return FReply::Handled();
}

FReply SUIWidgetManager::OnLoadSnapshotClicked(
    TSharedPtr<FUIWTManagerEntry> Entry)
{
  const FUIWTCheckpointIndexEntry *Checkpoint = FindEntrySnapshot(Entry);
  if (!Checkpoint)
  {
    UIWTNotify::Show(
        LOCTEXT("SnapshotGone",
                "That checkpoint no longer has a snapshot on disk."),
        false);
    return FReply::Handled();
  }

  if (!OnOpenSnapshot.IsBound())
  {
    return FReply::Handled();
  }

  FText Error;
  if (!OnOpenSnapshot.Execute(Checkpoint->SnapshotPath, Error))
  {
    UIWTNotify::Show(Error, false);
    return FReply::Handled();
  }

  UIWTNotify::Show(
      LOCTEXT("SnapshotLoaded", "Snapshot loaded."),
      true);
  return FReply::Handled();
}

const FUIWTCheckpointIndexEntry *
SUIWidgetManager::FindEntrySnapshot(const TSharedPtr<FUIWTManagerEntry> &Entry) const
{
  if (!Entry.IsValid() || !Entry->CheckpointId.IsValid())
  {
    return nullptr;
  }
  const FUIWTCheckpointIndexEntry *Checkpoint = CheckpointIndex.Find(Entry->CheckpointId);
  return (Checkpoint && Checkpoint->HasSnapshot()) ? Checkpoint : nullptr;
}

FString SUIWidgetManager::GetSelectedSnapshotPath() const
{
  const FUIWTCheckpointIndexEntry *Checkpoint =
      FindEntrySnapshot(FindSelectedEntry());
  return Checkpoint ? Checkpoint->SnapshotPath : FString();
}

// The toolbar button is disabled while a run is in flight; the row's context
// menu reaches this directly, so the refusal lives here too. Duplicating the
// copy a run is editing would capture it mid-edit.
FReply SUIWidgetManager::OnDuplicateClicked(FGuid SourceId)
{
  if (IsRunInFlight.Get(false))
  {
    UIWTNotify::Show(
        LOCTEXT("DuplicateDuringRun",
                "A run is editing a blueprint. Wait for it to finish, or "
                "cancel it, before duplicating."),
        false);
    return FReply::Handled();
  }

  UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();
  const FWidgetPreviewObject *Source = Settings->FindWidgetPreviewObject(SourceId);
  if (!Source)
  {
    return FReply::Handled();
  }

  // The copy is a real asset: duplicate the blueprint into the generated
  // mount first, so a failure leaves no half-made entry behind.
  UWidgetBlueprint *SourceBlueprint =
      UIWTGenerated::FindWidgetBlueprint(Source->WidgetClass);
  if (!SourceBlueprint)
  {
    UIWTNotify::Show(
        LOCTEXT("DuplicateNoBlueprint",
                "Choose a widget blueprint for this entry before duplicating "
                "it."),
        false);
    return FReply::Handled();
  }

  FText Error;
  UWidgetBlueprint *Copy =
      UIWTGenerated::DuplicateWidgetBlueprint(SourceBlueprint, Error);
  if (!Copy || !Copy->GeneratedClass)
  {
    UIWTNotify::Show(
        FText::Format(LOCTEXT("DuplicateFailed", "Could not duplicate: {0}"),
                      Error),
        false);
    return FReply::Handled();
  }

  // SourceEntryId is always the root entry.
  const FGuid RootId =
      Source->SourceEntryId.IsValid() ? Source->SourceEntryId : Source->Id;

  const FGuid NewId = Settings->DuplicateWidgetPreviewObject(SourceId);
  FWidgetPreviewObject *PreviewObject = Settings->FindWidgetPreviewObject(NewId);
  if (!PreviewObject)
  {
    return FReply::Handled();
  }
  PreviewObject->WidgetClass = TSoftClassPtr<UUserWidget>(Copy->GeneratedClass);
  PreviewObject->WidgetName = Copy->GetName();
  PreviewObject->SourceEntryId = RootId;
  Settings->SaveWidgetPreviewObjects();

  // The copy scans as its root, so the source's scan is its scan.
  if (const FUIWTLevelScanResult *SourceScan = ScanResults.Find(SourceId))
  {
    FUIWTLevelScanResult CopyScan = *SourceScan;
    CopyScan.EntryId = NewId;
    ScanResults.Add(NewId, MoveTemp(CopyScan));
  }

  // Land on the copy's entry.
  RefreshList();
  RestoreSelection(NewId, true);
  UIWTNotify::Show(
      FText::Format(LOCTEXT("DuplicateDone", "Created {0}."),
                    FText::FromString(Copy->GetName())),
      true);
  return FReply::Handled();
}

FReply SUIWidgetManager::OnDeleteClicked(FGuid Id)
{
  UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();

  // Remove the entry before deleting the asset, or the deletion's reference
  // check finds the soft class path. A blueprint in the generated folder (a
  // Duplicate's copy, or the empty one the chat made) goes with the entry
  // unless another entry picked it too; Content/ blueprints are left alone.
  TSoftClassPtr<UUserWidget> GeneratedClass;
  if (const FWidgetPreviewObject *PreviewObject = Settings->FindWidgetPreviewObject(Id))
  {
    const bool bShared = Settings->WidgetPreviewObjects.ContainsByPredicate(
        [PreviewObject](const FWidgetPreviewObject &Other)
        {
          return Other.Id != PreviewObject->Id &&
                 Other.WidgetClass == PreviewObject->WidgetClass;
        });
    if (!bShared &&
        UIWTGenerated::IsGeneratedPath(
            PreviewObject->WidgetClass.ToSoftObjectPath().GetLongPackageName()))
    {
      GeneratedClass = PreviewObject->WidgetClass;
    }
  }
  // The run may be editing the blueprint about to be deleted.
  if (!GeneratedClass.IsNull() && IsRunInFlight.Get(false))
  {
    UIWTNotify::Show(
        LOCTEXT("DeleteDuringRun",
                "A run is editing a blueprint. Wait for it to finish, or "
                "cancel it, before deleting this entry."),
        false);
    return FReply::Handled();
  }
  Settings->RemoveWidgetPreviewObject(Id);
  EditSession.End(Id);
  NewEntryIds.Remove(Id);
  NewWidgetNames.Remove(Id);
  EndFieldPick(Id);
  ScanResults.Remove(Id);
  if (SelectedEntryId == Id)
  {
    SelectedEntryId = FGuid();
  }
  OnEntryDeleted.ExecuteIfBound(Id);

  if (!GeneratedClass.IsNull())
  {
    FText Error;
    if (!UIWTGenerated::DeleteWidgetBlueprint(GeneratedClass, Error))
    {
      UIWTNotify::Show(
          FText::Format(LOCTEXT("DeleteCopyFailed",
                                "The entry was removed but its blueprint "
                                "was not deleted: {0}"),
                        Error),
          false);
    }
  }

  RefreshList();
  return FReply::Handled();
}

FReply SUIWidgetManager::OnPlayClicked(TSharedPtr<FUIWTManagerEntry> Entry)
{
  const FWidgetPreviewObject *PreviewObject =
      Entry.IsValid() ? UUIWidgetPreviewObjectManagerSettings::Get()
                            ->FindWidgetPreviewObject(Entry->Id)
                      : nullptr;
  if (!PreviewObject)
  {
    return FReply::Handled();
  }

  FUIWTPlayRequest Request;
  Request.MapPackagePath = Entry->LevelPackagePath.ToString();
  Request.WidgetClass = PreviewObject->WidgetClass;

  if (Entry->CheckpointId.IsValid())
  {
    if (const FUIWTCheckpointIndexEntry *Checkpoint =
            CheckpointIndex.Find(Entry->CheckpointId))
    {
      Request.CheckpointId = Checkpoint->Header.CheckpointId;
      Request.CheckpointSidecarPath = Checkpoint->SidecarPath;
      Request.CheckpointPayloadPath = Checkpoint->PayloadPath;
    }
  }

  FText Error;
  if (!UIWTPlayLauncher::RequestPlay(Request, Error))
  {
    UE_LOG(LogUIWidgetToolPlugin, Warning, TEXT("Play refused: %s"),
           *Error.ToString());
  }
  return FReply::Handled();
}

FName SUIWidgetManager::GetCheckpointPickLevel(FGuid Id) const
{
  if (EditSession.IsEditing(Id))
  {
    const TSharedPtr<FUIWTLevelOption> Level = EditSession.GetPendingLevel(Id);
    return UIWTManagerOptions::IsNoneLevelOption(Level) ? NAME_None
                                                        : Level->PackagePath;
  }
  const TSharedPtr<FUIWTManagerEntry> Entry = FindEntry(Id);
  return Entry.IsValid() ? Entry->LevelPackagePath : NAME_None;
}

TArray<TSharedPtr<FUIWTCheckpointOption>>
SUIWidgetManager::BuildEntryCheckpointOptions(FGuid Id) const
{
  const FName LevelPath = GetCheckpointPickLevel(Id);
  const FString MapPackagePath =
      LevelPath.IsNone() ? FString() : LevelPath.ToString();

  const TArray<const FUIWTCheckpointIndexEntry *> Compatible =
      CheckpointIndex.GetValid(MapPackagePath);

  TArray<TSharedPtr<FUIWTCheckpointOption>> Options;
  Options.Reserve(Compatible.Num());
  for (const FUIWTCheckpointIndexEntry *Checkpoint : Compatible)
  {
    TSharedRef<FUIWTCheckpointOption> Option =
        MakeShared<FUIWTCheckpointOption>();
    Option->Id = Checkpoint->Header.CheckpointId;
    Option->Display = Checkpoint->GetDisplayString();
    Option->PayloadPath = Checkpoint->PayloadPath;
    Options.Add(Option);
  }
  return Options;
}

void SUIWidgetManager::AssignCheckpoint(FGuid EntryId, FGuid CheckpointId)
{
  UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();
  if (FWidgetPreviewObject *PreviewObject = Settings->FindWidgetPreviewObject(EntryId))
  {
    PreviewObject->CheckpointId = CheckpointId;
    Settings->SaveWidgetPreviewObjects();
  }
  EndFieldPick(EntryId);
  RefreshList();
}

void SUIWidgetManager::RenameCheckpoint(FGuid CheckpointId,
                                        const FText &NewName)
{
  const FUIWTCheckpointIndexEntry *Checkpoint = CheckpointIndex.FindValid(CheckpointId);
  if (!Checkpoint)
  {
    UIWTNotify::Show(
        LOCTEXT("RenameGone",
                "That checkpoint is no longer in the index. Refresh and try "
                "again."),
        false);
    return;
  }

  const FString NewLabel = NewName.ToString().TrimStartAndEnd();
  const FString SidecarPath = Checkpoint->SidecarPath;

  FUIWTCheckpointResult Result;
  if (!UIWTCheckpointCodec::SetCheckpointDisplayName(SidecarPath, NewLabel,
                                                     Result))
  {
    UIWTNotify::Show(
        FText::Format(LOCTEXT("RenameFailed", "Could not rename: {0}"),
                      FText::FromString(Result.Message)),
        false);
    return;
  }

  CheckpointIndex.Rebuild();
  RefreshList();

  UIWTNotify::Show(
      NewLabel.IsEmpty()
          ? LOCTEXT("RenameCleared",
                    "Checkpoint name cleared - it shows <map>_<captured at> "
                    "again.")
          : FText::Format(LOCTEXT("RenameDone", "Checkpoint renamed to \"{0}\"."),
                          FText::FromString(NewLabel)),
      true);
}

void SUIWidgetManager::BeginCheckpointRenameIn(
    const TSharedPtr<SUIWTNameEditCell> &Cell,
    const TSharedPtr<FUIWTManagerEntry> &Entry) const
{
  const FUIWTCheckpointIndexEntry *Checkpoint =
      Entry.IsValid() ? CheckpointIndex.FindValid(Entry->CheckpointId) : nullptr;
  if (Checkpoint && Cell.IsValid())
  {
    Cell->BeginEdit(Entry->CheckpointId, Checkpoint->GetEffectiveDisplayName());
  }
}

void SUIWidgetManager::BeginCheckpointNameEdit()
{
  // Reached by double-click as well as the context menu, so apply the same
  // gate the menu entry greys out on.
  if (CanRenameSelectedCheckpoint())
  {
    BeginCheckpointRenameIn(CheckpointNameCell, FindSelectedEntry());
  }
}

bool SUIWidgetManager::IsSelectedEntryEditing() const
{
  return SelectedEntryId.IsValid() && EditSession.IsEditing(SelectedEntryId);
}

const FSlateBrush *SUIWidgetManager::GetUpdateButtonIcon() const
{
  return IsSelectedEntryEditing()
             ? FAppStyle::GetBrush("Icons.Check")
             : FUIWidgetToolPluginStyle::Get().GetBrush(
                   "UIWidgetTool.Icons.Update");
}

FText SUIWidgetManager::GetUpdateButtonToolTip() const
{
  if (IsSelectedEntryEditing())
  {
    if (IsNamingNewWidget())
    {
      return LOCTEXT("ConfirmNewTip",
                     "Keep the level and checkpoint chosen in this entry's "
                     "pickers. No widget is chosen, so a new empty widget "
                     "blueprint is created for the entry, named as typed in "
                     "the details panel's Widget field.");
    }
    return LOCTEXT("ConfirmTip",
                   "Keep the widget, level and checkpoint chosen in this "
                   "entry's pickers.");
  }
  return LOCTEXT("UpdateTip",
                 "Choose this entry's widget, which level it plays on, and "
                 "which checkpoint captured on that level it restores.");
}

FSlateColor SUIWidgetManager::GetUpdateButtonColor() const
{
  return IsSelectedEntryEditing() ? FSlateColor(FLinearColor::Green)
                                  : FSlateColor(FLinearColor::White);
}

FReply SUIWidgetManager::OnUpdateOrConfirmClicked()
{
  if (!SelectedEntryId.IsValid())
  {
    return FReply::Handled();
  }
  return EditSession.IsEditing(SelectedEntryId) ? OnConfirmClicked(SelectedEntryId)
                                                : OnUpdateClicked(SelectedEntryId);
}

EVisibility SUIWidgetManager::GetEditOnlyVisibility() const
{
  return IsSelectedEntryEditing() ? EVisibility::Visible : EVisibility::Collapsed;
}

FReply SUIWidgetManager::OnCancelEditClicked()
{
  if (!SelectedEntryId.IsValid() || !EditSession.IsEditing(SelectedEntryId))
  {
    return FReply::Handled();
  }

  EditSession.End(SelectedEntryId);
  NewEntryIds.Remove(SelectedEntryId);
  NewWidgetNames.Remove(SelectedEntryId);

  RunLevelScan();

  RefreshList();
  return FReply::Handled();
}

FReply SUIWidgetManager::OnDuplicateSelectedClicked()
{
  return OnDuplicateClicked(SelectedEntryId);
}

FReply SUIWidgetManager::OnDeleteSelectedClicked()
{
  if (!HasSelection())
  {
    return FReply::Handled();
  }
  return OnDeleteClicked(SelectedEntryId);
}

bool SUIWidgetManager::SelectedEntryHasWidget() const
{
  const FWidgetPreviewObject *PreviewObject =
      FindSelectedEntry().IsValid() ? FindSelectedPreviewObject() : nullptr;
  return PreviewObject && !PreviewObject->WidgetClass.IsNull();
}

bool SUIWidgetManager::CanPlaySelected() const
{
  const TSharedPtr<FUIWTManagerEntry> Entry = FindSelectedEntry();
  return Entry.IsValid() && !Entry->LevelPackagePath.IsNone() && SelectedEntryHasWidget();
}

bool SUIWidgetManager::CanSnapshotSelected() const
{
  return FindEntrySnapshot(FindSelectedEntry()) != nullptr;
}

FText SUIWidgetManager::GetPlayToolTip() const
{
  const TSharedPtr<FUIWTManagerEntry> Entry = FindSelectedEntry();
  if (!Entry.IsValid())
  {
    return LOCTEXT("PlayNoSelectionTip", "Select an entry to play.");
  }
  if (!SelectedEntryHasWidget())
  {
    return LOCTEXT("PlayNoWidgetTip", "Choose a widget class first.");
  }
  if (Entry->LevelPackagePath.IsNone())
  {
    return LOCTEXT("PlayNoTargetTip",
                   "No level or checkpoint chosen.");
  }
  return LOCTEXT("PlayTip",
                 "Start a PIE session.");
}

FText SUIWidgetManager::GetSnapshotToolTip() const
{
  const TSharedPtr<FUIWTManagerEntry> Entry = FindSelectedEntry();
  if (!Entry.IsValid())
  {
    return LOCTEXT("LoadSnapshotNoSelectionTip",
                   "Select a row to load its snapshot.");
  }
  if (FindEntrySnapshot(Entry))
  {
    return LOCTEXT("LoadSnapshotTip",
                   "Load this checkpoint's widget snapshot into the snapshot viewer.");
  }
  if (Entry->CheckpointId.IsValid())
  {
    return LOCTEXT("LoadSnapshotNoFileTip",
                   "No checkpoint file found.");
  }
  return LOCTEXT("LoadSnapshotNoCheckpointTip",
                 "No checkpoint assigned.");
}

FText SUIWidgetManager::GetDuplicateToolTip() const
{
  if (!HasSelection())
  {
    return LOCTEXT("DuplicateNoSelectionTip",
                   "Select an entry to duplicate it.");
  }
  if (IsRunInFlight.Get(false))
  {
    return LOCTEXT("DuplicateDuringRunTip",
                   "A run is editing a blueprint. Wait for it to finish, "
                   "or cancel it, first.");
  }
  if (IsSelectedEntryEditing())
  {
    return LOCTEXT("DuplicateWhileEditingTip",
                   "Confirm or cancel this entry's pickers first.");
  }
  if (!SelectedEntryHasWidget())
  {
    return LOCTEXT("DuplicateNoWidgetTip",
                   "Choose a widget blueprint for this entry first.");
  }
  return LOCTEXT("DuplicateTip",
                 "Copy this entry and its Widget Blueprint into the generated "
                 "folder.");
}

FReply SUIWidgetManager::OnPlaySelectedClicked()
{
  return OnPlayClicked(FindSelectedEntry());
}

FReply SUIWidgetManager::OnSnapshotSelectedClicked()
{
  return OnLoadSnapshotClicked(FindSelectedEntry());
}

FText SUIWidgetManager::GetSelectedCellText(FName Column) const
{
  const TSharedPtr<FUIWTManagerEntry> Entry = FindSelectedEntry();
  return Entry.IsValid() ? UIWTManagerColumns::CellText(*Entry, Column)
                         : LOCTEXT("NoSelectionValue", "-");
}

FText SUIWidgetManager::GetSelectedCheckpointText() const
{
  const TSharedPtr<FUIWTManagerEntry> Entry = FindSelectedEntry();
  const FUIWTCheckpointIndexEntry *Checkpoint =
      Entry.IsValid() ? CheckpointIndex.FindValid(Entry->CheckpointId) : nullptr;
  // The name alone; the capture time and actor count stay in the picker.
  return Checkpoint ? FText::FromString(Checkpoint->GetEffectiveDisplayName())
                    : GetSelectedCellText(UIWTManagerColumns::LevelCheckpoint);
}

const FUIWTCheckpointIndexEntry *
SUIWidgetManager::FindSelectedCheckpointFile() const
{
  const TSharedPtr<FUIWTManagerEntry> Entry = FindSelectedEntry();
  if (!Entry.IsValid())
  {
    return nullptr;
  }
  const FUIWTCheckpointIndexEntry *Checkpoint =
      CheckpointIndex.Find(Entry->CheckpointId);
  return (Checkpoint && !Checkpoint->SidecarPath.IsEmpty()) ? Checkpoint : nullptr;
}

bool SUIWidgetManager::CanRevealCheckpointFile() const
{
  return FindSelectedCheckpointFile() != nullptr;
}

FText SUIWidgetManager::GetRevealCheckpointFileToolTip() const
{
  if (!HasSelection())
  {
    return LOCTEXT("RevealCheckpointNoSelectionTip",
                   "Select an entry to locate its checkpoint file.");
  }
  if (!CanRevealCheckpointFile())
  {
    return LOCTEXT("RevealCheckpointNoneTip",
                   "This entry has no checkpoint file on disk.");
  }
  return LOCTEXT("RevealCheckpointTip",
                 "Show this checkpoint's file in the file browser.");
}

FReply SUIWidgetManager::OnRevealCheckpointFileClicked()
{
  const FUIWTCheckpointIndexEntry *Checkpoint = FindSelectedCheckpointFile();
  if (!Checkpoint)
  {
    return FReply::Handled();
  }

  const FString FullPath =
      FPaths::ConvertRelativePathToFull(Checkpoint->SidecarPath);
  if (!IFileManager::Get().FileExists(*FullPath))
  {
    UIWTNotify::Show(
        FText::Format(LOCTEXT("RevealCheckpointGone",
                              "Checkpoint file {0} is no longer on disk."),
                      FText::FromString(FullPath)),
        false);
    return FReply::Handled();
  }

  FPlatformProcess::ExploreFolder(*FullPath);
  return FReply::Handled();
}

bool SUIWidgetManager::CanPanelPick() const
{
  return HasSelection() && !IsSelectedEntryEditing();
}

void SUIWidgetManager::RebuildPanelPickers()
{
  if (!PanelLevelPickerBox.IsValid() || !PanelCheckpointPickerBox.IsValid())
  {
    return;
  }

  const TSharedPtr<FUIWTManagerEntry> Entry = FindSelectedEntry();
  const FGuid Id = Entry.IsValid() ? Entry->Id : FGuid();

  // The buttons show what the entry has saved, as the list's cells do; a
  // pick is written at once.
  TSharedRef<FUIWTLevelOption> CurrentLevel = MakeShared<FUIWTLevelOption>();
  TArray<TSharedPtr<FUIWTLevelOption>> LevelOptions;
  if (Entry.IsValid())
  {
    CurrentLevel->PackagePath = Entry->LevelPackagePath;
    CurrentLevel->Display = Entry->LevelDisplay;
    TSharedPtr<FUIWTLevelOption> Unused;
    LevelOptions = FUIWTEntryEditSession::MakeLevelOptions(
        Id, ScanResults, CheckpointIndex, nullptr, Unused);
  }

  FUIWTPickerArgs<FUIWTLevelOption> LevelArgs;
  LevelArgs.Options = &LevelOptions;
  const FText PickLevelTip = LOCTEXT(
      "PickLevelTip",
      "Choose which level this widget is tested on. Levels that reference "
      "the widget are listed when the scan found any; otherwise every level "
      "in the project is.");
  // Leads with the full package path the button shortens.
  LevelArgs.ToolTip =
      CurrentLevel->PackagePath.IsNone()
          ? PickLevelTip
          : FText::Format(LOCTEXT("PanelLevelPickerTip", "{0}\n\n{1}"),
                          FText::FromName(CurrentLevel->PackagePath),
                          PickLevelTip);
  LevelArgs.EmptyText =
      LOCTEXT("NoLevelsToPick", "No levels found in the project.");
  LevelArgs.GetCurrent = [CurrentLevel]() -> TSharedPtr<FUIWTLevelOption>
  { return CurrentLevel; };
  LevelArgs.OnPicked = [this, Id](TSharedPtr<FUIWTLevelOption> Picked)
  { PickLevel(Id, Picked); };
  LevelArgs.bFillWidth = true;
  PanelLevelPickerBox->SetContent(
      UIWTSearchablePicker::MakeSearchablePicker(MoveTemp(LevelArgs)));

  TSharedRef<FUIWTCheckpointOption> CurrentCheckpoint =
      MakeShared<FUIWTCheckpointOption>();
  TArray<TSharedPtr<FUIWTCheckpointOption>> CheckpointOptions;
  if (Entry.IsValid())
  {
    CurrentCheckpoint->Id = Entry->CheckpointId;
    CurrentCheckpoint->Display = GetSelectedCheckpointText().ToString();
    CheckpointOptions = BuildEntryCheckpointOptions(Id);
  }
  const bool bHasLevel = Entry.IsValid() && !GetCheckpointPickLevel(Id).IsNone();

  FUIWTPickerArgs<FUIWTCheckpointOption> CheckpointArgs;
  CheckpointArgs.Options = &CheckpointOptions;
  CheckpointArgs.ToolTip =
      bHasLevel ? LOCTEXT("PickCheckpointTip",
                          "Choose a checkpoint captured on this level.")
                : LOCTEXT("PickCheckpointNoLevelTip",
                          "Choose any checkpoint. Once a level is chosen, "
                          "only checkpoints captured on it are offered.");
  CheckpointArgs.EmptyText =
      bHasLevel ? LOCTEXT("NoCompatible",
                          "No checkpoints captured on this level yet.")
                : LOCTEXT("NoCheckpointsAtAll", "No checkpoints captured yet.");
  CheckpointArgs.GetCurrent =
      [CurrentCheckpoint]() -> TSharedPtr<FUIWTCheckpointOption>
  { return CurrentCheckpoint; };
  CheckpointArgs.OnPicked = [this, Id](TSharedPtr<FUIWTCheckpointOption> Picked)
  { AssignCheckpoint(Id, Picked.IsValid() ? Picked->Id : FGuid()); };
  CheckpointArgs.ButtonColor =
      Entry.IsValid() ? UIWTManagerColumns::CheckpointCellColor(*Entry)
                      : FSlateColor::UseSubduedForeground();
  CheckpointArgs.bFillWidth = true;
  CheckpointArgs.OnDoubleClicked =
      FSimpleDelegate::CreateSP(this, &SUIWidgetManager::BeginCheckpointNameEdit);
  PanelCheckpointPickerBox->SetContent(
      UIWTSearchablePicker::MakeSearchablePicker(MoveTemp(CheckpointArgs)));
}

UWidgetBlueprint *SUIWidgetManager::FindSelectedWidgetBlueprint() const
{
  const FWidgetPreviewObject *PreviewObject = FindSelectedPreviewObject();
  return PreviewObject ? UIWTGenerated::FindWidgetBlueprint(PreviewObject->WidgetClass)
                       : nullptr;
}

FName SUIWidgetManager::GetSelectedLevelPath() const
{
  const TSharedPtr<FUIWTManagerEntry> Entry = FindSelectedEntry();
  return Entry.IsValid() ? Entry->LevelPackagePath : NAME_None;
}

// The level the editor has open, as a package path.
static FName GetEditorLevelPath()
{
  const UWorld *World =
      GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
  return World ? World->GetPackage()->GetFName() : NAME_None;
}

bool SUIWidgetManager::CanOpenSelectedLevel() const
{
  const FName LevelPath = GetSelectedLevelPath();
  return !LevelPath.IsNone() && GEditor && !GEditor->PlayWorld &&
         LevelPath != GetEditorLevelPath();
}

FText SUIWidgetManager::GetOpenLevelToolTip() const
{
  const FName LevelPath = GetSelectedLevelPath();
  if (LevelPath.IsNone())
  {
    return LOCTEXT("OpenLevelNoneTip", "Choose a level for this entry first.");
  }
  if (GEditor && GEditor->PlayWorld)
  {
    return LOCTEXT("OpenLevelDuringPIETip",
                   "Stop the Play In Editor session to open this level.");
  }
  if (LevelPath == GetEditorLevelPath())
  {
    return LOCTEXT("OpenLevelAlreadyTip", "This level is already open.");
  }
  return FText::Format(
      LOCTEXT("OpenLevelTip", "Open {0} in the level editor."),
      FText::FromName(LevelPath));
}

FReply SUIWidgetManager::OnOpenLevelClicked()
{
  if (!CanOpenSelectedLevel())
  {
    return FReply::Handled();
  }

  // Loading replaces the open level; let the user keep its unsaved changes
  // or stay on it.
  if (!FEditorFileUtils::SaveDirtyPackages(/*bPromptUserToSave*/ true,
                                           /*bSaveMapPackages*/ true,
                                           /*bSaveContentPackages*/ false))
  {
    return FReply::Handled();
  }

  const FString LevelPath = GetSelectedLevelPath().ToString();
  if (!FEditorFileUtils::LoadMap(LevelPath))
  {
    UIWTNotify::Show(
        FText::Format(LOCTEXT("OpenLevelFailed", "Could not open {0}."),
                      FText::FromString(LevelPath)),
        false);
  }
  return FReply::Handled();
}

FReply SUIWidgetManager::OnOpenBlueprintClicked()
{
  UWidgetBlueprint *Blueprint = FindSelectedWidgetBlueprint();
  if (!Blueprint)
  {
    UIWTNotify::Show(
        LOCTEXT("BlueprintNotFound",
                "The Widget Blueprint for this entry could not be loaded. It "
                "may have been deleted or renamed - use Update to point the "
                "entry at it again."),
        false);
    return FReply::Handled();
  }

  UAssetEditorSubsystem *AssetEditorSubsystem =
      GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
  if (!AssetEditorSubsystem)
  {
    UIWTNotify::Show(
        LOCTEXT("BlueprintNoAssetEditor",
                "No asset editor is available to open the Blueprint in."),
        false);
    return FReply::Handled();
  }

  AssetEditorSubsystem->OpenEditorForAsset(Blueprint);
  return FReply::Handled();
}

FReply SUIWidgetManager::OnRevealWidgetFileClicked()
{
  const FWidgetPreviewObject *PreviewObject = FindSelectedPreviewObject();
  if (!PreviewObject || PreviewObject->WidgetClass.IsNull())
  {
    return FReply::Handled();
  }

  const FString PackageName =
      PreviewObject->WidgetClass.ToSoftObjectPath().GetLongPackageName();
  FString FullPath;
  if (FPackageName::TryConvertLongPackageNameToFilename(
          PackageName, FullPath, FPackageName::GetAssetPackageExtension()))
  {
    FullPath = FPaths::ConvertRelativePathToFull(FullPath);
  }
  if (FullPath.IsEmpty() || !IFileManager::Get().FileExists(*FullPath))
  {
    UIWTNotify::Show(
        FText::Format(LOCTEXT("RevealWidgetFileGone",
                              "The Widget Blueprint file for {0} is not on "
                              "disk. It may have been deleted or renamed."),
                      FText::FromString(PackageName)),
        false);
    return FReply::Handled();
  }

  FPlatformProcess::ExploreFolder(*FullPath);
  return FReply::Handled();
}

FText SUIWidgetManager::GetSelectedNoteEditText() const
{
  const FWidgetPreviewObject *PreviewObject = FindSelectedPreviewObject();
  return PreviewObject ? FText::FromString(PreviewObject->Note)
                       : FText::GetEmpty();
}

FText SUIWidgetManager::GetNoteHintText() const
{
  return FindSelectedPreviewObject()
             ? LOCTEXT("NoteEmptyHint", "Add a note...")
             : LOCTEXT("NoteNoSelectionHint", "Select an entry to see its note.");
}

bool SUIWidgetManager::IsNoteReadOnly() const
{
  return FindSelectedPreviewObject() == nullptr;
}

void SUIWidgetManager::OnNoteTextChanged(const FText &)
{
  // The box also reports the text it takes from a new selection while
  // unfocused; only typing starts a draft.
  if (NoteDraftEntryId.IsValid() || !NoteBox.IsValid() || IsNoteReadOnly() ||
      !(NoteBox->HasKeyboardFocus() || NoteBox->HasFocusedDescendants()))
  {
    return;
  }
  NoteDraftEntryId = SelectedEntryId;
}

void SUIWidgetManager::OnNoteTextCommitted(const FText &NewText,
                                           ETextCommit::Type CommitType)
{
  const FGuid EntryId = NoteDraftEntryId;
  NoteDraftEntryId.Invalidate();
  // Esc reverts the box and commits as cleared.
  if (!EntryId.IsValid() || CommitType == ETextCommit::OnCleared)
  {
    return;
  }
  SetNote(EntryId, NewText);
}

void SUIWidgetManager::SetNote(FGuid EntryId, const FText &NewNote)
{
  UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();
  FWidgetPreviewObject *PreviewObject = Settings->FindWidgetPreviewObject(EntryId);
  if (!PreviewObject)
  {
    return;
  }

  const FString Note = NewNote.ToString().TrimStartAndEnd();
  if (PreviewObject->Note == Note)
  {
    return;
  }
  PreviewObject->Note = Note;
  Settings->SaveWidgetPreviewObjects();

  // Rows carry the note in their Widget tooltip and the search filter
  // matches on it.
  RefreshList();
}

bool SUIWidgetManager::CanDuplicateSelected() const
{
  const FWidgetPreviewObject *PreviewObject = FindSelectedPreviewObject();
  return PreviewObject && !PreviewObject->WidgetClass.IsNull() &&
         !EditSession.IsEditing(PreviewObject->Id) && !IsRunInFlight.Get(false);
}

// The viewer's Blueprint tab follows the manager selection: the entry's tree,
// with picks accepted from snapshots of it or, for a duplicate, of the entry
// it was duplicated from.
void SUIWidgetManager::PushSelectionToViewer()
{
  if (!OnDesignBlueprintChanged.IsBound())
  {
    return;
  }
  const FWidgetPreviewObject *PreviewObject = FindSelectedPreviewObject();
  UWidgetBlueprint *Blueprint =
      PreviewObject ? UIWTGenerated::FindWidgetBlueprint(PreviewObject->WidgetClass) : nullptr;
  TArray<FString> Accepted;
  if (Blueprint)
  {
    Accepted.Add(Blueprint->GetPathName());
    if (const FWidgetPreviewObject *Root =
            UUIWidgetPreviewObjectManagerSettings::Get()
                ->FindWidgetPreviewObject(PreviewObject->SourceEntryId))
    {
      if (UWidgetBlueprint *RootBlueprint =
              UIWTGenerated::FindWidgetBlueprint(Root->WidgetClass))
      {
        Accepted.Add(RootBlueprint->GetPathName());
      }
    }
  }
  OnDesignBlueprintChanged.Execute(Blueprint, Accepted);
}

void SUIWidgetManager::DescribeEntryForRun(const FGuid &InEntryId,
                                           FString &OutLevel,
                                           FString &OutCheckpoint) const
{
  OutLevel.Reset();
  OutCheckpoint.Reset();
  if (const TSharedPtr<FUIWTManagerEntry> Entry = FindEntry(InEntryId))
  {
    OutLevel = Entry->LevelPackagePath.IsNone()
                   ? FString()
                   : Entry->LevelPackagePath.ToString();
    OutCheckpoint = Entry->bCheckpointInvalid ? FString() : Entry->CheckpointDisplay;
  }
}

#undef LOCTEXT_NAMESPACE
