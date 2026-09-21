#pragma once

#include "CoreMinimal.h"
#include "UIWTSnapshotReader.h"
#include "Widgets/Views/STableRow.h"

namespace UIWTSnapshotColumns
{

  extern const FName WidgetName;
  extern const FName Source;
  extern const FName Visibility;
  extern const FName Focusable;
  extern const FName Enabled;
  extern const FName Volatile;
  extern const FName HasActiveTimer;
  extern const FName Clipping;
  extern const FName LayerId;
  extern const FName ActualSize;
  extern const FName Address;

  struct FColumnInfo
  {
    FName Id;
    FText Label;
    bool bHiddenByDefault;

    // A positive FillWidth shares the remaining space; otherwise the column is
    // FixedWidth wide, widened if its label needs more.
    float FillWidth;
    float FixedWidth;
  };

  const TArray<FColumnInfo> &GetAll();
  const FColumnInfo *Find(const FName &InId);
  TArray<FName> GetDefaultHiddenColumns();

}

class SUIWTSnapshotTreeRow
    : public SMultiColumnTableRow<TSharedRef<FUIWTSnapshotNode>>
{
public:
  SLATE_BEGIN_ARGS(SUIWTSnapshotTreeRow) {}
  SLATE_ARGUMENT(TSharedPtr<FUIWTSnapshotNode>, Node)
  SLATE_END_ARGS()

  void Construct(const FArguments &InArgs,
                 const TSharedRef<STableViewBase> &InOwnerTable);

  virtual TSharedRef<SWidget>
  GenerateWidgetForColumn(const FName &InColumnName) override;

private:
  FSlateColor GetRowColor() const;

  TSharedPtr<FUIWTSnapshotNode> Node;
};
