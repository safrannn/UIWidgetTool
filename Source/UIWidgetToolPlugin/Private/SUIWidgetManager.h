#pragma once

#include "CoreMinimal.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SEntryRow;
class SBox;

enum class EUIWidgetSortField : uint8 { WidgetName, LevelBookmark };

class SUIWidgetManager : public SCompoundWidget {
  friend class SEntryRow;

public:
  SLATE_BEGIN_ARGS(SUIWidgetManager) {}
  SLATE_END_ARGS()

  void Construct(const FArguments &InArgs);

  void RefreshList();

private:
  TArray<TSharedPtr<FGuid>> Rows;
  TSharedPtr<SListView<TSharedPtr<FGuid>>> ListView;

  TSharedPtr<SBox> SearchBarPanel;
  FString SearchText;

  EUIWidgetSortField SortField = EUIWidgetSortField::WidgetName;
  bool bSortAscending = true;

  TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FGuid> Item,
                                      const TSharedRef<STableViewBase> &Owner);

  FReply OnPlayClicked(FGuid Id);
  FReply OnDeleteClicked(FGuid Id);
  void OnLevelBookmarkCommitted(const FText &NewText,
                                ETextCommit::Type CommitType, FGuid Id);
  void OnSearchTextChanged(const FText &NewText);

  TSharedRef<SWidget> BuildSortMenu();
  void SetSortField(EUIWidgetSortField NewField);
  bool IsSortField(EUIWidgetSortField Field) const;
  void SetSortAscending(bool bAscending);
  bool IsSortAscending(bool bAscending) const;
  const FSlateBrush *GetSortIcon() const;
};
