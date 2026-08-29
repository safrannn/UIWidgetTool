#include "SUIWidgetManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Styling/AppStyle.h"
#include "UIWidgetToolPlugin.h"
#include "WidgetBlueprint.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"

#define LOCTEXT_NAMESPACE "UIWidgetTool"

namespace {
const FName ColWidget("Widget");
const FName ColLevel("Level");
const FName ColLevelCheckpoint("LevelCheckpoint");
const FName ColActions("Actions");
constexpr float SearchBarPanelHeight = 28.f;

// The three fill columns share the space left over after the fixed-width
// Actions column. They are relative to each other; summing to 1 is a
// readability convention, not a requirement. Level Checkpoint gets the most
// because its display string is the longest
// ("Combat - 2026-08-28 14:02 - 217 actors").
constexpr float WidgetColumnFillWidth = 0.30f;
constexpr float LevelColumnFillWidth = 0.28f;
constexpr float LevelCheckpointColumnFillWidth = 0.42f;
constexpr float ActionsColumnWidth = 210.f;

// Row height is enforced on every generated cell (see
// SEntryRow::GenerateWidgetForColumn) and reused for the list's height cap, so
// the two cannot drift apart.
constexpr float RowHeight = 26.f;
constexpr int32 MaxVisibleRows = 10;
constexpr float HeaderRowHeight = 24.f;

// All Widget Blueprint assets in the project, sorted by name, for the
// row's in-place widget picker.
TArray<FAssetData> GetAvailableWidgetBlueprints() {
  TArray<FAssetData> Result;
  FAssetRegistryModule &AssetRegistryModule =
      FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
  AssetRegistryModule.Get().GetAssetsByClass(
      UWidgetBlueprint::StaticClass()->GetClassPathName(), Result,
      /*bSearchSubClasses=*/true);
  Result.Sort([](const FAssetData &A, const FAssetData &B) {
    return A.AssetName.LexicalLess(B.AssetName);
  });
  return Result;
}
} // namespace

class SEntryRow : public SMultiColumnTableRow<TSharedPtr<FGuid>> {
public:
  SLATE_BEGIN_ARGS(SEntryRow) {}
  SLATE_ARGUMENT(FGuid, EntryId)
  SLATE_ARGUMENT(SUIWidgetManager *, Owner)
  SLATE_END_ARGS()

  void Construct(const FArguments &InArgs,
                 const TSharedRef<STableViewBase> &OwnerTable) {
    WidgetPreviewObjectId = InArgs._EntryId;
    Owner = InArgs._Owner;
    SMultiColumnTableRow<TSharedPtr<FGuid>>::Construct(
        FSuperRowType::FArguments(), OwnerTable);
  }

  virtual TSharedRef<SWidget>
  GenerateWidgetForColumn(const FName &Column) override;

private:
  // The column's content, unpadded and unsized. GenerateWidgetForColumn wraps
  // this to apply the shared row height.
  TSharedRef<SWidget> GenerateCellContent(const FName &Column);

  FGuid WidgetPreviewObjectId;
  SUIWidgetManager *Owner = nullptr;
};

