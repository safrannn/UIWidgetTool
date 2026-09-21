#include "SUIWidgetManager.h"

#include "Blueprint/UserWidget.h"
#include "Editor.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
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
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
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

  constexpr float WidgetColumnFillWidth = 0.27f;
  constexpr float LevelColumnFillWidth = 0.24f;
  constexpr float LevelCheckpointColumnFillWidth = 0.29f;
  constexpr float BlueprintColumnFillWidth = 0.20f;

  constexpr float ListPanelSizeValue = 0.5f;
  constexpr float UtilityPanelSizeValue = 0.5f;
  constexpr float ListPanelMinHeight = 72.f;
  constexpr float UtilityPanelMinHeight = 164.f;

  constexpr float HeaderRowHeight = 24.f;
  constexpr float DesiredVisibleRows = 10.f;

  constexpr float DetailLabelWidth = 104.f;

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
    if (ColumnId == UIWTManagerColumns::Blueprint)
    {
      return EUIWidgetSortField::Blueprint;
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
         LOCTEXT("FilterCheckpoint", "Level Checkpoint")},
        {EUIWTSearchField::Blueprint, LOCTEXT("FilterBlueprint", "Blueprint")}};
    return Options;
  }

  // The package the level scan looks for referencers of. A generated copy is
  // referenced by nothing, so it scans as its root original; SourceEntryId
  // is always the root, so this is one lookup.
  FName GetWidgetPackageName(const FWidgetPreviewObject &InPreviewObject)
  {
    const FWidgetPreviewObject *Root = &InPreviewObject;
    if (!InPreviewObject.IsOriginal())
    {
      if (const FWidgetPreviewObject *Found =
              UUIWidgetPreviewObjectManagerSettings::Get()
                  ->FindWidgetPreviewObject(InPreviewObject.SourceEntryId))
      {
        Root = Found;
      }
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

  TSharedRef<SWidget> MakeButtonIcon(const FSlateBrush *Brush)
  {
    return SNew(SImage)
        .Image(Brush)
        .ColorAndOpacity(FSlateColor::UseForeground());
  }

  // Icon followed by a label, for buttons that keep a short caption.
  TSharedRef<SWidget> MakeIconLabel(const FSlateBrush *Brush, const FText &Label)
  {
    return SNew(SHorizontalBox) +
           SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(
               FMargin(0.f, 0.f, 4.f, 0.f))[MakeButtonIcon(Brush)] +
           SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
               [SNew(STextBlock).Text(Label)];
  }

  TSharedRef<SWidget> MakeDetailRow(TSharedRef<SWidget> Label,
                                    TAttribute<FText> CopyText,
                                    FSimpleDelegate OnDoubleClicked,
                                    TSharedRef<SWidget> Value,
                                    FSimpleDelegate OnRename = FSimpleDelegate(),
                                    TAttribute<bool> CanRename = true)
  {
    return SNew(SHorizontalBox) +
           SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(FMargin(0.f, 0.f, 6.f, 0.f))[SNew(SBox).WidthOverride(DetailLabelWidth)[Label]] +
           SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Top)
               [SNew(SUIWTCopyableCell)
                    .CopyText(CopyText)
                    .OnRename(OnRename)
                    .CanRename(CanRename)
                    .OnDoubleClicked(OnDoubleClicked)[Value]];
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

  TSharedRef<SWidget> MakeDetailLabel(const FText &Text)
  {
    return SNew(STextBlock)
        .Text(Text)
        .AutoWrapText(true)
        .ColorAndOpacity(FSlateColor::UseSubduedForeground());
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
       SVerticalBox::Slot().FillHeight(1.f)
           [SNew(SSplitter).Orientation(Orient_Vertical) +
            SSplitter::Slot()
                .Value(ListPanelSizeValue)
                .MinSize(ListPanelMinHeight)[BuildListPanel()] +
            SSplitter::Slot()
                .Value(UtilityPanelSizeValue)
                .MinSize(UtilityPanelMinHeight)[BuildUtilityPanel()]] +
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
               ButtonSlot(
                   SNew(SButton)
                       .ToolTipText(LOCTEXT("NewEntryTip", "Add a new entry."))
                       .OnClicked(this, &SUIWidgetManager::OnAddWidgetClicked)
                           [MakeButtonIcon(
                               FAppStyle::GetBrush("Icons.AddCircle"))]) +
               ButtonSlot(
                   SNew(SButton)
                       .ToolTipText(this,
                                    &SUIWidgetManager::GetUpdateButtonToolTip)
                       .ButtonColorAndOpacity(
                           this, &SUIWidgetManager::GetUpdateButtonColor)
                       .IsEnabled(this, &SUIWidgetManager::HasSelection)
                       .OnClicked(
                           this, &SUIWidgetManager::OnUpdateOrConfirmClicked)
                           [SNew(SImage)
                                .Image(this,
                                       &SUIWidgetManager::GetUpdateButtonIcon)
                                .ColorAndOpacity(
                                    FSlateColor::UseForeground())]) +
               ButtonSlot(
                   SNew(SButton)
                       .Text(LOCTEXT("CancelBtn", "Cancel"))
                       .ToolTipText(LOCTEXT("CancelTip", "Undo the changes."))
                       .Visibility(this,
                                   &SUIWidgetManager::GetEditOnlyVisibility)
                       .OnClicked(this,
                                  &SUIWidgetManager::OnCancelEditClicked)) +
               ButtonSlot(
                   SNew(SButton)
                       .ToolTipText(this,
                                    &SUIWidgetManager::GetDuplicateToolTip)
                       .IsEnabled(this,
                                  &SUIWidgetManager::CanDuplicateSelected)
                       .OnClicked(
                           this, &SUIWidgetManager::OnDuplicateSelectedClicked)
                           [MakeButtonIcon(
                               FAppStyle::GetBrush("Icons.Duplicate"))]) +
               ButtonSlot(
                   SNew(SButton)
                       .ToolTipText(
                           LOCTEXT("DeleteTip", "Remove the selected entry."))
                       .IsEnabled(this, &SUIWidgetManager::HasSelection)
                       .OnClicked(this,
                                  &SUIWidgetManager::OnDeleteSelectedClicked)
                           [MakeButtonIcon(FUIWidgetToolPluginStyle::Get().GetBrush(
                               "UIWidgetTool.Icons.Delete"))]) +
               SHorizontalBox::Slot().FillWidth(1.f) +
               SHorizontalBox::Slot().AutoWidth()
                   [SNew(SButton)
                        .ToolTipText(LOCTEXT(
                            "RefreshTip",
                            "Re-read the checkpoint directory and rescan which "
                            "levels reference each widget."))
                        .OnClicked(this, &SUIWidgetManager::OnRefreshClicked)
                            [MakeButtonIcon(
                                FAppStyle::GetBrush("Icons.Refresh"))]]];

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
                                           LevelCheckpointColumnFillWidth) +
                                MakeColumn(UIWTManagerColumns::Blueprint,
                                           LOCTEXT("HBlueprint", "Blueprint"),
                                           BlueprintColumnFillWidth))] +
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

  TSharedRef<SWidget> CheckpointLabel =
      SNew(SHorizontalBox) +
      SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
          [MakeDetailLabel(LOCTEXT("DetailCheckpoint", "Level Checkpoint"))] +
      SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(4.f, 0.f, 0.f, 0.f))[SNew(SButton).ButtonStyle(FAppStyle::Get(), "SimpleButton").ContentPadding(FMargin(2.f)).IsEnabled(this, &SUIWidgetManager::CanRevealCheckpointFile).ToolTipText(this, &SUIWidgetManager::GetRevealCheckpointFileToolTip).OnClicked(this, &SUIWidgetManager::OnRevealCheckpointFileClicked)[SNew(SImage).Image(FAppStyle::Get().GetBrush("Icons.FolderOpen")).ColorAndOpacity(FSlateColor::UseForeground())]];

  return SNew(SVerticalBox) +
         SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 2.f))
             [MakeDetailRow(
                 MakeDetailLabel(LOCTEXT("DetailWidget", "Widget")), WidgetText,
                 FSimpleDelegate::CreateSP(
                     this, &SUIWidgetManager::BeginWidgetNameEdit),
                 SAssignNew(WidgetNameCell, SUIWTNameEditCell)
                     .OnCommitted(this, &SUIWidgetManager::RenameWidget)
                     .HintText(LOCTEXT("WidgetRenameHint",
                                       "New blueprint name"))
                         [SNew(STextBlock)
                              .Text(WidgetText)
                              .AutoWrapText(true)
                              .ColorAndOpacity(FSlateColor::UseForeground())])] +
         SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 2.f))
             [MakeDetailRow(
                 MakeDetailLabel(LOCTEXT("DetailLevel", "Level")), LevelText,
                 FSimpleDelegate(),
                 SNew(STextBlock)
                     .Text(LevelText)
                     .ToolTipText(this,
                                  &SUIWidgetManager::GetSelectedLevelToolTip)
                     .AutoWrapText(true)
                     .ColorAndOpacity(FSlateColor::UseForeground()))] +
         SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 2.f))
             [MakeDetailRow(
                 CheckpointLabel, CheckpointText, FSimpleDelegate(),
                 SAssignNew(CheckpointNameCell, SUIWTNameEditCell)
                     .OnCommitted(this, &SUIWidgetManager::RenameCheckpoint)
                     .HintText(LOCTEXT("RenameHint",
                                       "Leave empty for the map name"))
                         [SNew(STextBlock)
                              .Text(CheckpointText)
                              .AutoWrapText(true)
                              .ColorAndOpacity(
                                  this, &SUIWidgetManager::
                                            GetSelectedCheckpointColor)],
                 FSimpleDelegate::CreateSP(
                     this, &SUIWidgetManager::BeginCheckpointNameEdit),
                 TAttribute<bool>(
                     this, &SUIWidgetManager::CanRenameSelectedCheckpoint))] +
         SVerticalBox::Slot().AutoHeight()
             [MakeDetailRow(
                 MakeDetailLabel(LOCTEXT("DetailNote", "Note")),
                 TAttribute<FText>(this, &SUIWidgetManager::GetSelectedNoteText),
                 FSimpleDelegate::CreateSP(this, &SUIWidgetManager::BeginNoteEdit),
                 SAssignNew(NoteCell, SUIWTNameEditCell)
                     .OnCommitted(this, &SUIWidgetManager::SetNote)
                     .HintText(LOCTEXT("NoteEditHint",
                                       "Leave empty to clear the note"))
                         [SNew(STextBlock)
                              .Text(this, &SUIWidgetManager::GetSelectedNoteText)
                              .AutoWrapText(true)
                              .ColorAndOpacity(
                                  FSlateColor::UseSubduedForeground())])];
}

