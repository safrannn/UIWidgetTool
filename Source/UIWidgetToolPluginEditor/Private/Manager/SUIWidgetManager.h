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

class SBox;
class SEditableTextBox;
class SUIWTNameEditCell;
class UWidgetBlueprint;

DECLARE_DELEGATE_RetVal_TwoParams(bool, FOnUIWTOpenSnapshot,
                                  const FString & /*Path*/,
                                  FText & /*OutError*/);

DECLARE_DELEGATE_TwoParams(FOnUIWTDesignBlueprintChanged, UWidgetBlueprint *,
                           const TArray<FString> & /*AcceptedAssetPaths*/);

DECLARE_DELEGATE_OneParam(FOnUIWTEntryDeleted, const FGuid & /*EntryId*/);

// Main panel is a list of widget preview entries.
class SUIWidgetManager : public SCompoundWidget
{
public:
  // Bound by the owner (FUIWidgetToolPluginEditorModule) when it builds the
  // manager in UIWidgetToolPluginEditor.cpp.
  SLATE_BEGIN_ARGS(SUIWidgetManager) {}
  SLATE_NAMED_SLOT(FArguments, UtilityPanelContent)
  SLATE_ATTRIBUTE(bool, IsRunInFlight)
  SLATE_EVENT(FSimpleDelegate, OnSelectionChanged)
  SLATE_EVENT(FOnUIWTOpenSnapshot, OnOpenSnapshot)
  SLATE_EVENT(FOnUIWTDesignBlueprintChanged, OnDesignBlueprintChanged)
  SLATE_EVENT(FOnUIWTEntryDeleted, OnEntryDeleted)
  SLATE_END_ARGS()

  void Construct(const FArguments &InArgs);
  void RefreshAll();

  // The selected entry's details beside UtilityPanelContent, for the owner
  // to put in its own tab. The list stays in this widget. Each call builds a
  // new panel, so a reopened tab gets a fresh one.
  TSharedRef<SWidget> MakeUtilityPanel();

  // === Selection ===
  const FGuid &GetSelectedEntryId() const { return SelectedEntryId; }

  void PushSelectionToViewer();

  FString GetSelectedSnapshotPath() const;

  void DescribeEntryForRun(const FGuid &InEntryId, FString &OutLevel,
                           FString &OutCheckpoint) const;

  // === Entry lifecycle ===
  bool CanDuplicateSelected() const;
  FReply OnDuplicateSelectedClicked();
  FReply OnDuplicateClicked(FGuid SourceId);

  // === Edit session ===
  const FUIWTEntryEditSession &GetEditSession() const { return EditSession; }
  void BeginCellEdit(FGuid Id, FName Column);

  // === Field pick ===
  void BeginFieldPick(FGuid Id, FName Column);
  bool IsFieldPickOpen(FGuid Id, FName Column) const
  {
    return Id.IsValid() && FieldPickEntryId == Id && FieldPickColumn == Column;
  }
  TSharedRef<SWidget> MakePicker(FGuid Id, FName Column);

  // === Renaming ===
  void RenameCheckpoint(FGuid CheckpointId, const FText &NewName);
  void BeginCheckpointRenameIn(const TSharedPtr<SUIWTNameEditCell> &Cell,
                               const TSharedPtr<FUIWTManagerEntry> &Entry) const;
  void RenameWidget(FGuid EntryId, const FText &NewName);

private:
  // === Slate args and owner bindings ===
  TSharedRef<SWidget> UtilityPanelContent = SNullWidget::NullWidget;
  TAttribute<bool> IsRunInFlight;
  FSimpleDelegate OnSelectionChanged;
  FOnUIWTOpenSnapshot OnOpenSnapshot;
  FOnUIWTDesignBlueprintChanged OnDesignBlueprintChanged;
  FOnUIWTEntryDeleted OnEntryDeleted;

  // === List ===
  TArray<TSharedPtr<FUIWTManagerEntry>> Entries;
  TSharedPtr<SListView<TSharedPtr<FUIWTManagerEntry>>> ListView;
  void RefreshList();
  FReply OnRefreshClicked();

  // === UI building ===
  TSharedRef<SWidget> BuildListPanel();
  TSharedRef<SWidget> BuildListToolbar();
  TSharedRef<SWidget> BuildUtilityPanel();
  TSharedRef<SWidget> BuildSelectedEntryDetails();
  TSharedRef<ITableRow>
  OnGenerateRow(TSharedPtr<FUIWTManagerEntry> Item,
                const TSharedRef<STableViewBase> &Owner);

  // === Search, filter and sort ===
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
  EColumnSortMode::Type GetColumnSortMode(FName ColumnId) const;
  void OnColumnSortModeChanged(EColumnSortPriority::Type Priority,
                               const FName &ColumnId,
                               EColumnSortMode::Type NewSortMode);

  // === Selection ===
  FGuid SelectedEntryId;
  void OnListSelectionChanged(TSharedPtr<FUIWTManagerEntry> Item,
                              ESelectInfo::Type SelectInfo);
  void RestoreSelection(const FGuid &InEntryId, bool bScrollIntoView);
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