void SUIWidgetManager::Construct(const FArguments &InArgs) {
  // Box containing the search bar and sort button; lives inside
  // WidgetSearchBox below, which is sized to match the Widget column.
  TSharedRef<SWidget> WidgetSearchAndSortBox =
      SNew(SHorizontalBox) +
      SHorizontalBox::Slot().FillWidth(
          1.f)[SNew(SSearchBox)
                   .HintText(LOCTEXT("SearchHint", "Search widgets..."))
                   .OnTextChanged(this,
                                  &SUIWidgetManager::OnSearchTextChanged)] +
      SHorizontalBox::Slot().AutoWidth().Padding(FMargin(
          4.f, 0.f, 0.f,
          0.f))[SNew(SComboButton)
                    .ContentPadding(FMargin(4.f, 0.f))
                    .ButtonContent()[SNew(SImage).Image(
                        this, &SUIWidgetManager::GetSortIcon)]
                    .OnGetMenuContent(this, &SUIWidgetManager::BuildSortMenu)];

  // Row above the search bar, split into per-column boxes sized to match the
  // columns below. The Level and Level Checkpoint slots are empty spacers that
  // exist only to keep ActionsBox aligned under the Actions column.
  TSharedRef<SWidget> ColumnAlignedBoxesRow =
      SNew(SHorizontalBox) +
      SHorizontalBox::Slot().FillWidth(
          WidgetColumnFillWidth)[SAssignNew(WidgetSearchBox, SHorizontalBox) +
                                 SHorizontalBox::Slot().FillWidth(
                                     1.f)[WidgetSearchAndSortBox]] +
      SHorizontalBox::Slot().FillWidth(LevelColumnFillWidth)[SNew(SSpacer)] +
      SHorizontalBox::Slot().FillWidth(
          LevelCheckpointColumnFillWidth)[SNew(SSpacer)] +
      SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(
          ActionsColumnWidth)[SAssignNew(ActionsBox, SHorizontalBox)]];

  // Cap the list so a project with hundreds of entries doesn't produce an
  // unboundedly tall panel; beyond the cap SListView scrolls on its own.
  const float MaxListHeight = MaxVisibleRows * RowHeight + HeaderRowHeight;

  ChildSlot.Padding(FMargin(10.f))
      [SNew(SVerticalBox) +
       SVerticalBox::Slot()
           .AutoHeight()[SNew(SBox)
                             .HeightOverride(SearchBarPanelHeight)
                             .Padding(FMargin(2.f))[ColumnAlignedBoxesRow]] +
       SVerticalBox::Slot().AutoHeight()
           [SNew(SBox).MaxDesiredHeight(MaxListHeight)
                [SAssignNew(ListView, SListView<TSharedPtr<FGuid>>)
                     .ListItemsSource(&Rows)
                     .OnGenerateRow(this, &SUIWidgetManager::OnGenerateRow)
                     .SelectionMode(ESelectionMode::Single)
                     .HeaderRow(
                         SNew(SHeaderRow) +
                         SHeaderRow::Column(ColWidget)
                             .DefaultLabel(LOCTEXT("HWidget", "Widget"))
                             .FillWidth(WidgetColumnFillWidth) +
                         SHeaderRow::Column(ColLevel)
                             .DefaultLabel(LOCTEXT("HLevel", "Level"))
                             .FillWidth(LevelColumnFillWidth) +
                         SHeaderRow::Column(ColLevelCheckpoint)
                             .DefaultLabel(LOCTEXT("HLevelCheckpoint",
                                                   "Level Checkpoint"))
                             .FillWidth(LevelCheckpointColumnFillWidth) +
                         SHeaderRow::Column(ColActions)
                             .DefaultLabel(LOCTEXT("HActions", "Actions"))
                             .FixedWidth(ActionsColumnWidth))]]];

  RefreshList();
}

void SUIWidgetManager::RefreshList() {
  Rows.Reset();
  if (UUIWidgetPreviewObjectManagerSettings *Settings =
          UUIWidgetPreviewObjectManagerSettings::Get()) {
    TArray<const FWidgetPreviewObject *> Filtered;
    for (const FWidgetPreviewObject &WidgetPreviewObject :
         Settings->WidgetPreviewObjects) {
      if (!SearchText.IsEmpty() &&
          !WidgetPreviewObject.WidgetName.Contains(SearchText)) {
        continue;
      }
      Filtered.Add(&WidgetPreviewObject);
    }

    Filtered.Sort(
        [this](const FWidgetPreviewObject &A, const FWidgetPreviewObject &B) {
          const int32 Comparison =
              A.WidgetName.Compare(B.WidgetName, ESearchCase::IgnoreCase);
          return bSortAscending ? Comparison < 0 : Comparison > 0;
        });

    for (const FWidgetPreviewObject *WidgetPreviewObject : Filtered) {
      Rows.Add(MakeShared<FGuid>(WidgetPreviewObject->Id));
    }
  }
  if (ListView.IsValid()) {
    ListView->RequestListRefresh();
  }
}