TSharedRef<SWidget> SUIWidgetManager::BuildUtilityPanel()
{
  TSharedRef<SWidget> ButtonRow =
      SNew(SHorizontalBox) +
      ButtonSlot(
          SNew(SButton)
              .IsEnabled(this, &SUIWidgetManager::CanPlaySelected)
              .ToolTipText(this, &SUIWidgetManager::GetPlayToolTip)
              .OnClicked(this, &SUIWidgetManager::OnPlaySelectedClicked)
                  [MakeButtonIcon(FAppStyle::GetBrush("Icons.Play"))]) +
      ButtonSlot(
          SNew(SButton)
              .IsEnabled(this, &SUIWidgetManager::CanSnapshotSelected)
              .ToolTipText(this, &SUIWidgetManager::GetSnapshotToolTip)
              .OnClicked(this, &SUIWidgetManager::OnSnapshotSelectedClicked)
                  [MakeIconLabel(FAppStyle::GetBrush("Icons.FolderOpen"),
                                 LOCTEXT("LoadSnapshotBtn", "Snapshot"))]) +
      SHorizontalBox::Slot().AutoWidth()
          [SNew(SButton)
               .IsEnabled(this, &SUIWidgetManager::SelectedEntryHasWidget)
               .ToolTipText(LOCTEXT(
                   "BlueprintTip",
                   "Open this entry's Widget Blueprint in the Blueprint "
                   "editor."))
               .OnClicked(this, &SUIWidgetManager::OnOpenBlueprintClicked)
                   [MakeIconLabel(FAppStyle::GetBrush("Icons.FolderOpen"),
                                  LOCTEXT("BlueprintBtn", "Blueprint"))]];

  ButtonRow->SlatePrepass();
  const float LeftColumnWidth = ButtonRow->GetDesiredSize().X;

  TSharedRef<SWidget> LeftChild =
      SNew(SVerticalBox) +
      SVerticalBox::Slot().AutoHeight()[ButtonRow] +
      SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 6.f))
          [SNew(SSeparator).Orientation(Orient_Horizontal)] +
      SVerticalBox::Slot().FillHeight(1.f)[BuildSelectedEntryDetails()];

  return SNew(SHorizontalBox) +
         SHorizontalBox::Slot().AutoWidth().Padding(
             FMargin(2.f, 2.f, 4.f, 2.f))
             [SNew(SBorder)
                  .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
                  .Padding(FMargin(6.f))
                      [SNew(SBox)
                           .WidthOverride(LeftColumnWidth)
                               [LeftChild]]] +
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
  UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();
  if (FWidgetPreviewObject *PreviewObject = Settings->FindWidgetPreviewObject(Id))
  {
    const TSharedPtr<FAssetData> Selection = EditSession.GetPendingWidget(Id);
    bClassChanged = ApplyWidgetSelection(*PreviewObject, Selection);
    if (!bClassChanged && !UIWTManagerOptions::IsNoneWidgetOption(Selection))
    {
      NotifyWidgetPickFailed(Selection);
    }

    const bool bLevelChanged =
        ApplyLevelSelection(*PreviewObject, EditSession.GetPendingLevel(Id));
    if (bClassChanged || bLevelChanged)
    {
      Settings->SaveWidgetPreviewObjects();
    }
  }
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
      Args.GetCurrent = [this, Id] { return EditSession.GetPendingWidget(Id); };
      Args.OnPicked = [this, Id](TSharedPtr<FAssetData> Picked)
      { SetPendingWidget(Id, Picked); };
    }
    else
    {
      Args.Options = &FieldPickWidgetOptions;
      Args.GetCurrent = [this] { return FieldPickWidget; };
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
      Args.GetCurrent = [this, Id] { return EditSession.GetPendingLevel(Id); };
      Args.OnPicked = [this, Id](TSharedPtr<FUIWTLevelOption> Picked)
      { SetPendingLevel(Id, Picked); };
    }
    else
    {
      Args.Options = &FieldPickLevelOptions;
      Args.GetCurrent = [this] { return FieldPickLevel; };
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

  RefreshList();
  return OnUpdateClicked(NewId);
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
                "A run is editing a blueprint copy. Wait for it to finish, or "
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

  // SourceEntryId is always the root original.
  const FGuid RootId =
      Source->IsOriginal() ? Source->Id : Source->SourceEntryId;

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
  // check finds the soft class path.
  TSoftClassPtr<UUserWidget> GeneratedClass;
  if (const FWidgetPreviewObject *PreviewObject = Settings->FindWidgetPreviewObject(Id))
  {
    if (!PreviewObject->IsOriginal())
    {
      GeneratedClass = PreviewObject->WidgetClass;
    }
  }
  Settings->RemoveWidgetPreviewObject(Id);
  EditSession.End(Id);
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
                                "The entry was removed but its blueprint copy "
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
                    "Checkpoint name cleared - it shows its map name again.")
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
    Cell->BeginEdit(Entry->CheckpointId, Checkpoint->Header.DisplayName);
  }
}

