#pragma once

#include "AssetRegistry/AssetData.h"
#include "CoreMinimal.h"
#include "Styling/SlateColor.h"

enum class EUIWidgetSortField : uint8
{
  None,
  WidgetName,
  Level,
  Checkpoint,
  Blueprint
};

enum class EUIWTSearchField : uint8
{
  All,
  Widget,
  Level,
  Checkpoint,
  Blueprint
};

struct FUIWTLevelOption
{
  FName PackagePath;
  FString Display;
};

struct FUIWTCheckpointOption
{
  FGuid Id;
  FString Display;
  FString PayloadPath;
};

struct FUIWTManagerEntry
{
  FGuid Id;

  FName LevelPackagePath;
  FGuid CheckpointId;
  bool bCheckpointInvalid = false;

  FString WidgetDisplay;
  FString LevelDisplay;
  FString CheckpointDisplay;
  FString BlueprintDisplay;

  FString Note;

  bool bIsOriginal = true;
  int32 EntryOrder = 0;
};

// The list's column ids, shared by the header row, the entry rows and the
// selected-entry details.
namespace UIWTManagerColumns
{
  extern const FName Widget;
  extern const FName Level;
  extern const FName LevelCheckpoint;
  extern const FName Blueprint;

  // The entry's text for a column as the list shows it.
  FText CellText(const FUIWTManagerEntry &Entry, FName Column);
  FSlateColor CheckpointCellColor(const FUIWTManagerEntry &Entry);
}

// Every picker offers a "(none)" option that clears the field. These tell
// the sentinel apart from a real pick.
namespace UIWTManagerOptions
{
  FText GetNoneOptionText();

  bool IsNoneWidgetOption(const TSharedPtr<FAssetData> &In);
  bool IsNoneLevelOption(const TSharedPtr<FUIWTLevelOption> &In);
  bool IsNoneCheckpointOption(const TSharedPtr<FUIWTCheckpointOption> &In);

  TSharedRef<FAssetData> MakeNoneWidgetOption();
  TSharedRef<FUIWTLevelOption> MakeNoneLevelOption();
}
