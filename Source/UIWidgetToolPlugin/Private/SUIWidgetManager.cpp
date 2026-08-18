#include "SUIWidgetManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Styling/AppStyle.h"
#include "UIWidgetToolPlugin.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
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
const FName ColLevelBookmark("LevelBookmark");
const FName ColActions("Actions");
constexpr float SearchBarPanelHeight = 28.f;
constexpr float WidgetColumnFillWidth = 0.35f;
constexpr float LevelBookmarkColumnFillWidth = 0.4f;
constexpr float ActionsColumnWidth = 210.f;
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
  FGuid WidgetPreviewObjectId;
  SUIWidgetManager *Owner = nullptr;
};

void SUIWidgetManager::Construct(const FArguments &InArgs) {
  // Box containing the search bar and sort button; sized in the layout
  // below to match the width of the Widget column.
  TSharedRef<SWidget> SearchAndSortBox =
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

  ChildSlot
      [SNew(SVerticalBox) +
       SVerticalBox::Slot().AutoHeight()
           [SAssignNew(SearchBarPanel, SBox)
                .HeightOverride(SearchBarPanelHeight)
                .Padding(FMargin(
                    2.f))[SNew(SHorizontalBox) +
                          SHorizontalBox::Slot().FillWidth(
                              WidgetColumnFillWidth)[SearchAndSortBox] +
                          SHorizontalBox::Slot().FillWidth(
                              LevelBookmarkColumnFillWidth)[SNew(SSpacer)] +
                          SHorizontalBox::Slot().AutoWidth()
                              [SNew(SBox).WidthOverride(ActionsColumnWidth)]]] +
       SVerticalBox::Slot().FillHeight(
           1.f)[SAssignNew(ListView, SListView<TSharedPtr<FGuid>>)
                    .ListItemsSource(&Rows)
                    .OnGenerateRow(this, &SUIWidgetManager::OnGenerateRow)
                    .SelectionMode(ESelectionMode::Single)
                    .HeaderRow(SNew(SHeaderRow) +
                               SHeaderRow::Column(ColWidget)
                                   .DefaultLabel(LOCTEXT("HWidget", "Widget"))
                                   .FillWidth(WidgetColumnFillWidth) +
                               SHeaderRow::Column(ColLevelBookmark)
                                   .DefaultLabel(LOCTEXT("HLevelBookmark",
                                                         "Level Bookmark"))
                                   .FillWidth(LevelBookmarkColumnFillWidth) +
                               SHeaderRow::Column(ColActions)
                                   .DefaultLabel(LOCTEXT("HActions", "Actions"))
                                   .FixedWidth(ActionsColumnWidth))]];

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
          !WidgetPreviewObject.WidgetName.Contains(SearchText) &&
          !WidgetPreviewObject.LevelBookmark.Contains(SearchText)) {
        continue;
      }
      Filtered.Add(&WidgetPreviewObject);
    }

    Filtered.Sort(
        [this](const FWidgetPreviewObject &A, const FWidgetPreviewObject &B) {
          const int32 Comparison =
              SortField == EUIWidgetSortField::WidgetName
                  ? A.WidgetName.Compare(B.WidgetName, ESearchCase::IgnoreCase)
                  : A.LevelBookmark.Compare(B.LevelBookmark,
                                            ESearchCase::IgnoreCase);
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
    MenuBuilder.AddMenuEntry(
        LOCTEXT("SortByLevelBookmark", "Level Bookmark"), FText(), FSlateIcon(),
        FUIAction(
            FExecuteAction::CreateSP(this, &SUIWidgetManager::SetSortField,
                                     EUIWidgetSortField::LevelBookmark),
            FCanExecuteAction(),
            FIsActionChecked::CreateSP(this, &SUIWidgetManager::IsSortField,
                                       EUIWidgetSortField::LevelBookmark)),
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
  FUIWidgetToolPluginModule::OpenUpdateTab();
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
    return SNew(STextBlock)
        .Text(FText::FromString(bValid ? WidgetPreviewObject->WidgetName
                                       : WidgetPreviewObject->WidgetName +
                                             TEXT(" (invalid)")))
        .ColorAndOpacity(bValid ? FSlateColor::UseForeground()
                                : FSlateColor(FLinearColor::Red));
  }
  if (Column == ColLevelBookmark) {
    return SNew(STextBlock)
        .Text(FText::FromString(WidgetPreviewObject->LevelBookmark));
  }
  if (Column == ColActions) {
    return SNew(SHorizontalBox) +
           SHorizontalBox::Slot().AutoWidth().Padding(FMargin(
               0.f, 0.f, 2.f,
               0.f))[SNew(SButton)
                         .Text(LOCTEXT("UpdateBtn", "Update"))
                         .OnClicked(Owner, &SUIWidgetManager::OnUpdateClicked,
                                    WidgetPreviewObjectId)] +
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
