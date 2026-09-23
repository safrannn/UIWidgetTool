#include "SUIWTDetailRow.h"

#include "Styling/AppStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
  // Lines the row names up under the category title, past its arrow.
  constexpr float NameIndent = 22.f;
  constexpr float GridLineWidth = 1.f;
}

void SUIWTDetailRow::Construct(const FArguments &InArgs)
{
  NameFraction = InArgs._NameFraction;
  OnNameFractionChanged = InArgs._OnNameFractionChanged;

  ChildSlot
      [SNew(SBorder)
           .BorderImage(FAppStyle::GetBrush("DetailsView.CategoryMiddle"))
           .BorderBackgroundColor(this, &SUIWTDetailRow::GetBackgroundColor)
           .Padding(0.f)
               [SNew(SSplitter)
                    .Style(FAppStyle::Get(), "DetailsView.Splitter")
                    .PhysicalSplitterHandleSize(GridLineWidth)
                    .HitDetectionSplitterHandleSize(5.f) +
                SSplitter::Slot()
                    .Value(this, &SUIWTDetailRow::GetNameFraction)
                    .OnSlotResized(this, &SUIWTDetailRow::OnNameResized)
                        [SNew(SBox)
                             .MinDesiredHeight(MinHeight)
                             .VAlign(VAlign_Center)
                             .Padding(FMargin(NameIndent, 2.f, 4.f, 2.f))
                                 [SNew(STextBlock).Text(InArgs._Label)]] +
                SSplitter::Slot()
                    .Value(this, &SUIWTDetailRow::GetValueFraction)
                    .OnSlotResized(this, &SUIWTDetailRow::OnValueResized)
                        [SNew(SBox)
                             .MinDesiredHeight(MinHeight)
                             .VAlign(VAlign_Center)
                             .Padding(FMargin(8.f, 2.f, 4.f, 2.f))
                                 [InArgs._Content.Widget]]]];
}

FSlateColor SUIWTDetailRow::GetBackgroundColor() const
{
  return FAppStyle::Get().GetSlateColor(IsHovered() ? "Colors.Header"
                                                    : "Colors.Panel");
}

void SUIWTDetailRow::OnNameResized(float NewValue)
{
  OnNameFractionChanged.ExecuteIfBound(NewValue);
}

void SUIWTDetailRow::OnValueResized(float NewValue)
{
  OnNameFractionChanged.ExecuteIfBound(1.f - NewValue);
}

TSharedRef<SWidget>
UIWTDetails::MakeCategory(const FText &Title,
                          const TArray<TSharedRef<SWidget>> &Rows)
{
  // The body shows the grid-line color through the gaps between rows.
  TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);
  for (const TSharedRef<SWidget> &Row : Rows)
  {
    Body->AddSlot().AutoHeight().Padding(FMargin(0.f, GridLineWidth, 0.f, 0.f))
        [Row];
  }

  return SNew(SExpandableArea)
      .BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
      .BodyBorderImage(FAppStyle::GetBrush("DetailsView.GridLine"))
      .AreaTitle(Title)
      // The font every plain STextBlock uses, so titles match the list rows.
      .AreaTitleFont(
          FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText").Font)
      .HeaderPadding(FMargin(6.f, 5.f))
      .Padding(0.f)
      .AllowAnimatedTransition(false)
      .BodyContent()[Body];
}
