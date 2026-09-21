#include "SUIWTSnapshotTreeRow.h"

#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SExpanderArrow.h"

#define LOCTEXT_NAMESPACE "UIWidgetToolSnapshotTree"

namespace UIWTSnapshotColumns
{

  const FName WidgetName(TEXT("WidgetName"));
  const FName Source(TEXT("Source"));
  const FName Visibility(TEXT("Visibility"));
  const FName Focusable(TEXT("Focusable"));
  const FName Enabled(TEXT("Enabled"));
  const FName Volatile(TEXT("Volatile"));
  const FName HasActiveTimer(TEXT("HasActiveTimer"));
  const FName Clipping(TEXT("Clipping"));
  const FName LayerId(TEXT("LayerId"));
  const FName ActualSize(TEXT("ActualSize"));
  const FName Address(TEXT("Address"));

  const TArray<FColumnInfo> &GetAll()
  {
    // Id, label, hidden by default, fill width, fixed width.
    static const TArray<FColumnInfo> Columns = {
        {WidgetName, LOCTEXT("ColWidgetName", "Widget Name"), false, 0.45f, 0.f},
        {Source, LOCTEXT("ColSource", "Source"), false, 0.25f, 0.f},
        {Visibility, LOCTEXT("ColVisibility", "Visibility"), false, 0.f, 90.f},
        {Focusable, LOCTEXT("ColFocusable", "Focusable"), false, 0.f, 44.f},
        {Enabled, LOCTEXT("ColEnabled", "Enabled"), false, 0.f, 44.f},
        {Volatile, LOCTEXT("ColVolatile", "Volatile"), true, 0.f, 44.f},
        {HasActiveTimer, LOCTEXT("ColTimer", "Timer"), true, 0.f, 44.f},
        {Clipping, LOCTEXT("ColClipping", "Clipping"), false, 0.f, 80.f},
        {LayerId, LOCTEXT("ColLayer", "Layer"), false, 0.f, 50.f},
        {ActualSize, LOCTEXT("ColSize", "Size"), false, 0.f, 100.f},
        {Address, LOCTEXT("ColAddress", "Address"), true, 0.f, 120.f},
    };
    return Columns;
  }

  const FColumnInfo *Find(const FName &InId)
  {
    return GetAll().FindByPredicate(
        [&InId](const FColumnInfo &In)
        { return In.Id == InId; });
  }

  TArray<FName> GetDefaultHiddenColumns()
  {
    TArray<FName> Hidden;
    for (const FColumnInfo &Column : GetAll())
    {
      if (Column.bHiddenByDefault)
      {
        Hidden.Add(Column.Id);
      }
    }
    return Hidden;
  }

}

namespace
{

  TSharedRef<SWidget> MakeTextCell(const FText &InText,
                                   const FText &InToolTip = FText::GetEmpty())
  {
    return SNew(SBox)
        .VAlign(VAlign_Center)
        .Padding(FMargin(4.f, 0.f))
            [SNew(STextBlock).Text(InText).ToolTipText(InToolTip)];
  }

  TSharedRef<SWidget> MakeFlagCell(bool bInValue)
  {
    return SNew(SBox)
        .VAlign(VAlign_Center)
        .HAlign(HAlign_Center)
            [SNew(SImage)
                 .Image(FAppStyle::Get().GetBrush(bInValue ? "Icons.Check"
                                                           : "NoBrush"))
                 .ColorAndOpacity(FSlateColor::UseForeground())];
  }

}

void SUIWTSnapshotTreeRow::Construct(
    const FArguments &InArgs, const TSharedRef<STableViewBase> &InOwnerTable)
{
  Node = InArgs._Node;

  SMultiColumnTableRow<TSharedRef<FUIWTSnapshotNode>>::Construct(
      FSuperRowType::FArguments(), InOwnerTable);
}

FSlateColor SUIWTSnapshotTreeRow::GetRowColor() const
{
  if (!Node.IsValid())
  {
    return FSlateColor::UseForeground();
  }
  return Node->bVisible ? FSlateColor::UseForeground()
                        : FSlateColor::UseSubduedForeground();
}

TSharedRef<SWidget>
SUIWTSnapshotTreeRow::GenerateWidgetForColumn(const FName &InColumnName)
{
  if (!Node.IsValid())
  {
    return SNullWidget::NullWidget;
  }

  TSharedRef<SWidget> Cell = SNullWidget::NullWidget;

  if (InColumnName == UIWTSnapshotColumns::WidgetName)
  {
    return SNew(SHorizontalBox) +
           SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
               [SNew(SExpanderArrow, SharedThis(this)).IndentAmount(12.f)] +
           SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
               [SNew(STextBlock)
                    .Text(Node->WidgetTypeAndShortName)
                    .ToolTipText(Node->WidgetType)
                    .ColorAndOpacity(this, &SUIWTSnapshotTreeRow::GetRowColor)];
  }

  if (InColumnName == UIWTSnapshotColumns::Source)
  {
    const FText ToolTip =
        Node->File.IsEmpty()
            ? FText::GetEmpty()
            : FText::FromString(FString::Printf(TEXT("%s:%d"), *Node->File,
                                                Node->LineNumber));
    Cell = MakeTextCell(Node->WidgetReadableLocation, ToolTip);
  }
  else if (InColumnName == UIWTSnapshotColumns::Visibility)
  {
    Cell = MakeTextCell(Node->WidgetVisibilityText);
  }
  else if (InColumnName == UIWTSnapshotColumns::Focusable)
  {
    Cell = MakeFlagCell(Node->bFocusable);
  }
  else if (InColumnName == UIWTSnapshotColumns::Enabled)
  {
    Cell = MakeFlagCell(Node->bEnabled);
  }
  else if (InColumnName == UIWTSnapshotColumns::Volatile)
  {
    Cell = MakeFlagCell(Node->bIsVolatile || Node->bIsVolatileIndirectly);
  }
  else if (InColumnName == UIWTSnapshotColumns::HasActiveTimer)
  {
    Cell = MakeFlagCell(Node->bHasActiveTimers);
  }
  else if (InColumnName == UIWTSnapshotColumns::Clipping)
  {
    Cell = MakeTextCell(Node->WidgetClippingText);
  }
  else if (InColumnName == UIWTSnapshotColumns::LayerId)
  {
    Cell = MakeTextCell(FText::AsNumber(Node->LayerId));
  }
  else if (InColumnName == UIWTSnapshotColumns::ActualSize)
  {
    const FVector2f Size = TransformPoint(
        Node->AccumulatedLayoutTransform.GetScale(), Node->LocalSize);
    Cell = MakeTextCell(FText::FromString(FString::Printf(
        TEXT("%d x %d"), FMath::RoundToInt(Size.X), FMath::RoundToInt(Size.Y))));
  }
  else if (InColumnName == UIWTSnapshotColumns::Address)
  {
    Cell = MakeTextCell(FText::FromString(
        FString::Printf(TEXT("0x%0llx"), Node->WidgetAddress)));
  }

  return Cell;
}

#undef LOCTEXT_NAMESPACE