void SUIWidgetManager::OnSearchTextChanged(const FText &NewText) {
  SearchText = NewText.ToString();
  RefreshList();
}

TSharedRef<SWidget> SUIWidgetManager::BuildSortMenu() {
  FMenuBuilder MenuBuilder(/*bInShouldCloseWindowAfterMenuSelection=*/true,
                           nullptr);

  MenuBuilder.BeginSection("SortBy", LOCTEXT("SortBySection", "Sort By"));
  {
    MenuBuilder.AddMenuEntry(
        LOCTEXT("SortByWidgetName", "Widget Name"), FText(), FSlateIcon(),
        FUIAction(
            FExecuteAction::CreateSP(this, &SUIWidgetManager::SetSortField,
                                     EUIWidgetSortField::WidgetName),
            FCanExecuteAction(),
            FIsActionChecked::CreateSP(this, &SUIWidgetManager::IsSortField,
                                       EUIWidgetSortField::WidgetName)),
        NAME_None, EUserInterfaceActionType::RadioButton);
  }
  MenuBuilder.EndSection();

  MenuBuilder.BeginSection("SortType", LOCTEXT("SortTypeSection", "Sort Type"));
  {
    MenuBuilder.AddMenuEntry(
        LOCTEXT("SortAscending", "Ascending"), FText(), FSlateIcon(),
        FUIAction(FExecuteAction::CreateSP(
                      this, &SUIWidgetManager::SetSortAscending, true),
                  FCanExecuteAction(),
                  FIsActionChecked::CreateSP(
                      this, &SUIWidgetManager::IsSortAscending, true)),
        NAME_None, EUserInterfaceActionType::RadioButton);
    MenuBuilder.AddMenuEntry(
        LOCTEXT("SortDescending", "Descending"), FText(), FSlateIcon(),
        FUIAction(FExecuteAction::CreateSP(
                      this, &SUIWidgetManager::SetSortAscending, false),
                  FCanExecuteAction(),
                  FIsActionChecked::CreateSP(
                      this, &SUIWidgetManager::IsSortAscending, false)),
        NAME_None, EUserInterfaceActionType::RadioButton);
  }
  MenuBuilder.EndSection();

  return MenuBuilder.MakeWidget();
}

void SUIWidgetManager::SetSortField(EUIWidgetSortField NewField) {
  SortField = NewField;
  RefreshList();
}

bool SUIWidgetManager::IsSortField(EUIWidgetSortField Field) const {
  return SortField == Field;
}

void SUIWidgetManager::SetSortAscending(bool bAscending) {
  bSortAscending = bAscending;
  RefreshList();
}

bool SUIWidgetManager::IsSortAscending(bool bAscending) const {
  return bSortAscending == bAscending;
}

const FSlateBrush *SUIWidgetManager::GetSortIcon() const {
  return FAppStyle::Get().GetBrush(bSortAscending ? "Icons.SortUp"
                                                  : "Icons.SortDown");
}

TSharedRef<ITableRow>
SUIWidgetManager::OnGenerateRow(TSharedPtr<FGuid> Item,
                                const TSharedRef<STableViewBase> &Owner) {
  return SNew(SEntryRow, Owner).EntryId(*Item).Owner(this);
}

