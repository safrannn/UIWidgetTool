#pragma once

#include "CoreMinimal.h"
#include "UIWTCheckpointIndex.h"
#include "UIWTDirectoryWatcher.h"
#include "UIWTEntryEditSession.h"
#include "UIWTLevelScan.h"
#include "UIWTManagerListBuilder.h"
#include "UIWTManagerTypes.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"

class SUIWTNameEditCell;
class UWidgetBlueprint;

DECLARE_DELEGATE_RetVal_TwoParams(bool, FOnUIWTOpenSnapshot,
                                  const FString & /*Path*/,
                                  FText & /*OutError*/);

DECLARE_DELEGATE_TwoParams(FOnUIWTDesignBlueprintChanged, UWidgetBlueprint *,
                           const TArray<FString> & /*AcceptedAssetPaths*/);

DECLARE_DELEGATE_OneParam(FOnUIWTEntryDeleted, const FGuid & /*EntryId*/);

class SUIWidgetManager : public SCompoundWidget
{
public:
  SLATE_BEGIN_ARGS(SUIWidgetManager) {}
  SLATE_NAMED_SLOT(FArguments, UtilityPanelContent)
  SLATE_ATTRIBUTE(bool, IsRunInFlight)
  SLATE_EVENT(FSimpleDelegate, OnSelectionChanged)
  SLATE_EVENT(FOnUIWTOpenSnapshot, OnOpenSnapshot)
  SLATE_EVENT(FOnUIWTDesignBlueprintChanged, OnDesignBlueprintChanged)
  SLATE_EVENT(FOnUIWTEntryDeleted, OnEntryDeleted)
  SLATE_END_ARGS()

  void Construct(const FArguments &InArgs);

  void RefreshList();

  void RefreshAll();

  const FGuid &GetSelectedEntryId() const { return SelectedEntryId; }
  bool CanDuplicateSelected() const;
  FReply OnDuplicateSelectedClicked();

  void DescribeEntryForRun(const FGuid &InEntryId, FString &OutLevel,
                           FString &OutCheckpoint) const;

  // Re-sends the selected entry's blueprint through OnDesignBlueprintChanged.
  // The owner calls this when the viewer appears or the blueprint changed
  // underneath it.
  void PushSelectionToViewer();

  // Snapshot file of the selected entry's checkpoint; empty when there is no
  // selection, no checkpoint, or no snapshot on disk. The owner loads it into
  // the viewer as the selection moves.
  FString GetSelectedSnapshotPath() const;

  // What the list's rows read and do. Picks go through the manager rather
  // than straight into the session so the list rescans and refreshes.
  const FUIWTEntryEditSession &GetEditSession() const { return EditSession; }
  // Starts editing the entry (if it is not already) with the column's picker
  // opening as soon as its row appears.
  void BeginCellEdit(FGuid Id, FName Column);
  FReply OnDuplicateClicked(FGuid SourceId);

  // A double-click on a Widget, Level or Level Checkpoint cell of a row that
  // is not being edited shows that one cell's picker, opened, without
  // starting an edit session. A pick writes just that field to the settings;
  // the cell reverts to text once the picker's menu closes either way.
  void BeginFieldPick(FGuid Id, FName Column);
  bool IsFieldPickOpen(FGuid Id, FName Column) const
  {
    return Id.IsValid() && FieldPickEntryId == Id && FieldPickColumn == Column;
  }
  // The picker a row shows in a Widget, Level or Level Checkpoint cell: the
  // entry's edit picker while it is being edited (a pick stays pending until
  // confirmed, except a checkpoint, which assigns at once), otherwise the
  // field pick in progress on that cell.
  TSharedRef<SWidget> MakePicker(FGuid Id, FName Column);

  void RenameCheckpoint(FGuid CheckpointId, const FText &NewName);
  void BeginCheckpointRenameIn(const TSharedPtr<SUIWTNameEditCell> &Cell,
                               const TSharedPtr<FUIWTManagerEntry> &Entry) const;

private:
  void SetPendingWidget(FGuid Id, TSharedPtr<FAssetData> Selection);
  void SetPendingLevel(FGuid Id, TSharedPtr<FUIWTLevelOption> Selection);

