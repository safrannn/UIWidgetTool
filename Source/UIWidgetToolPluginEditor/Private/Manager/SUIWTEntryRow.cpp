#include "SUIWTEntryRow.h"

#include "SUIWTManagerCells.h"
#include "SUIWidgetManager.h"
#include "UIWTEntryEditSession.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UIWidgetTool"

using namespace UIWTManagerOptions;

namespace
{
  constexpr float CellHorizontalPadding = 2.f;
}

void SUIWTEntryRow::Construct(const FArguments &InArgs,
                              const TSharedRef<STableViewBase> &OwnerTable)
{
  Entry = InArgs._Entry;
  Owner = InArgs._Owner;
  SMultiColumnTableRow<TSharedPtr<FUIWTManagerEntry>>::Construct(
      FSuperRowType::FArguments(), OwnerTable);
}

TSharedRef<SWidget> SUIWTEntryRow::GenerateWidgetForColumn(const FName &Column)
{
  TSharedRef<SWidget> CellContent = GenerateCellContent(Column);

  FOnClicked OnDuplicateEntry;
  FSimpleDelegate OnRename;
  FSimpleDelegate OnDoubleClicked;
  if (Entry.IsValid() && Owner)
  {
    OnDuplicateEntry = FOnClicked::CreateSP(
        Owner, &SUIWidgetManager::OnDuplicateClicked,
        Entry->Id);

    const bool bPickable = Column == UIWTManagerColumns::Widget ||
                           Column == UIWTManagerColumns::Level ||
                           Column == UIWTManagerColumns::LevelCheckpoint;
    if (Owner->GetEditSession().IsEditing(Entry->Id))
    {
      if (bPickable && Column != UIWTManagerColumns::LevelCheckpoint)
      {
        OnDoubleClicked = FSimpleDelegate::CreateSP(
            Owner, &SUIWidgetManager::BeginCellEdit, Entry->Id, Column);
      }
    }
    else if (bPickable)
    {
      OnDoubleClicked = FSimpleDelegate::CreateSP(
          Owner, &SUIWidgetManager::BeginFieldPick, Entry->Id, Column);
      if (Column == UIWTManagerColumns::LevelCheckpoint &&
          Entry->CheckpointId.IsValid())
      {
        OnRename = FSimpleDelegate::CreateSP(
            this, &SUIWTEntryRow::BeginCheckpointRename);
      }
    }
    else if (Column == UIWTManagerColumns::Blueprint && !Entry->bIsOriginal)
    {
      // A copy's blueprint file is renamed in place; originals show no
      // name here.
      OnDoubleClicked = FSimpleDelegate::CreateSP(
          this, &SUIWTEntryRow::BeginBlueprintRename);
    }
  }

  TSharedRef<SWidget> Cell =
      SNew(SUIWTCopyableCell)
          .CopyText(TAttribute<FText>::CreateSP(
              this, &SUIWTEntryRow::GetCopyTextForColumn, Column))
          .OnRename(OnRename)
          .OnDuplicateEntry(OnDuplicateEntry)
          .OnDoubleClicked(OnDoubleClicked)[CellContent];

  return SNew(SBox)
      .HeightOverride(Height)
      .VAlign(VAlign_Center)
      .Padding(FMargin(CellHorizontalPadding, 0.f))[Cell];
}

void SUIWTEntryRow::BeginCheckpointRename()
{
  Owner->BeginCheckpointRenameIn(CheckpointNameCell, Entry);
}

void SUIWTEntryRow::BeginBlueprintRename()
{
  Owner->BeginBlueprintRenameIn(BlueprintNameCell, Entry);
}

FText SUIWTEntryRow::GetCopyTextForColumn(FName Column) const
{
  if (!Entry.IsValid())
  {
    return LOCTEXT("Missing", "<missing>");
  }

  const FGuid EntryId = Entry->Id;
  if (Owner->GetEditSession().IsEditing(EntryId))
  {
    if (Column == UIWTManagerColumns::Widget)
    {
      const TSharedPtr<FAssetData> Selection =
          Owner->GetEditSession().GetPendingWidget(EntryId);
      return IsNoneWidgetOption(Selection)
                 ? GetNoneOptionText()
                 : FText::FromName(Selection->AssetName);
    }
    if (Column == UIWTManagerColumns::Level)
    {
      const TSharedPtr<FUIWTLevelOption> Selection =
          Owner->GetEditSession().GetPendingLevel(EntryId);
      return IsNoneLevelOption(Selection)
                 ? GetNoneOptionText()
                 : FText::FromString(Selection->Display);
    }
  }

  return UIWTManagerColumns::CellText(*Entry, Column);
}