FReply SUIWidgetManager::OnUpdateClicked(FGuid Id) {
  TArray<TSharedPtr<FAssetData>> Options;
  for (const FAssetData &Asset : GetAvailableWidgetBlueprints()) {
    Options.Add(MakeShared<FAssetData>(Asset));
  }

  // Default the picker to whichever entry currently matches this row, so
  // opening the picker doesn't itself change anything until confirmed.
  TSharedPtr<FAssetData> InitialSelection;
  if (UUIWidgetPreviewObjectManagerSettings *Settings =
          UUIWidgetPreviewObjectManagerSettings::Get()) {
    if (FWidgetPreviewObject *Entry = Settings->FindWidgetPreviewObject(Id)) {
      for (const TSharedPtr<FAssetData> &Option : Options) {
        if (Option->AssetName == FName(*Entry->WidgetName)) {
          InitialSelection = Option;
          break;
        }
      }
    }
  }
  if (!InitialSelection.IsValid() && Options.Num() > 0) {
    InitialSelection = Options[0];
  }

  EditOptions.Add(Id, MoveTemp(Options));
  PendingSelection.Add(Id, InitialSelection);
  EditingRows.Add(Id);
  RefreshList();
  return FReply::Handled();
}

FReply SUIWidgetManager::OnConfirmClicked(FGuid Id) {
  TSharedPtr<FAssetData> Selection = GetPendingSelection(Id);
  if (Selection.IsValid()) {
    if (UUIWidgetPreviewObjectManagerSettings *Settings =
            UUIWidgetPreviewObjectManagerSettings::Get()) {
      if (FWidgetPreviewObject *Entry = Settings->FindWidgetPreviewObject(Id)) {
        Entry->WidgetName = Selection->AssetName.ToString();
        if (UWidgetBlueprint *WBP =
                Cast<UWidgetBlueprint>(Selection->GetAsset())) {
          if (WBP->GeneratedClass) {
            Entry->WidgetClass =
                TSoftClassPtr<UUserWidget>(WBP->GeneratedClass);
          }
        }
        Settings->SaveWidgetPreviewObjects();
      }
    }
  }
  EditingRows.Remove(Id);
  EditOptions.Remove(Id);
  PendingSelection.Remove(Id);
  RefreshList();
  return FReply::Handled();
}

FReply SUIWidgetManager::OnPlayClicked(FGuid Id) {
  FUIWidgetToolPluginModule::OpenPlayTab();
  return FReply::Handled();
}

FReply SUIWidgetManager::OnDeleteClicked(FGuid Id) {
  if (UUIWidgetPreviewObjectManagerSettings *Settings =
          UUIWidgetPreviewObjectManagerSettings::Get()) {
    Settings->RemoveWidgetPreviewObject(Id);
  }
  RefreshList();
  return FReply::Handled();
}

TSharedRef<SWidget> SEntryRow::GenerateWidgetForColumn(const FName &Column) {
  // Every cell is sized to RowHeight so the list's height cap (MaxVisibleRows *
  // RowHeight) describes the real layout rather than an assumed one.
  return SNew(SBox)
      .HeightOverride(RowHeight)
      .VAlign(VAlign_Center)
      .Padding(FMargin(2.f, 0.f))[GenerateCellContent(Column)];
}

