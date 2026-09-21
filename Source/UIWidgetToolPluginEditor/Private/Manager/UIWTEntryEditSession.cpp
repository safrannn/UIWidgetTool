#include "UIWTEntryEditSession.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/World.h"
#include "UIWTCheckpointIndex.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "WidgetBlueprint.h"

namespace
{
  TArray<FAssetData> GetAvailableWidgetBlueprints()
  {
    TArray<FAssetData> Result;
    IAssetRegistry::GetChecked().GetAssetsByClass(
        UWidgetBlueprint::StaticClass()->GetClassPathName(), Result, true);
    Result.Sort([](const FAssetData &A, const FAssetData &B)
                { return A.AssetName.LexicalLess(B.AssetName); });
    return Result;
  }

  TArray<FName> GetAllLevelPackagePaths()
  {
    TArray<FAssetData> Worlds;
    IAssetRegistry::GetChecked().GetAssetsByClass(
        UWorld::StaticClass()->GetClassPathName(), Worlds, false);

    TSet<FName> Unique;
    for (const FAssetData &World : Worlds)
    {
      if (World.PackageName.ToString().StartsWith(TEXT("/Game/")))
      {
        Unique.Add(World.PackageName);
      }
    }

    TArray<FName> Result = Unique.Array();
    Result.Sort([](const FName &A, const FName &B)
                { return A.LexicalLess(B); });
    return Result;
  }
}

TArray<FGuid> FUIWTEntryEditSession::GetEditingIds() const
{
  TArray<FGuid> Ids;
  States.GetKeys(Ids);
  return Ids;
}

TArray<TSharedPtr<FAssetData>>
FUIWTEntryEditSession::MakeWidgetOptions(FGuid Id,
                                         TSharedPtr<FAssetData> &OutCurrent)
{
  TArray<TSharedPtr<FAssetData>> Options;
  Options.Add(UIWTManagerOptions::MakeNoneWidgetOption());
  for (const FAssetData &Asset : GetAvailableWidgetBlueprints())
  {
    Options.Add(MakeShared<FAssetData>(Asset));
  }

  OutCurrent = Options[0];
  const FWidgetPreviewObject *PreviewObject =
      UUIWidgetPreviewObjectManagerSettings::Get()->FindWidgetPreviewObject(Id);
  if (!PreviewObject)
  {
    return Options;
  }

  // Matched on the package, not the asset name: two WBP_Foo in different
  // folders are different blueprints, and the name alone would show the
  // wrong one as the entry's current pick.
  const FString PackagePath =
      PreviewObject->WidgetClass.ToSoftObjectPath().GetLongPackageName();
  if (PackagePath.IsEmpty())
  {
    return Options;
  }
  const FName PackageName(*PackagePath);
  if (const TSharedPtr<FAssetData> *Found = Options.FindByPredicate(
          [PackageName](const TSharedPtr<FAssetData> &Option)
          { return Option->PackageName == PackageName; }))
  {
    OutCurrent = *Found;
  }
  return Options;
}

void FUIWTEntryEditSession::Begin(FGuid Id)
{
  FUIWTEntryEditState &State = States.FindOrAdd(Id);
  State.WidgetOptions = MakeWidgetOptions(Id, State.PendingWidget);
}

void FUIWTEntryEditSession::End(FGuid Id)
{
  States.Remove(Id);
  if (AutoOpenPickerEntry == Id)
  {
    AutoOpenPickerEntry.Invalidate();
  }
}

const TArray<TSharedPtr<FAssetData>> *
FUIWTEntryEditSession::GetWidgetOptions(FGuid Id) const
{
  const FUIWTEntryEditState *State = States.Find(Id);
  return State ? &State->WidgetOptions : nullptr;
}

TSharedPtr<FAssetData> FUIWTEntryEditSession::GetPendingWidget(FGuid Id) const
{
  const FUIWTEntryEditState *State = States.Find(Id);
  return State ? State->PendingWidget : nullptr;
}

void FUIWTEntryEditSession::SetPendingWidget(FGuid Id,
                                             TSharedPtr<FAssetData> InSelection)
{
  if (FUIWTEntryEditState *State = States.Find(Id))
  {
    State->PendingWidget = InSelection;
  }
}

const TArray<TSharedPtr<FUIWTLevelOption>> *
FUIWTEntryEditSession::GetLevelOptions(FGuid Id) const
{
  const FUIWTEntryEditState *State = States.Find(Id);
  return State ? &State->LevelOptions : nullptr;
}

