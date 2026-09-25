#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateColor.h"
#include "UIWTManagerTypes.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "UIWidgetTool"

// How a picker shows each kind of option: its label, the tooltip on its row,
// and which option is the "(none)" sentinel. One specialization per option
// type, so every picker of that type reads the same way.
template <typename TOption>
struct FUIWTPickerTraits;

template <>
struct FUIWTPickerTraits<FAssetData>
{
  static FText Label(const TSharedPtr<FAssetData> &In)
  {
    return FText::FromName(In->AssetName);
  }
  static FText ItemToolTip(const TSharedPtr<FAssetData> &In)
  {
    return FText::FromName(In->PackageName);
  }
  static bool IsNone(const TSharedPtr<FAssetData> &In)
  {
    return UIWTManagerOptions::IsNoneWidgetOption(In);
  }
  static TSharedRef<FAssetData> MakeNone()
  {
    return UIWTManagerOptions::MakeNoneWidgetOption();
  }
};

template <>
struct FUIWTPickerTraits<FUIWTLevelOption>
{
  static FText Label(const TSharedPtr<FUIWTLevelOption> &In)
  {
    return FText::FromString(In->Display);
  }
  static FText ItemToolTip(const TSharedPtr<FUIWTLevelOption> &In)
  {
    return FText::FromName(In->PackagePath);
  }
  static bool IsNone(const TSharedPtr<FUIWTLevelOption> &In)
  {
    return UIWTManagerOptions::IsNoneLevelOption(In);
  }
  static TSharedRef<FUIWTLevelOption> MakeNone()
  {
    return UIWTManagerOptions::MakeNoneLevelOption();
  }
};

template <>
struct FUIWTPickerTraits<FUIWTCheckpointOption>
{
  static FText Label(const TSharedPtr<FUIWTCheckpointOption> &In)
  {
    return FText::FromString(In->Display);
  }
  static FText ItemToolTip(const TSharedPtr<FUIWTCheckpointOption> &In)
  {
    return FText::FromString(In->PayloadPath);
  }
  static bool IsNone(const TSharedPtr<FUIWTCheckpointOption> &In)
  {
    return UIWTManagerOptions::IsNoneCheckpointOption(In);
  }
  static TSharedRef<FUIWTCheckpointOption> MakeNone()
  {
    return MakeShared<FUIWTCheckpointOption>();
  }
};

// What one picker offers and does. The option list is copied when the
// picker is made, so it may point at a local.
template <typename TOption>
struct FUIWTPickerArgs
{
  const TArray<TSharedPtr<TOption>> *Options = nullptr;
  FText ToolTip;
  // Shown in the menu when there is nothing to pick from.
  FText EmptyText;
  TFunction<TSharedPtr<TOption>()> GetCurrent;
  TFunction<void(TSharedPtr<TOption>)> OnPicked;
  FSlateColor ButtonColor = FSlateColor::UseForeground();
  // Opens the menu itself once the button has been laid out.
  bool bOpenOnShow = false;
  // Hears the menu open and close, including a close without a pick.
  FOnIsOpenChanged OnMenuOpenChanged;
  // Fills the slot it is given instead of a fixed width.
  bool bFillWidth = false;
  // A double-click on the button closes the menu the first click opened and
  // runs this instead.
  FSimpleDelegate OnDoubleClicked;
};

namespace UIWTSearchablePicker
{
  constexpr float EditPickerWidth = 300.f;
  constexpr float EditPickerMenuHeight = 300.f;