  // The level whose checkpoints the entry's picker offers: the pending level
  // while editing, otherwise the level the list shows for the entry.
  FName GetCheckpointPickLevel(FGuid Id) const;
  TArray<TSharedPtr<FUIWTCheckpointOption>>
  BuildEntryCheckpointOptions(FGuid Id) const;
  void AssignCheckpoint(FGuid EntryId, FGuid CheckpointId);

  TSharedRef<SWidget> UtilityPanelContent = SNullWidget::NullWidget;
  TAttribute<bool> IsRunInFlight;
  FSimpleDelegate OnSelectionChanged;
  FOnUIWTOpenSnapshot OnOpenSnapshot;
  FOnUIWTDesignBlueprintChanged OnDesignBlueprintChanged;
  FOnUIWTEntryDeleted OnEntryDeleted;

  TArray<TSharedPtr<FUIWTManagerEntry>> Entries;
  TSharedPtr<SListView<TSharedPtr<FUIWTManagerEntry>>> ListView;

  TSharedRef<SWidget> BuildListPanel();
  TSharedRef<SWidget> BuildListToolbar();
  TSharedRef<SWidget> BuildUtilityPanel();
  TSharedRef<SWidget> BuildSelectedEntryDetails();

  FUIWTManagerListQuery ListQuery;

  void OnSearchTextChanged(const FText &NewText);
  TSharedRef<SWidget> BuildFilterMenu();
  void SetSearchField(EUIWTSearchField InField);
  bool IsSearchField(EUIWTSearchField InField) const;
  FText GetSearchFieldLabel() const;
  EVisibility GetSearchFieldLabelVisibility() const;
  FText GetFilterToolTip() const;
  EVisibility GetEmptySearchMessageVisibility() const;
  FText GetEmptySearchMessageText() const;

  FUIWTEntryEditSession EditSession;

  FGuid FieldPickEntryId;
  FName FieldPickColumn;
  TArray<TSharedPtr<FAssetData>> FieldPickWidgetOptions;
  TSharedPtr<FAssetData> FieldPickWidget;
  TArray<TSharedPtr<FUIWTLevelOption>> FieldPickLevelOptions;
  TSharedPtr<FUIWTLevelOption> FieldPickLevel;
  void EndFieldPick(const FGuid &Id);
  void OnFieldPickMenuOpenChanged(bool bIsOpen, FGuid Id, FName Column);
  void PickWidget(FGuid Id, TSharedPtr<FAssetData> Selection);
  void PickLevel(FGuid Id, TSharedPtr<FUIWTLevelOption> Selection);

  bool CanRenameSelectedCheckpoint() const;

  // Renaming the selected entry's widget from the details renames its
  // blueprint's asset too - a copy in the mount, or a hand-added entry's
  // blueprint under Content/ (the one place the tool does edit there).
  TSharedPtr<SUIWTNameEditCell> WidgetNameCell;
  void BeginWidgetNameEdit();
  void RenameWidget(FGuid EntryId, const FText &NewName);

  // Writes the picked widget or level into the entry. True when the widget
  // class changed, so the level scan needs re-running.
  static bool ApplyWidgetSelection(FWidgetPreviewObject &PreviewObject,
                                   const TSharedPtr<FAssetData> &Selection);
  static bool ApplyLevelSelection(FWidgetPreviewObject &PreviewObject,
                                  const TSharedPtr<FUIWTLevelOption> &Level);

  FUIWTCheckpointIndex CheckpointIndex;
  FUIWTDirectoryWatcher CheckpointWatcher;
  void OnCheckpointDirectoryChanged();

  TMap<FGuid, FUIWTLevelScanResult> ScanResults;

  TSharedRef<ITableRow>
  OnGenerateRow(TSharedPtr<FUIWTManagerEntry> Item,
                const TSharedRef<STableViewBase> &Owner);

  void RunLevelScan();