TSharedRef<SWidget> SEntryRow::GenerateCellContent(const FName &Column) {
  UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();
  FWidgetPreviewObject *WidgetPreviewObject =
      Settings ? Settings->FindWidgetPreviewObject(WidgetPreviewObjectId)
               : nullptr;
  if (!WidgetPreviewObject) {
    return SNew(STextBlock).Text(LOCTEXT("Missing", "<missing>"));
  }

  // Broken/invalid state when the asset can't be resolved.
  const bool bValid = !WidgetPreviewObject->WidgetClass.IsNull();

  if (Column == ColWidget) {
    if (Owner->IsRowEditing(WidgetPreviewObjectId)) {
      const TArray<TSharedPtr<FAssetData>> *Options =
          Owner->GetEditOptions(WidgetPreviewObjectId);
      static const TArray<TSharedPtr<FAssetData>> EmptyOptions;
      const TArray<TSharedPtr<FAssetData>> &OptionsRef =
          Options ? *Options : EmptyOptions;
      const TSharedPtr<FAssetData> InitialSelection =
          Owner->GetPendingSelection(WidgetPreviewObjectId);

      SUIWidgetManager *OwnerPtr = Owner;
      FGuid EntryId = WidgetPreviewObjectId;
      return SNew(SComboBox<TSharedPtr<FAssetData>>)
          .OptionsSource(&OptionsRef)
          .InitiallySelectedItem(InitialSelection)
          .OnGenerateWidget_Lambda([](TSharedPtr<FAssetData> Item) {
            return SNew(STextBlock).Text(FText::FromName(Item->AssetName));
          })
          .OnSelectionChanged_Lambda(
              [OwnerPtr, EntryId](TSharedPtr<FAssetData> NewSelection,
                                  ESelectInfo::Type) {
                OwnerPtr->SetPendingSelection(EntryId, NewSelection);
              })[SNew(STextBlock).Text_Lambda([OwnerPtr, EntryId]() {
            TSharedPtr<FAssetData> Selection =
                OwnerPtr->GetPendingSelection(EntryId);
            return Selection.IsValid() ? FText::FromName(Selection->AssetName)
                                       : FText::GetEmpty();
          })];
    }
    return SNew(STextBlock)
        .Text(FText::FromString(bValid ? WidgetPreviewObject->WidgetName
                                       : WidgetPreviewObject->WidgetName +
                                             TEXT(" (invalid)")))
        .ColorAndOpacity(bValid ? FSlateColor::UseForeground()
                                : FSlateColor(FLinearColor::Red));
  }
  // Level and Level Checkpoint are structurally in place but have no data
  // source yet: Level needs the transitive reference scan (Plan.md A.3) and
  // Level Checkpoint needs checkpoint files to exist at all (Plan.md B.5).
  // Until then every row is the "no levels, no checkpoints" case from A.2,
  // which renders as an empty cell. A subdued placeholder is used rather than a
  // genuinely blank cell so the columns read as "nothing here yet" instead of
  // "broken", and rather than "scanning..." so it doesn't claim work is
  // happening that isn't.
  if (Column == ColLevel || Column == ColLevelCheckpoint) {
    return SNew(STextBlock)
        .Text(LOCTEXT("EmptyCell", "—"))
        .ColorAndOpacity(FSlateColor::UseSubduedForeground());
  }

  if (Column == ColActions) {
    const bool bEditing = Owner->IsRowEditing(WidgetPreviewObjectId);
    TSharedRef<SWidget> UpdateOrConfirmButton =
        bEditing
            ? StaticCastSharedRef<SWidget>(
                  SNew(SButton)
                      .ButtonColorAndOpacity(FLinearColor::Green)
                      .Text(LOCTEXT("ConfirmBtn", "Confirm"))
                      .OnClicked(Owner, &SUIWidgetManager::OnConfirmClicked,
                                 WidgetPreviewObjectId))
            : StaticCastSharedRef<SWidget>(
                  SNew(SButton)
                      .Text(LOCTEXT("UpdateBtn", "Update"))
                      .OnClicked(Owner, &SUIWidgetManager::OnUpdateClicked,
                                 WidgetPreviewObjectId));
    return SNew(SHorizontalBox) +
           SHorizontalBox::Slot().AutoWidth().Padding(
               FMargin(0.f, 0.f, 2.f, 0.f))[UpdateOrConfirmButton] +
           SHorizontalBox::Slot().AutoWidth().Padding(FMargin(
               0.f, 0.f, 2.f,
               0.f))[SNew(SButton)
                         .Text(LOCTEXT("PlayBtn", "Play"))
                         .OnClicked(Owner, &SUIWidgetManager::OnPlayClicked,
                                    WidgetPreviewObjectId)] +
           SHorizontalBox::Slot()
               .AutoWidth()[SNew(SButton)
                                .Text(LOCTEXT("DeleteBtn", "Delete"))
                                .OnClicked(Owner,
                                           &SUIWidgetManager::OnDeleteClicked,
                                           WidgetPreviewObjectId)];
  }
  return SNullWidget::NullWidget;
}

#undef LOCTEXT_NAMESPACE
