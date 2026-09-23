#pragma once

#include "CoreMinimal.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SCompoundWidget.h"

// One name | value row drawn like a Details panel property row. Rows in one
// panel share NameFraction, so their columns line up and resize together.
class SUIWTDetailRow : public SCompoundWidget
{
public:
  static constexpr float MinHeight = 28.f;

  SLATE_BEGIN_ARGS(SUIWTDetailRow) : _NameFraction(0.35f) {}
  SLATE_ATTRIBUTE(FText, Label)
  SLATE_ATTRIBUTE(float, NameFraction)
  SLATE_EVENT(SSplitter::FOnSlotResized, OnNameFractionChanged)
  SLATE_DEFAULT_SLOT(FArguments, Content)
  SLATE_END_ARGS()

  void Construct(const FArguments &InArgs);

private:
  FSlateColor GetBackgroundColor() const;
  float GetNameFraction() const { return NameFraction.Get(); }
  float GetValueFraction() const { return 1.f - NameFraction.Get(); }
  void OnNameResized(float NewValue);
  void OnValueResized(float NewValue);

  TAttribute<float> NameFraction;
  SSplitter::FOnSlotResized OnNameFractionChanged;
};

namespace UIWTDetails
{
  // A collapsible Details panel category over the given rows.
  TSharedRef<SWidget> MakeCategory(const FText &Title,
                                   const TArray<TSharedRef<SWidget>> &Rows);
}
