#pragma once

#include "CoreMinimal.h"
#include "UIWTManagerTypes.h"
#include "Widgets/Views/STableRow.h"

class SUIWTNameEditCell;
class SUIWidgetManager;

// One entry of the manager list. Reads the entry's edit state from and
// routes its picks and actions back to the owning manager.
class SUIWTEntryRow : public SMultiColumnTableRow<TSharedPtr<FUIWTManagerEntry>>
{
public:
  static constexpr float Height = 26.f;

  SLATE_BEGIN_ARGS(SUIWTEntryRow) {}
  SLATE_ARGUMENT(TSharedPtr<FUIWTManagerEntry>, Entry)
  SLATE_ARGUMENT(SUIWidgetManager *, Owner)
  SLATE_END_ARGS()

  void Construct(const FArguments &InArgs,
                 const TSharedRef<STableViewBase> &OwnerTable);

  virtual TSharedRef<SWidget>
  GenerateWidgetForColumn(const FName &Column) override;

private:
  TSharedRef<SWidget> GenerateCellContent(const FName &Column);

  FText GetCopyTextForColumn(FName Column) const;

  void BeginCheckpointRename();

  TSharedPtr<FUIWTManagerEntry> Entry;
  SUIWidgetManager *Owner = nullptr;
  TSharedPtr<SUIWTNameEditCell> CheckpointNameCell;
};