TSharedPtr<FUIWTLevelOption>
FUIWTEntryEditSession::GetPendingLevel(FGuid Id) const
{
  const FUIWTEntryEditState *State = States.Find(Id);
  return State ? State->PendingLevel : nullptr;
}

void FUIWTEntryEditSession::SetPendingLevel(
    FGuid Id, TSharedPtr<FUIWTLevelOption> InSelection)
{
  if (FUIWTEntryEditState *State = States.Find(Id))
  {
    State->PendingLevel = InSelection;
  }
}

TArray<TSharedPtr<FUIWTLevelOption>> FUIWTEntryEditSession::MakeLevelOptions(
    FGuid Id, const TMap<FGuid, FUIWTLevelScanResult> &InScanResults,
    const FUIWTCheckpointIndex &InCheckpoints,
    const TSharedPtr<FUIWTLevelOption> &InPreferred,
    TSharedPtr<FUIWTLevelOption> &OutCurrent)
{
  const FWidgetPreviewObject *PreviewObject =
      UUIWidgetPreviewObjectManagerSettings::Get()->FindWidgetPreviewObject(Id);

  TArray<FName> Paths;
  TSet<FName> AllLevelPaths;
  if (const FUIWTLevelScanResult *Scan = InScanResults.Find(Id))
  {
    Paths = Scan->LevelPackagePaths;
  }
  if (Paths.Num() == 0)
  {
    Paths = GetAllLevelPackagePaths();
  }
  if (PreviewObject && !PreviewObject->LevelPackagePath.IsNone())
  {
    Paths.AddUnique(PreviewObject->LevelPackagePath);
  }
  AllLevelPaths.Append(Paths);

  TArray<TSharedPtr<FUIWTLevelOption>> Options;
  Options.Reserve(Paths.Num() + 1);
  Options.Add(UIWTManagerOptions::MakeNoneLevelOption());
  for (const FName &Path : Paths)
  {
    TSharedRef<FUIWTLevelOption> Option = MakeShared<FUIWTLevelOption>();
    Option->PackagePath = Path;
    Option->Display = UIWTLevelScan::MakeLevelDisplayName(Path, AllLevelPaths);
    Options.Add(Option);
  }

  auto FindOption = [&Options](FName PackagePath) -> TSharedPtr<FUIWTLevelOption>
  {
    const TSharedPtr<FUIWTLevelOption> *Found = Options.FindByPredicate(
        [PackagePath](const TSharedPtr<FUIWTLevelOption> &Option)
        { return Option->PackagePath == PackagePath; });
    return Found ? *Found : nullptr;
  };

  TSharedPtr<FUIWTLevelOption> InitialSelection;
  if (InPreferred.IsValid())
  {
    InitialSelection = FindOption(InPreferred->PackagePath);
  }
  if (!InitialSelection.IsValid() && PreviewObject &&
      !PreviewObject->LevelPackagePath.IsNone())
  {
    InitialSelection = FindOption(PreviewObject->LevelPackagePath);
  }
  if (!InitialSelection.IsValid())
  {
    if (const FUIWTCheckpointIndexEntry *Assigned =
            PreviewObject ? InCheckpoints.FindValid(PreviewObject->CheckpointId)
                          : nullptr)
    {
      InitialSelection = FindOption(FName(*Assigned->Header.MapPackagePath));
    }
  }
  if (!InitialSelection.IsValid())
  {
    InitialSelection = Options[0];
  }

  OutCurrent = InitialSelection;
  return Options;
}

void FUIWTEntryEditSession::BuildLevelOptions(
    FGuid Id, const TMap<FGuid, FUIWTLevelScanResult> &InScanResults,
    const FUIWTCheckpointIndex &InCheckpoints)
{
  if (FUIWTEntryEditState *State = States.Find(Id))
  {
    const TSharedPtr<FUIWTLevelOption> Preferred = State->PendingLevel;
    State->LevelOptions = MakeLevelOptions(Id, InScanResults, InCheckpoints,
                                           Preferred, State->PendingLevel);
  }
}

void FUIWTEntryEditSession::RequestAutoOpenPicker(FGuid Id, FName Column)
{
  AutoOpenPickerEntry = Id;
  AutoOpenPickerColumn = Column;
}

bool FUIWTEntryEditSession::TakeAutoOpenPicker(FGuid Id, FName Column)
{
  if (AutoOpenPickerEntry != Id || AutoOpenPickerColumn != Column)
  {
    return false;
  }
  AutoOpenPickerEntry.Invalidate();
  return true;
}
