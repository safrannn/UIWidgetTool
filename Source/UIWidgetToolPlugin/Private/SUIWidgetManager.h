#pragma once

#include "AssetRegistry/AssetData.h"
#include "CoreMinimal.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SEntryRow;
class SBox;
class SHorizontalBox;

enum class EUIWidgetSortField : uint8 { WidgetName };

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

  FString SearchText;

  // Row above the search bar, split into per-column boxes matching the
  // widths of the Widget / Actions columns below.
  TSharedPtr<SHorizontalBox> WidgetSearchBox;
  TSharedPtr<SHorizontalBox> ActionsBox;

  EUIWidgetSortField SortField = EUIWidgetSortField::WidgetName;
  bool bSortAscending = true;

  // Ids of rows currently showing the widget-picker in place of their name,
  // and the state backing that picker.
  TSet<FGuid> EditingRows;
  TMap<FGuid, TArray<TSharedPtr<FAssetData>>> EditOptions;
  TMap<FGuid, TSharedPtr<FAssetData>> PendingSelection;

  TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FGuid> Item,
                                      const TSharedRef<STableViewBase> &Owner);

  FReply OnUpdateClicked(FGuid Id);
  FReply OnConfirmClicked(FGuid Id);
  FReply OnPlayClicked(FGuid Id);
  FReply OnDeleteClicked(FGuid Id);
  void OnSearchTextChanged(const FText &NewText);

  bool IsRowEditing(FGuid Id) const { return EditingRows.Contains(Id); }
  const TArray<TSharedPtr<FAssetData>> *GetEditOptions(FGuid Id) const {
    return EditOptions.Find(Id);
  }
  TSharedPtr<FAssetData> GetPendingSelection(FGuid Id) const {
    return PendingSelection.FindRef(Id);
  }
  void SetPendingSelection(FGuid Id, TSharedPtr<FAssetData> Selection) {
    PendingSelection.Add(Id, Selection);
  }

  TSharedRef<SWidget> BuildSortMenu();
  void SetSortField(EUIWidgetSortField NewField);
  bool IsSortField(EUIWidgetSortField Field) const;
  void SetSortAscending(bool bAscending);
  bool IsSortAscending(bool bAscending) const;
  const FSlateBrush *GetSortIcon() const;
};
