#include "UIWTDesignTree.h"

// Row and column hints (import-image.md, section 2): a reader that can't
// measure layout itself (the image reader) marks a frame or group
// layout:row or layout:column, and this turns the mark into a layout block
// measured from the children's boxes, before the converter runs. Figma and
// Photoshop trees don't carry these hints, so they convert as before.
namespace
{
  using namespace UIWTDesignTree;

  const TCHAR *const RowHint = TEXT("layout:row");
  const TCHAR *const ColumnHint = TEXT("layout:column");
  // Pixels of slack before a gap or an alignment counts as uneven.
  constexpr double Tolerance = 2.0;

  bool HasLayoutHint(const FNode &InNode)
  {
    return InNode.Hints.Contains(RowHint) || InNode.Hints.Contains(ColumnHint);
  }

  bool AnyLayoutHint(const FNode &InNode)
  {
    if (HasLayoutHint(InNode))
    {
      return true;
    }
    for (const FNode &Child : InNode.Children)
    {
      if (AnyLayoutHint(Child))
      {
        return true;
      }
    }
    return false;
  }

  void Note(TArray<FNote> &OutNotes, const FNode &InNode, const TCHAR *InCategory,
            const FString &InDetail)
  {
    OutNotes.Add({InNode.Id, InCategory, InDetail});
  }

