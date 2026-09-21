#pragma once

#include "AssetRegistry/AssetData.h"
#include "CoreMinimal.h"
#include "UIWTLevelScan.h"
#include "UIWTManagerTypes.h"

class FUIWTCheckpointIndex;

// What one entry offers and has picked while its widget and level are being
// chosen. Nothing is written to the settings until the manager confirms.
struct FUIWTEntryEditState
{
  TArray<TSharedPtr<FAssetData>> WidgetOptions;
  TSharedPtr<FAssetData> PendingWidget;
  TArray<TSharedPtr<FUIWTLevelOption>> LevelOptions;
  TSharedPtr<FUIWTLevelOption> PendingLevel;
};

// The entries currently being edited. Reads the settings for an entry's
// current values; only the manager writes them back.
class FUIWTEntryEditSession
{
public:
  bool IsEditing(FGuid Id) const { return States.Contains(Id); }
  bool IsEmpty() const { return States.IsEmpty(); }
  TArray<FGuid> GetEditingIds() const;

  // Offers every widget blueprint in the project, with the entry's current
  // widget picked. Level options wait for BuildLevelOptions.
  void Begin(FGuid Id);
  void End(FGuid Id);

  const TArray<TSharedPtr<FAssetData>> *GetWidgetOptions(FGuid Id) const;
  TSharedPtr<FAssetData> GetPendingWidget(FGuid Id) const;
  void SetPendingWidget(FGuid Id, TSharedPtr<FAssetData> InSelection);

  const TArray<TSharedPtr<FUIWTLevelOption>> *GetLevelOptions(FGuid Id) const;
  TSharedPtr<FUIWTLevelOption> GetPendingLevel(FGuid Id) const;
  void SetPendingLevel(FGuid Id, TSharedPtr<FUIWTLevelOption> InSelection);

  // Offers the levels the scan found for the entry, or every level in the
  // project when it found none, plus the entry's own level. Keeps the
  // pending level when it is still offered; otherwise picks the entry's
  // level, then its checkpoint's map, then none.
  void BuildLevelOptions(FGuid Id,
                         const TMap<FGuid, FUIWTLevelScanResult> &InScanResults,
                         const FUIWTCheckpointIndex &InCheckpoints);

  // A double-click on a cell asks for its picker to open as soon as the row
  // is generated. The row takes the request once.
  void RequestAutoOpenPicker(FGuid Id, FName Column);
  bool TakeAutoOpenPicker(FGuid Id, FName Column);

  // The option lists Begin and BuildLevelOptions offer, for use outside a
  // session. OutCurrent is the option matching the entry's saved value (or
  // the fallback described on BuildLevelOptions), none when nothing matches.
  static TArray<TSharedPtr<FAssetData>>
  MakeWidgetOptions(FGuid Id, TSharedPtr<FAssetData> &OutCurrent);
  static TArray<TSharedPtr<FUIWTLevelOption>>
  MakeLevelOptions(FGuid Id,
                   const TMap<FGuid, FUIWTLevelScanResult> &InScanResults,
                   const FUIWTCheckpointIndex &InCheckpoints,
                   const TSharedPtr<FUIWTLevelOption> &InPreferred,
                   TSharedPtr<FUIWTLevelOption> &OutCurrent);

private:
  TMap<FGuid, FUIWTEntryEditState> States;

  FGuid AutoOpenPickerEntry;
  FName AutoOpenPickerColumn;
};