  // A combo button whose menu is a search box over the options, with the
  // none option last as "Clear". Enter picks the first match.
  template <typename TOption>
  TSharedRef<SWidget> MakeSearchablePicker(FUIWTPickerArgs<TOption> InArgs)
  {
    using FTraits = FUIWTPickerTraits<TOption>;
    using FOptionList = TArray<TSharedPtr<TOption>>;
    using FListView = SListView<TSharedPtr<TOption>>;

    TSharedRef<FOptionList> AllOptions = MakeShared<FOptionList>();
    if (InArgs.Options)
    {
      for (const TSharedPtr<TOption> &Option : *InArgs.Options)
      {
        if (!FTraits::IsNone(Option))
        {
          AllOptions->Add(Option);
        }
      }
    }
    auto LabelOf = [](const TSharedPtr<TOption> &Item)
    {
      return FTraits::IsNone(Item) ? UIWTManagerOptions::GetNoneOptionText()
                                   : FTraits::Label(Item);
    };

    // The button handles a double-click as a second press, so the content
    // takes it first, before it bubbles up to the button.
    TSharedRef<SBorder> ButtonContent =
        SNew(SBorder)
            .BorderImage(FAppStyle::GetNoBrush())
            .Padding(0.f)
            .VAlign(VAlign_Center)
                [SNew(STextBlock)
                     .Text_Lambda([GetCurrent = InArgs.GetCurrent, LabelOf]()
                                  { return LabelOf(GetCurrent()); })
                     .ColorAndOpacity(InArgs.ButtonColor)];

    TSharedRef<SComboButton> ComboButton =
        SNew(SComboButton)
            .ContentPadding(FMargin(4.f, 0.f))
            .ToolTipText(InArgs.ToolTip)
            .OnMenuOpenChanged(InArgs.OnMenuOpenChanged)
            .ButtonContent()[ButtonContent];
    TWeakPtr<SComboButton> WeakCombo = ComboButton;

    if (InArgs.OnDoubleClicked.IsBound())
    {
      ButtonContent->SetOnMouseDoubleClick(FPointerEventHandler::CreateLambda(
          [WeakCombo, OnDoubleClicked = InArgs.OnDoubleClicked](
              const FGeometry &, const FPointerEvent &MouseEvent) -> FReply
          {
            if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
            {
              return FReply::Unhandled();
            }
            if (const TSharedPtr<SComboButton> Combo = WeakCombo.Pin())
            {
              Combo->SetIsOpen(false);
            }
            OnDoubleClicked.ExecuteIfBound();
            return FReply::Handled();
          }));
    }

    TSharedRef<TWeakPtr<SSearchBox>> LastSearchBox =
        MakeShared<TWeakPtr<SSearchBox>>();

    ComboButton->SetOnGetMenuContent(FOnGetContent::CreateLambda(
        [AllOptions, NoneOption = FTraits::MakeNone(),
         EmptyText = InArgs.EmptyText, LabelOf, OnPicked = InArgs.OnPicked,
         WeakCombo, LastSearchBox]()
        {
          TSharedRef<FOptionList> Filtered = MakeShared<FOptionList>();
          TSharedPtr<FListView> ListView;
          TSharedPtr<SSearchBox> SearchBox;

          auto ApplyFilter = [AllOptions, NoneOption,
                              Filtered](const FText &InQuery)
          {
            const FString Query = InQuery.ToString().TrimStartAndEnd();
            Filtered->Reset();
            for (const TSharedPtr<TOption> &Option : *AllOptions)
            {
              if (Query.IsEmpty() ||
                  FTraits::Label(Option).ToString().Contains(Query) ||
                  FTraits::ItemToolTip(Option).ToString().Contains(Query))
              {
                Filtered->Add(Option);
              }
            }
            Filtered->Add(NoneOption);
          };
          ApplyFilter(FText::GetEmpty());

          auto Pick = [OnPicked, WeakCombo](TSharedPtr<TOption> Picked)
          {
            if (const TSharedPtr<SComboButton> Combo = WeakCombo.Pin())
            {
              Combo->SetIsOpen(false);
            }
            OnPicked(Picked);
          };

          SAssignNew(ListView, FListView)
              .ListItemsSource(&Filtered.Get())
              .SelectionMode(ESelectionMode::Single)
              .OnGenerateRow_Lambda(
                  [LabelOf](TSharedPtr<TOption> Item,
                            const TSharedRef<STableViewBase> &OwnerTable)
                  {
                    const bool bIsNone = FTraits::IsNone(Item);
                    return SNew(STableRow<TSharedPtr<TOption>>, OwnerTable)
                        .Padding(FMargin(4.f, 2.f))
                            [SNew(STextBlock)
                                 .Text(bIsNone ? LOCTEXT("ClearOption", "Clear")
                                               : LabelOf(Item))
                                 .ColorAndOpacity(
                                     bIsNone ? FSlateColor::UseSubduedForeground()
                                             : FSlateColor::UseForeground())
                                 .ToolTipText(bIsNone
                                                  ? FText::GetEmpty()
                                                  : FTraits::ItemToolTip(Item))];
                  })
              .OnSelectionChanged_Lambda(
                  [Filtered, Pick](TSharedPtr<TOption> Picked,
                                   ESelectInfo::Type SelectInfo)
                  {
                    if (SelectInfo != ESelectInfo::Direct && Picked.IsValid())
                    {
                      Pick(Picked);
                    }
                  });
          TWeakPtr<FListView> WeakList = ListView;

          SAssignNew(SearchBox, SSearchBox)
              .HintText(LOCTEXT("PickerSearchHint", "Search..."))
              .OnTextChanged_Lambda(
                  [ApplyFilter, WeakList](const FText &InText)
                  {
                    ApplyFilter(InText);
                    if (const TSharedPtr<FListView> List = WeakList.Pin())
                    {
                      List->RequestListRefresh();
                    }
                  })
              .OnTextCommitted_Lambda(
                  [Filtered, Pick](const FText &, ETextCommit::Type CommitType)
                  {
                    if (CommitType == ETextCommit::OnEnter &&
                        Filtered->Num() > 0 &&
                        !FTraits::IsNone((*Filtered)[0]))
                    {
                      Pick((*Filtered)[0]);
                    }
                  });
          if (const TSharedPtr<SComboButton> Combo = WeakCombo.Pin())
          {
            Combo->SetMenuContentWidgetToFocus(SearchBox);
          }
          *LastSearchBox = SearchBox;

          const bool bHasOptions = AllOptions->Num() > 0;
          return SNew(SBox).WidthOverride(EditPickerWidth)
              [SNew(SVerticalBox) +
               SVerticalBox::Slot().AutoHeight().Padding(FMargin(2.f))
                   [SearchBox.ToSharedRef()] +
               SVerticalBox::Slot().AutoHeight().Padding(FMargin(6.f, 2.f))
                   [SNew(STextBlock)
                        .Text(EmptyText)
                        .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                        .Visibility(bHasOptions ? EVisibility::Collapsed
                                                : EVisibility::Visible)] +
               SVerticalBox::Slot().AutoHeight()
                   [SNew(SBox).MaxDesiredHeight(EditPickerMenuHeight)
                        [ListView.ToSharedRef()]]];
        }));

    if (InArgs.bOpenOnShow)
    {
      TSharedRef<int32> FramesLeft = MakeShared<int32>(10);
      FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
          [WeakCombo, LastSearchBox, FramesLeft](float) -> bool
          {
            const TSharedPtr<SComboButton> Combo = WeakCombo.Pin();
            if (!Combo.IsValid() || --(*FramesLeft) < 0)
            {
              return false;
            }
            if (Combo->GetCachedGeometry().GetLocalSize().X <= 0.f)
            {
              return true;
            }
            Combo->SetIsOpen(true, false);
            if (const TSharedPtr<SSearchBox> SearchBox = LastSearchBox->Pin())
            {
              FSlateApplication::Get().SetKeyboardFocus(
                  SearchBox, EFocusCause::SetDirectly);
            }
            return false;
          }));
    }

    if (InArgs.bFillWidth)
    {
      return ComboButton;
    }
    return SNew(SBox).WidthOverride(EditPickerWidth)[ComboButton];
  }
}

#undef LOCTEXT_NAMESPACE