TSharedRef<SWidget> SUIWTEntryRow::GenerateCellContent(const FName &Column)
{
  const FWidgetPreviewObject *WidgetPreviewObject =
      Entry.IsValid()
          ? UUIWidgetPreviewObjectManagerSettings::Get()->FindWidgetPreviewObject(
                Entry->Id)
          : nullptr;
  if (!WidgetPreviewObject)
  {
    return SNew(STextBlock).Text(LOCTEXT("Missing", "<missing>"));
  }

  const FGuid EntryId = Entry->Id;
  const bool bEditing = Owner->GetEditSession().IsEditing(EntryId);
  const bool bShowPicker = bEditing || Owner->IsFieldPickOpen(EntryId, Column);

  if (Column == UIWTManagerColumns::Widget)
  {
    if (bShowPicker)
    {
      return Owner->MakePicker(EntryId, Column);
    }

    const bool bHasWidget = !WidgetPreviewObject->WidgetClass.IsNull();
    return SNew(STextBlock)
        .Text(GetCopyTextForColumn(UIWTManagerColumns::Widget))
        .ColorAndOpacity(bHasWidget ? FSlateColor::UseForeground()
                                    : FSlateColor::UseSubduedForeground());
  }

  if (Column == UIWTManagerColumns::Level)
  {
    if (bShowPicker)
    {
      return Owner->MakePicker(EntryId, Column);
    }

    if (Entry->LevelDisplay.IsEmpty())
    {
      return SNew(STextBlock)
          .Text(GetCopyTextForColumn(UIWTManagerColumns::Level))
          .ToolTipText(LOCTEXT("NoLevelTip",
                               "No level references this widget, and no "
                               "checkpoint is assigned."))
          .ColorAndOpacity(FSlateColor::UseSubduedForeground());
    }
    return SNew(STextBlock)
        .Text(GetCopyTextForColumn(UIWTManagerColumns::Level))
        .ToolTipText(FText::FromName(Entry->LevelPackagePath));
  }

  if (Column == UIWTManagerColumns::LevelCheckpoint)
  {
    if (bShowPicker)
    {
      CheckpointNameCell.Reset();
      return Owner->MakePicker(EntryId, Column);
    }

    return SAssignNew(CheckpointNameCell, SUIWTNameEditCell)
        .OnCommitted(Owner, &SUIWidgetManager::RenameCheckpoint)
        .HintText(LOCTEXT("RenameHint", "Leave empty for <map>_<captured at>"))
            [SNew(STextBlock)
                 .Text(GetCopyTextForColumn(UIWTManagerColumns::LevelCheckpoint))
                 .ColorAndOpacity(
                     UIWTManagerColumns::CheckpointCellColor(*Entry))];
  }

  if (Column == UIWTManagerColumns::Blueprint)
  {
    if (Entry->bIsOriginal)
    {
      return SNew(STextBlock)
          .Text(LOCTEXT("BlueprintOriginalCell", "original"))
          .ToolTipText(LOCTEXT("BlueprintOriginalTip",
                               "Hand-added entry. Its blueprint under Content/ "
                               "is never edited; Duplicate it to chat."))
          .ColorAndOpacity(FSlateColor::UseSubduedForeground());
    }
    return SAssignNew(BlueprintNameCell, SUIWTNameEditCell)
        .OnCommitted(Owner, &SUIWidgetManager::RenameWidget)
        .HintText(LOCTEXT("WidgetRenameHint", "New blueprint name"))
            [SNew(STextBlock)
                 .Text(GetCopyTextForColumn(UIWTManagerColumns::Blueprint))
                 .ToolTipText(Entry->Note.IsEmpty()
                                  ? LOCTEXT("BlueprintNoNoteTip", "No note yet.")
                                  : FText::FromString(Entry->Note))];
  }

  return SNullWidget::NullWidget;
}

#undef LOCTEXT_NAMESPACE
