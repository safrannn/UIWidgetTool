#pragma once

#include "CoreMinimal.h"
#include "UIWTLevelScan.h"
#include "UIWTManagerTypes.h"

class FUIWTCheckpointIndex;
class FUIWTEntryEditSession;
struct FWidgetPreviewObject;

// What the list shows of the settings: the search that filters it and the
// column it is sorted on.
struct FUIWTManagerListQuery
{
  // Trimmed; empty matches everything.
  FString SearchText;
  EUIWTSearchField SearchField = EUIWTSearchField::All;

  // None keeps the settings order.
  EUIWidgetSortField SortField = EUIWidgetSortField::None;
  bool bSortAscending = true;

  // Kept in the list even when the search would drop it, so the selection
  // does not vanish while typing.
  FGuid PinnedEntryId;
};

namespace UIWTManagerListBuilder
{
  // One list entry per preview object, minus those the search drops, sorted
  // by the query. Entries being edited are always kept and show their
  // pending level rather than the saved one.
  TArray<TSharedPtr<FUIWTManagerEntry>>
  Build(const TArray<FWidgetPreviewObject> &InPreviewObjects,
        const FUIWTCheckpointIndex &InCheckpoints,
        const TMap<FGuid, FUIWTLevelScanResult> &InScanResults,
        const FUIWTEntryEditSession &InEditSession,
        const FUIWTManagerListQuery &InQuery);
}
