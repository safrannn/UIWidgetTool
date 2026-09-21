#include "UIWTManagerListBuilder.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "UIWTCheckpointIndex.h"
#include "UIWTEntryEditSession.h"
#include "UIWidgetPreviewObjectManagerSettings.h"

namespace
{
  FString DescribeInvalidCheckpoint(const FUIWTCheckpointIndexEntry *InCheckpoint)
  {
    return InCheckpoint ? FString::Printf(TEXT("(unreadable: %s)"),
                                          *InCheckpoint->ErrorCode)
                        : FString(TEXT("(missing checkpoint file)"));
  }

  // Whether the entry's blueprint is still there, without loading it: the
  // list is rebuilt on every keystroke in the search box, and a class that
  // cannot be loaded would be retried, and logged, on every one of them.
  bool WidgetClassExists(const TSoftClassPtr<UUserWidget> &InClass)
  {
    if (InClass.IsValid())
    {
      return true;
    }
    const FSoftObjectPath ClassPath = InClass.ToSoftObjectPath();
    FString AssetName = ClassPath.GetAssetName();
    AssetName.RemoveFromEnd(TEXT("_C"));
    const FSoftObjectPath BlueprintPath(FString::Printf(
        TEXT("%s.%s"), *ClassPath.GetLongPackageName(), *AssetName));
    return IAssetRegistry::GetChecked()
        .GetAssetByObjectPath(BlueprintPath)
        .IsValid();
  }

  FString DisplayNameForEntry(const FWidgetPreviewObject &InPreviewObject)
  {
    if (InPreviewObject.WidgetClass.IsNull())
    {
      return TEXT("(choose a widget)");
    }
    if (!WidgetClassExists(InPreviewObject.WidgetClass))
    {
      return InPreviewObject.WidgetName.IsEmpty()
                 ? TEXT("(invalid)")
                 : InPreviewObject.WidgetName + TEXT(" (invalid)");
    }
    return InPreviewObject.WidgetName.IsEmpty() ? TEXT("(choose a widget)")
                                                : InPreviewObject.WidgetName;
  }

  TSharedRef<FUIWTManagerEntry>
  MakeEntry(const FWidgetPreviewObject &InPreviewObject, int32 InEntryOrder,
            const FUIWTCheckpointIndex &InCheckpoints,
            const TMap<FGuid, FUIWTLevelScanResult> &InScanResults,
            const FUIWTEntryEditSession &InEditSession)
  {
    TSharedRef<FUIWTManagerEntry> Entry = MakeShared<FUIWTManagerEntry>();
    Entry->Id = InPreviewObject.Id;
    Entry->WidgetDisplay = DisplayNameForEntry(InPreviewObject);
    if (!InPreviewObject.IsOriginal())
    {
      Entry->BlueprintDisplay =
          InPreviewObject.WidgetClass.ToSoftObjectPath().GetAssetName();
      Entry->BlueprintDisplay.RemoveFromEnd(TEXT("_C"));
    }
    Entry->Note = InPreviewObject.Note;
    Entry->bIsOriginal = InPreviewObject.IsOriginal();
    Entry->EntryOrder = InEntryOrder;

    const FUIWTCheckpointIndexEntry *AssignedCheckpoint =
        InCheckpoints.Find(InPreviewObject.CheckpointId);
    const bool bCheckpointValid =
        AssignedCheckpoint && AssignedCheckpoint->bValid;

    if (InEditSession.IsEditing(InPreviewObject.Id))
    {
      if (const TSharedPtr<FUIWTLevelOption> Level =
              InEditSession.GetPendingLevel(InPreviewObject.Id))
      {
        Entry->LevelPackagePath = Level->PackagePath;
        Entry->LevelDisplay = Level->Display;
      }
    }
    else
    {
      const FUIWTLevelScanResult *Scan =
          InScanResults.Find(InPreviewObject.Id);
      TSet<FName> AllLevelPaths;
      if (Scan)
      {
        AllLevelPaths.Append(Scan->LevelPackagePaths);
      }

      FName LevelPath = InPreviewObject.LevelPackagePath;
      if (LevelPath.IsNone() && bCheckpointValid)
      {
        LevelPath = FName(*AssignedCheckpoint->Header.MapPackagePath);
      }
      if (LevelPath.IsNone() && Scan && Scan->LevelPackagePaths.Num() > 0)
      {
        LevelPath = Scan->LevelPackagePaths[0];
      }
      if (!LevelPath.IsNone())
      {
        AllLevelPaths.Add(LevelPath);
        Entry->LevelPackagePath = LevelPath;
        Entry->LevelDisplay =
            UIWTLevelScan::MakeLevelDisplayName(LevelPath, AllLevelPaths);
      }
    }

    if (InPreviewObject.CheckpointId.IsValid())
    {
      Entry->CheckpointId = InPreviewObject.CheckpointId;
      if (bCheckpointValid)
      {
        Entry->CheckpointDisplay = AssignedCheckpoint->GetDisplayString();
      }
      else
      {
        Entry->bCheckpointInvalid = true;
        Entry->CheckpointDisplay = DescribeInvalidCheckpoint(AssignedCheckpoint);
      }
    }
    return Entry;
  }