  // === Selection helpers ===
  TSharedPtr<FUIWTManagerEntry> FindEntry(const FGuid &InEntryId) const;
  TSharedPtr<FUIWTManagerEntry> FindSelectedEntry() const;
  const FWidgetPreviewObject *FindSelectedPreviewObject() const;
  UWidgetBlueprint *FindSelectedWidgetBlueprint() const;
  bool HasSelection() const { return SelectedEntryId.IsValid(); }

  // === Details panel ===
  float DetailNameFraction = 0.35f;
  float GetDetailNameFraction() const { return DetailNameFraction; }
  void SetDetailNameFraction(float InFraction) { DetailNameFraction = InFraction; }
  FText GetSelectedCellText(FName Column) const;
  FText GetSelectedCheckpointText() const;
  FSlateColor GetSelectedCheckpointColor() const;

  // === Entry lifecycle ===
  FReply OnAddWidgetClicked();
  FText GetDuplicateToolTip() const;
  FReply OnDeleteSelectedClicked();
  FReply OnDeleteClicked(FGuid Id);

  // === Edit session ===
  FUIWTEntryEditSession EditSession;
  void SetPendingWidget(FGuid Id, TSharedPtr<FAssetData> Selection);
  void SetPendingLevel(FGuid Id, TSharedPtr<FUIWTLevelOption> Selection);
  void ConfirmEditingEntries();
  bool IsSelectedEntryEditing() const;
  const FSlateBrush *GetUpdateButtonIcon() const;
  FText GetUpdateButtonToolTip() const;
  FSlateColor GetUpdateButtonColor() const;
  FReply OnUpdateOrConfirmClicked();
  FReply OnUpdateClicked(FGuid Id);
  FReply OnConfirmClicked(FGuid Id);
  FReply OnCancelEditClicked();
  EVisibility GetEditOnlyVisibility() const;

  // === Field pick ===
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

  // Writes the picked widget or level into the entry. True when the widget
  // class changed, so the level scan needs re-running.
  static bool ApplyWidgetSelection(FWidgetPreviewObject &PreviewObject,
                                   const TSharedPtr<FAssetData> &Selection);
  static bool ApplyLevelSelection(FWidgetPreviewObject &PreviewObject,
                                  const TSharedPtr<FUIWTLevelOption> &Level);

  // === Level scan ===
  TMap<FGuid, FUIWTLevelScanResult> ScanResults;
  void RunLevelScan();

  // === Checkpoint ===
  FUIWTCheckpointIndex CheckpointIndex;
  FUIWTDirectoryWatcher CheckpointWatcher;
  void OnCheckpointDirectoryChanged();

  FName GetCheckpointPickLevel(FGuid Id) const;
  TArray<TSharedPtr<FUIWTCheckpointOption>>
  BuildEntryCheckpointOptions(FGuid Id) const;
  void AssignCheckpoint(FGuid EntryId, FGuid CheckpointId);

  FText GetCheckpointDirectoryText() const;
  FReply OnOpenSaveDirectoryClicked();
  const FUIWTCheckpointIndexEntry *FindSelectedCheckpointFile() const;
  bool CanRevealCheckpointFile() const;
  FText GetRevealCheckpointFileToolTip() const;
  FReply OnRevealCheckpointFileClicked();

  // === Play, snapshot and blueprint ===
  FReply OnPlaySelectedClicked();
  FReply OnPlayClicked(TSharedPtr<FUIWTManagerEntry> Entry);
  FReply OnSnapshotSelectedClicked();
  bool SelectedEntryHasWidget() const;
  bool CanPlaySelected() const;
  bool CanSnapshotSelected() const;
  FText GetPlayToolTip() const;
  FText GetSnapshotToolTip() const;
  FReply OnLoadSnapshotClicked(TSharedPtr<FUIWTManagerEntry> Entry);
  FReply OnOpenBlueprintClicked();
  FName GetSelectedLevelPath() const;
  bool CanOpenSelectedLevel() const;
  FText GetOpenLevelToolTip() const;
  FReply OnOpenLevelClicked();
  const FUIWTCheckpointIndexEntry *
  FindEntrySnapshot(const TSharedPtr<FUIWTManagerEntry> &Entry) const;

  // === Renaming ===
  TSharedPtr<SUIWTNameEditCell> WidgetNameCell;
  void BeginWidgetNameEdit();
  TSharedPtr<SUIWTNameEditCell> CheckpointNameCell;
  void BeginCheckpointNameEdit();
  bool CanRenameSelectedCheckpoint() const;

  // === Details panel pickers ===
  // The level and checkpoint rows are always pickers, writing a pick at
  // once. A picker copies its options when made, so they are rebuilt on
  // every selection change and list refresh.
  TSharedPtr<SBox> PanelLevelPickerBox;
  TSharedPtr<SBox> PanelCheckpointPickerBox;
  bool CanPanelPick() const;
  void RebuildPanelPickers();

  // === Entry note ===
  // The entry the note box is being typed into, taken on the first edit so a
  // commit on focus loss still reaches it after the selection has moved.
  FGuid NoteDraftEntryId;
  TSharedPtr<SEditableTextBox> NoteBox;
  FText GetSelectedNoteEditText() const;
  FText GetNoteHintText() const;
  bool IsNoteReadOnly() const;
  void OnNoteTextChanged(const FText &NewText);
  void OnNoteTextCommitted(const FText &NewText, ETextCommit::Type CommitType);
  void SetNote(FGuid EntryId, const FText &NewNote);
};
