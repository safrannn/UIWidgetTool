#include "UIWTManagerTypes.h"

#define LOCTEXT_NAMESPACE "UIWidgetTool"

namespace UIWTManagerColumns
{
  const FName Widget("Widget");
  const FName Level("Level");
  const FName LevelCheckpoint("LevelCheckpoint");

  FText CellText(const FUIWTManagerEntry &Entry, FName Column)
  {
    if (Column == Widget)
    {
      return FText::FromString(Entry.WidgetDisplay);
    }
    if (Column == Level)
    {
      return Entry.LevelDisplay.IsEmpty() ? LOCTEXT("NoLevel", "no level")
                                        : FText::FromString(Entry.LevelDisplay);
    }
    if (Column == LevelCheckpoint)
    {
      return Entry.CheckpointDisplay.IsEmpty()
                 ? LOCTEXT("NoCheckpoint", "(none)")
                 : FText::FromString(Entry.CheckpointDisplay);
    }
    return FText::GetEmpty();
  }

  FSlateColor CheckpointCellColor(const FUIWTManagerEntry &Entry)
  {
    if (Entry.bCheckpointInvalid)
    {
      return FSlateColor(FLinearColor::Red);
    }
    return Entry.CheckpointDisplay.IsEmpty() ? FSlateColor::UseSubduedForeground()
                                           : FSlateColor::UseForeground();
  }
}

namespace UIWTManagerOptions
{
  FText GetNoneOptionText() { return LOCTEXT("NoneOption", "(none)"); }

  bool IsNoneWidgetOption(const TSharedPtr<FAssetData> &In)
  {
    return !In.IsValid() || !In->IsValid();
  }

  bool IsNoneLevelOption(const TSharedPtr<FUIWTLevelOption> &In)
  {
    return !In.IsValid() || In->PackagePath.IsNone();
  }

  bool IsNoneCheckpointOption(const TSharedPtr<FUIWTCheckpointOption> &In)
  {
    return !In.IsValid() || !In->Id.IsValid();
  }

  TSharedRef<FAssetData> MakeNoneWidgetOption() { return MakeShared<FAssetData>(); }

  TSharedRef<FUIWTLevelOption> MakeNoneLevelOption()
  {
    TSharedRef<FUIWTLevelOption> Option = MakeShared<FUIWTLevelOption>();
    Option->PackagePath = NAME_None;
    Option->Display = GetNoneOptionText().ToString();
    return Option;
  }
}

#undef LOCTEXT_NAMESPACE