void SUIWidgetManager::BeginCheckpointNameEdit()
{
  BeginCheckpointRenameIn(CheckpointNameCell, FindSelectedEntry());
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
    return LOCTEXT("PlayNoSelectionTip", "Select an entry to play it.");
  }
  if (!SelectedEntryHasWidget())
  {
    return LOCTEXT("PlayNoWidgetTip", "Choose a widget class first.");
  }
  if (Entry->LevelPackagePath.IsNone())
  {
    return LOCTEXT("PlayNoTargetTip",
                   "This entry has no level and no checkpoint to play on. "
                   "Assign a checkpoint, or use the widget in a level.");
  }
  return LOCTEXT("PlayTip",
                 "Start a fresh Play In Editor session on this level, restore "
                 "the checkpoint if one is assigned, then show the widget.");
}

FText SUIWidgetManager::GetSnapshotToolTip() const
{
  const TSharedPtr<FUIWTManagerEntry> Entry = FindSelectedEntry();
  if (!Entry.IsValid())
  {
    return LOCTEXT("LoadSnapshotNoSelectionTip",
                   "Select an entry to load its snapshot.");
  }
  if (FindEntrySnapshot(Entry))
  {
    return LOCTEXT("LoadSnapshotTip",
                   "Load this checkpoint's widget snapshot into the Widget "
                   "Reflector's snapshot viewer.");
  }
  if (Entry->CheckpointId.IsValid())
  {
    return LOCTEXT("LoadSnapshotNoFileTip",
                   "This checkpoint has no widget snapshot on disk. It was "
                   "captured before snapshots, or the file was deleted.");
  }
  return LOCTEXT("LoadSnapshotNoCheckpointTip",
                 "Assign a checkpoint to this entry first: the snapshot is "
                 "captured with the checkpoint.");
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
                   "A run is editing a blueprint copy. Wait for it to finish, "
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

FText SUIWidgetManager::GetSelectedLevelToolTip() const
{
  const TSharedPtr<FUIWTManagerEntry> Entry = FindSelectedEntry();
  if (!Entry.IsValid() || Entry->LevelPackagePath.IsNone())
  {
    return FText::GetEmpty();
  }
  return FText::FromName(Entry->LevelPackagePath);
}

FText SUIWidgetManager::GetSelectedCheckpointText() const
{
  const TSharedPtr<FUIWTManagerEntry> Entry = FindSelectedEntry();
  const FUIWTCheckpointIndexEntry *Checkpoint =
      Entry.IsValid() ? CheckpointIndex.FindValid(Entry->CheckpointId) : nullptr;
  return Checkpoint ? FText::FromString(Checkpoint->GetDisplayString(true))
               : GetSelectedCellText(UIWTManagerColumns::LevelCheckpoint);
}

FSlateColor SUIWidgetManager::GetSelectedCheckpointColor() const
{
  const TSharedPtr<FUIWTManagerEntry> Entry = FindSelectedEntry();
  return Entry.IsValid() ? UIWTManagerColumns::CheckpointCellColor(*Entry)
                         : FSlateColor::UseSubduedForeground();
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

UWidgetBlueprint *SUIWidgetManager::FindSelectedWidgetBlueprint() const
{
  const FWidgetPreviewObject *PreviewObject = FindSelectedPreviewObject();
  return PreviewObject ? UIWTGenerated::FindWidgetBlueprint(PreviewObject->WidgetClass)
               : nullptr;
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

FText SUIWidgetManager::GetSelectedNoteText() const
{
  const FWidgetPreviewObject *PreviewObject = FindSelectedPreviewObject();
  if (!PreviewObject)
  {
    return LOCTEXT("NoSelectionValue", "-");
  }
  if (PreviewObject->IsOriginal())
  {
    return LOCTEXT("NoteOriginal", "(original)");
  }
  return PreviewObject->Note.IsEmpty() ? LOCTEXT("NoteEmpty", "(no note yet)")
                               : FText::FromString(PreviewObject->Note);
}

void SUIWidgetManager::BeginNoteEdit()
{
  const FWidgetPreviewObject *PreviewObject = FindSelectedPreviewObject();
  if (!PreviewObject || !NoteCell.IsValid() ||
      EditSession.IsEditing(PreviewObject->Id))
  {
    return;
  }
  if (PreviewObject->IsOriginal())
  {
    UIWTNotify::Show(LOCTEXT("NoteOriginalEdit",
                             "Originals carry no note - duplicate the entry "
                             "and note the copy."),
                     false);
    return;
  }
  NoteCell->BeginEdit(PreviewObject->Id, PreviewObject->Note);
}

void SUIWidgetManager::SetNote(FGuid EntryId, const FText &NewNote)
{
  UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();
  FWidgetPreviewObject *PreviewObject = Settings->FindWidgetPreviewObject(EntryId);
  if (!PreviewObject || PreviewObject->IsOriginal())
  {
    return;
  }

  PreviewObject->Note = NewNote.ToString().TrimStartAndEnd();
  Settings->SaveWidgetPreviewObjects();

  // Rows carry the note in their Blueprint tooltip and the search filter
  // matches on it.
  RefreshList();
}

bool SUIWidgetManager::CanDuplicateSelected() const
{
  const FWidgetPreviewObject *PreviewObject = FindSelectedPreviewObject();
  return PreviewObject && !PreviewObject->WidgetClass.IsNull() &&
         !EditSession.IsEditing(PreviewObject->Id) && !IsRunInFlight.Get(false);
}

// The viewer's Blueprint tab follows the manager selection: the copy's tree,
// with picks accepted from snapshots of the copy or of its root original.
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
    if (!PreviewObject->IsOriginal())
    {
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