  void SortEntries(TArray<TSharedPtr<FUIWTManagerEntry>> &Entries,
                   EUIWidgetSortField SortField, bool bSortAscending)
  {
    auto SortKey = [SortField](const FUIWTManagerEntry &Entry) -> const FString &
    {
      switch (SortField)
      {
      case EUIWidgetSortField::Level:
        return Entry.LevelDisplay;
      case EUIWidgetSortField::Checkpoint:
        return Entry.CheckpointDisplay;
      case EUIWidgetSortField::Blueprint:
        return Entry.BlueprintDisplay;
      default:
        return Entry.WidgetDisplay;
      }
    };
    auto Compare = [](const FString &X, const FString &Y)
    { return X.Compare(Y, ESearchCase::IgnoreCase); };

    Entries.Sort(
        [&](const TSharedPtr<FUIWTManagerEntry> &A,
            const TSharedPtr<FUIWTManagerEntry> &B)
        {
          if (SortField == EUIWidgetSortField::None &&
              A->EntryOrder != B->EntryOrder)
          {
            return A->EntryOrder < B->EntryOrder;
          }
          if (SortField == EUIWidgetSortField::Blueprint &&
              A->BlueprintDisplay.IsEmpty() != B->BlueprintDisplay.IsEmpty())
          {
            return B->BlueprintDisplay.IsEmpty();
          }
          if (const int32 Result = Compare(SortKey(*A), SortKey(*B)))
          {
            return bSortAscending ? Result < 0 : Result > 0;
          }
          if (const int32 Result = Compare(A->WidgetDisplay, B->WidgetDisplay))
          {
            return Result < 0;
          }
          if (const int32 Result = Compare(A->LevelDisplay, B->LevelDisplay))
          {
            return Result < 0;
          }
          if (const int32 Result =
                  Compare(A->CheckpointDisplay, B->CheckpointDisplay))
          {
            return Result < 0;
          }
          return A->EntryOrder < B->EntryOrder;
        });
  }

  bool EntryMatchesSearch(const FUIWTManagerEntry &Entry,
                          const FString &InSearchText,
                          EUIWTSearchField InSearchField)
  {
    if (InSearchText.IsEmpty())
    {
      return true;
    }
    auto Contains = [&InSearchText](const FString &In)
    { return In.Contains(InSearchText, ESearchCase::IgnoreCase); };

    auto MatchesLevel = [&Entry, &Contains]()
    {
      return Contains(Entry.LevelDisplay) ||
             (!Entry.LevelPackagePath.IsNone() &&
              Contains(Entry.LevelPackagePath.ToString()));
    };

    switch (InSearchField)
    {
    case EUIWTSearchField::Widget:
      return Contains(Entry.WidgetDisplay);
    case EUIWTSearchField::Level:
      return MatchesLevel();
    case EUIWTSearchField::Checkpoint:
      return Contains(Entry.CheckpointDisplay);
    case EUIWTSearchField::Blueprint:
      return Contains(Entry.BlueprintDisplay) || Contains(Entry.Note);
    default:
      return Contains(Entry.WidgetDisplay) || MatchesLevel() ||
             Contains(Entry.CheckpointDisplay) ||
             Contains(Entry.BlueprintDisplay) || Contains(Entry.Note);
    }
  }
}

TArray<TSharedPtr<FUIWTManagerEntry>> UIWTManagerListBuilder::Build(
    const TArray<FWidgetPreviewObject> &InPreviewObjects,
    const FUIWTCheckpointIndex &InCheckpoints,
    const TMap<FGuid, FUIWTLevelScanResult> &InScanResults,
    const FUIWTEntryEditSession &InEditSession,
    const FUIWTManagerListQuery &InQuery)
{
  TArray<TSharedPtr<FUIWTManagerEntry>> Entries;
  Entries.Reserve(InPreviewObjects.Num());

  for (int32 EntryIndex = 0; EntryIndex < InPreviewObjects.Num(); ++EntryIndex)
  {
    const FWidgetPreviewObject &PreviewObject = InPreviewObjects[EntryIndex];
    TSharedRef<FUIWTManagerEntry> Entry =
        MakeEntry(PreviewObject, EntryIndex, InCheckpoints, InScanResults,
                  InEditSession);

    const bool bKeep = InQuery.SearchText.IsEmpty() ||
                       InEditSession.IsEditing(PreviewObject.Id) ||
                       PreviewObject.Id == InQuery.PinnedEntryId ||
                       EntryMatchesSearch(*Entry, InQuery.SearchText,
                                          InQuery.SearchField);
    if (bKeep)
    {
      Entries.Add(Entry);
    }
  }

  SortEntries(Entries, InQuery.SortField, InQuery.bSortAscending);
  return Entries;
}