  bool Measure(FNode &InOutNode, TArray<FNote> &OutNotes)
  {
    if (!HasLayoutHint(InOutNode))
    {
      return false;
    }
    if ((InOutNode.Kind != EKind::Frame && InOutNode.Kind != EKind::Group) ||
        InOutNode.Layout.IsValid() || InOutNode.Children.IsEmpty())
    {
      Note(OutNotes, InOutNode, TEXT("hintIgnored"),
           TEXT("a row or column hint needs a frame or group with children and no layout of its "
                "own"));
      return false;
    }
    const bool bRow = InOutNode.Hints.Contains(RowHint);
    auto Start = [bRow](const FNode &InChild) { return bRow ? InChild.Box.X : InChild.Box.Y; };
    auto Length = [bRow](const FNode &InChild) { return bRow ? InChild.Box.W : InChild.Box.H; };
    auto CrossStart = [bRow](const FNode &InChild) { return bRow ? InChild.Box.Y : InChild.Box.X; };
    auto CrossLength = [bRow](const FNode &InChild) { return bRow ? InChild.Box.H : InChild.Box.W; };

    TArray<FNode> Children = InOutNode.Children;
    Children.StableSort([&Start](const FNode &A, const FNode &B) { return Start(A) < Start(B); });
    for (int32 Index = 1; Index < Children.Num(); ++Index)
    {
      if (Start(Children[Index]) < Start(Children[Index - 1]) + Length(Children[Index - 1]) - 1.0)
      {
        Note(OutNotes, InOutNode, TEXT("hintIgnored"),
             FString::Printf(TEXT("the children overlap along the %s, so it isn't one; they stay "
                                  "where they are"),
                             bRow ? TEXT("row") : TEXT("column")));
        return false;
      }
    }

    // The gap between neighbours, evened out.
    double GapSum = 0.0;
    double GapMin = TNumericLimits<double>::Max();
    double GapMax = TNumericLimits<double>::Lowest();
    for (int32 Index = 1; Index < Children.Num(); ++Index)
    {
      const double Gap = Start(Children[Index]) - (Start(Children[Index - 1]) + Length(Children[Index - 1]));
      GapSum += Gap;
      GapMin = FMath::Min(GapMin, Gap);
      GapMax = FMath::Max(GapMax, Gap);
    }
    const double Spacing =
        Children.Num() > 1 ? FMath::RoundToDouble(GapSum / (Children.Num() - 1)) : 0.0;
    if (Children.Num() > 1 && GapMax - GapMin > Tolerance)
    {
      Note(OutNotes, InOutNode, TEXT("layoutApprox"),
           FString::Printf(TEXT("gaps from %g to %g px; every gap is %g"), GapMin, GapMax, Spacing));
    }

    // Across: aligned at the start, the centre or the end.
    double CrossMin = TNumericLimits<double>::Max();
    double CrossMax = TNumericLimits<double>::Lowest();
    double StartSpread[2] = {TNumericLimits<double>::Max(), TNumericLimits<double>::Lowest()};
    double CenterSpread[2] = {TNumericLimits<double>::Max(), TNumericLimits<double>::Lowest()};
    double EndSpread[2] = {TNumericLimits<double>::Max(), TNumericLimits<double>::Lowest()};
    for (const FNode &Child : Children)
    {
      const double Begin = CrossStart(Child);
      const double End = Begin + CrossLength(Child);
      CrossMin = FMath::Min(CrossMin, Begin);
      CrossMax = FMath::Max(CrossMax, End);
      auto Spread = [](double (&InOut)[2], double InValue)
      {
        InOut[0] = FMath::Min(InOut[0], InValue);
        InOut[1] = FMath::Max(InOut[1], InValue);
      };
      Spread(StartSpread, Begin);
      Spread(CenterSpread, (Begin + End) * 0.5);
      Spread(EndSpread, End);
    }
    EAlignCross Cross = EAlignCross::Start;
    if (StartSpread[1] - StartSpread[0] <= Tolerance)
    {
      Cross = EAlignCross::Start;
    }
    else if (CenterSpread[1] - CenterSpread[0] <= Tolerance)
    {
      Cross = EAlignCross::Center;
    }
    else if (EndSpread[1] - EndSpread[0] <= Tolerance)
    {
      Cross = EAlignCross::End;
    }
    else
    {
      Note(OutNotes, InOutNode, TEXT("layoutApprox"),
           FString::Printf(TEXT("the children aren't aligned across the %s; they're aligned to "
                                "its start"),
                           bRow ? TEXT("row") : TEXT("column")));
    }

    const double MainLength = bRow ? InOutNode.Box.W : InOutNode.Box.H;
    const double CrossTotal = bRow ? InOutNode.Box.H : InOutNode.Box.W;
    const double MainBefore = Start(Children[0]);
    const double MainAfter = MainLength - (Start(Children.Last()) + Length(Children.Last()));
    const double CrossBefore = CrossMin;
    const double CrossAfter = CrossTotal - CrossMax;
    if (MainBefore < 0.0 || MainAfter < 0.0 || CrossBefore < 0.0 || CrossAfter < 0.0)
    {
      Note(OutNotes, InOutNode, TEXT("layoutApprox"),
           TEXT("children reach outside the frame; its padding is 0 on those sides"));
    }

    TSharedRef<FLayout> Layout = MakeShared<FLayout>();
    Layout->Mode = bRow ? ELayoutMode::Horizontal : ELayoutMode::Vertical;
    Layout->Spacing = FMath::Max(Spacing, 0.0);
    Layout->AlignMain = EAlignMain::Start;
    Layout->AlignCross = Cross;
    const double Before = FMath::Max(MainBefore, 0.0);
    const double After = FMath::Max(MainAfter, 0.0);
    const double Top = FMath::Max(CrossBefore, 0.0);
    const double Bottom = FMath::Max(CrossAfter, 0.0);
    Layout->Padding = bRow ? FEdges{Before, Top, After, Bottom} : FEdges{Top, Before, Bottom, After};
    InOutNode.Layout = Layout;
    InOutNode.Children = MoveTemp(Children);
    InOutNode.Kind = EKind::Frame;
    return true;
  }

  bool ApplyTo(FNode &InOutNode, TArray<FNote> &OutNotes)
  {
    bool bChanged = Measure(InOutNode, OutNotes);
    for (FNode &Child : InOutNode.Children)
    {
      bChanged |= ApplyTo(Child, OutNotes);
    }
    return bChanged;
  }
}

bool UIWTDesignTree::HasLayoutHints(const FDocument &InDocument)
{
  if (AnyLayoutHint(InDocument.Root))
  {
    return true;
  }
  for (const TPair<FString, FComponentDef> &Component : InDocument.Components)
  {
    if (AnyLayoutHint(Component.Value.Root))
    {
      return true;
    }
  }
  return false;
}

bool UIWTDesignTree::ApplyLayoutHints(FDocument &InOutDocument)
{
  bool bChanged = ApplyTo(InOutDocument.Root, InOutDocument.Notes);
  for (TPair<FString, FComponentDef> &Component : InOutDocument.Components)
  {
    bChanged |= ApplyTo(Component.Value.Root, InOutDocument.Notes);
  }
  return bChanged;
}