  FGuid SelectedEntryId;
  void OnListSelectionChanged(TSharedPtr<FUIWTManagerEntry> Item,
                              ESelectInfo::Type SelectInfo);
  void ConfirmEditingEntries();
  void RestoreSelection(const FGuid &InEntryId, bool bScrollIntoView);

  // What the owner's selection handler reads: it reloads the blueprint,
  // rebuilds the viewer's design tree and stats the snapshot file, and the
  // list is rebuilt on every keystroke in the search box. The event fires
  // only when one of these actually moved.
  struct FSelectionSignature
  {
    FGuid EntryId;
    FString SnapshotPath;
    FSoftObjectPath WidgetClass;

    bool operator!=(const FSelectionSignature &Other) const
    {
      return EntryId != Other.EntryId || SnapshotPath != Other.SnapshotPath ||
             WidgetClass != Other.WidgetClass;
    }
  };
  FSelectionSignature MakeSelectionSignature() const;
  void NotifySelectionChanged();
  FSelectionSignature NotifiedSelection;

  TSharedPtr<FUIWTManagerEntry> FindEntry(const FGuid &InEntryId) const;
  TSharedPtr<FUIWTManagerEntry> FindSelectedEntry() const;
  const FWidgetPreviewObject *FindSelectedPreviewObject() const;
  bool HasSelection() const { return SelectedEntryId.IsValid(); }

  bool IsSelectedEntryEditing() const;
  const FSlateBrush *GetUpdateButtonIcon() const;
  FText GetUpdateButtonToolTip() const;
  FSlateColor GetUpdateButtonColor() const;
  FReply OnUpdateOrConfirmClicked();
  FReply OnDeleteSelectedClicked();

  FReply OnCancelEditClicked();
  EVisibility GetEditOnlyVisibility() const;

  FReply OnPlaySelectedClicked();
  FReply OnSnapshotSelectedClicked();
  bool SelectedEntryHasWidget() const;
  bool CanPlaySelected() const;
  bool CanSnapshotSelected() const;
  FText GetPlayToolTip() const;
  FText GetSnapshotToolTip() const;
  FText GetDuplicateToolTip() const;

  FText GetSelectedCellText(FName Column) const;
  FText GetSelectedLevelToolTip() const;
  FText GetSelectedCheckpointText() const;
  FSlateColor GetSelectedCheckpointColor() const;

  const FUIWTCheckpointIndexEntry *FindSelectedCheckpointFile() const;
  bool CanRevealCheckpointFile() const;
  FText GetRevealCheckpointFileToolTip() const;
  FReply OnRevealCheckpointFileClicked();

  FReply OnOpenBlueprintClicked();
  UWidgetBlueprint *FindSelectedWidgetBlueprint() const;

  FText GetSelectedNoteText() const;

  // Double-clicking the Note detail edits a copy's note in place; originals
  // carry no note, matching the Claude toolset's SetEntryNote.
  TSharedPtr<SUIWTNameEditCell> NoteCell;
  void BeginNoteEdit();
  void SetNote(FGuid EntryId, const FText &NewNote);

  FReply OnUpdateClicked(FGuid Id);
  FReply OnConfirmClicked(FGuid Id);
  FReply OnPlayClicked(TSharedPtr<FUIWTManagerEntry> Entry);
  FReply OnDeleteClicked(FGuid Id);
  FReply OnAddWidgetClicked();
  FReply OnRefreshClicked();
  FReply OnOpenSaveDirectoryClicked();

  FReply OnLoadSnapshotClicked(TSharedPtr<FUIWTManagerEntry> Entry);

  const FUIWTCheckpointIndexEntry *
  FindEntrySnapshot(const TSharedPtr<FUIWTManagerEntry> &Entry) const;

  TSharedPtr<SUIWTNameEditCell> CheckpointNameCell;
  void BeginCheckpointNameEdit();

  FText GetCheckpointDirectoryText() const;

  EColumnSortMode::Type GetColumnSortMode(FName ColumnId) const;
  void OnColumnSortModeChanged(EColumnSortPriority::Type Priority,
                               const FName &ColumnId,
                               EColumnSortMode::Type NewSortMode);
};
