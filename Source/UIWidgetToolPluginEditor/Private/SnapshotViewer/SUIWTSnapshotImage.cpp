#include "SUIWTSnapshotImage.h"

#include "Rendering/DrawElements.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UIWidgetToolSnapshotImage"

namespace
{

  const FName DebugBorderBrushName(TEXT("Debug.Border"));

  bool NodeContainsPoint(const FUIWTSnapshotNode &InNode,
                         const FVector2f &InPoint)
  {
    const FVector2f Extent = TransformPoint(
        InNode.AccumulatedLayoutTransform.GetScale(), InNode.LocalSize);
    const FSlateRect Rect = FSlateRect::FromPointAndExtent(
        FVector2D(InNode.AccumulatedLayoutTransform.GetTranslation()),
        FVector2D(Extent));
    return Rect.ContainsPoint(FVector2D(InPoint));
  }

  // The deepest, topmost-painted UMG widget under the point, with its
  // ancestors. Internal Slate widgets are transparent to the search: a UMG
  // text block or panel is SelfHitTestInvisible, so Slate's own hit-test
  // would fall through it to whatever is hit-testable beneath - usually the
  // viewport - and the pick would map to nothing in the Blueprint tree.
  bool FindUMGNodesUnderPoint(const FVector2f &InPoint,
                              const TSharedRef<FUIWTSnapshotNode> &InNode,
                              TArray<TSharedRef<FUIWTSnapshotNode>> &OutNodes)
  {
    // Hidden or Collapsed, or not laid out at all (an inactive switcher
    // slot): nothing beneath was painted, whatever stale geometry the
    // descendants still carry.
    if (!InNode->bVisible || InNode->LocalSize.X <= 0.f ||
        InNode->LocalSize.Y <= 0.f)
    {
      return false;
    }

    const int32 PathLength = OutNodes.Num();
    OutNodes.Add(InNode);

    // Later children paint on top, and a child is a finer answer than its
    // parent, so descend last to first before considering this node.
    for (int32 Index = InNode->ChildNodes.Num() - 1; Index >= 0; --Index)
    {
      if (FindUMGNodesUnderPoint(InPoint, InNode->ChildNodes[Index], OutNodes))
      {
        return true;
      }
    }

    if (!InNode->UMGName.IsEmpty() && NodeContainsPoint(*InNode, InPoint))
    {
      return true;
    }

    OutNodes.SetNum(PathLength);
    return false;
  }

  // Slate's own hit-test, for snapshots with no UMG widget under the point
  // (pure Slate UI, or a file written before UMG names were recorded).
  bool FindNodesUnderPoint(const FVector2f &InPoint,
                           const TSharedRef<FUIWTSnapshotNode> &InNode,
                           TArray<TSharedRef<FUIWTSnapshotNode>> &OutNodes)
  {
    if (!InNode->bHitTestVisible && !InNode->bChildrenHitTestVisible)
    {
      return false;
    }

    if (!NodeContainsPoint(*InNode, InPoint))
    {
      return false;
    }

    const int32 PathLength = OutNodes.Num();
    OutNodes.Add(InNode);

    if (InNode->bChildrenHitTestVisible)
    {
      // Later children paint on top (SOverlay, SCanvas), so walk them last
      // to first and take the first hit, the way Slate's own hit-test does.
      // Layout transforms only: render transforms and clipping are ignored,
      // as in the engine's Widget Reflector.
      for (int32 Index = InNode->ChildNodes.Num() - 1; Index >= 0; --Index)
      {
        if (FindNodesUnderPoint(InPoint, InNode->ChildNodes[Index], OutNodes))
        {
          return true;
        }
      }
    }

    if (InNode->bHitTestVisible)
    {
      return true;
    }
    // Self-hit-test-invisible and nothing underneath hit: this node is not
    // on the path after all, so the caller can try the next sibling.
    OutNodes.SetNum(PathLength);
    return false;
  }

}

void SUIWTSnapshotCanvas::Construct(const FArguments &InArgs)
{
  OnNodesHovered = InArgs._OnNodesHovered;
  OnNodesCommitted = InArgs._OnNodesCommitted;
  SetVisibility(EVisibility::Visible);
}

const FUIWTSnapshotRoot *SUIWTSnapshotCanvas::GetRoot() const
{
  return Document.IsValid() ? Document->GetRoot(RootIndex) : nullptr;
}

FVector2f SUIWTSnapshotCanvas::GetRootTranslation() const
{
  const FUIWTSnapshotRoot *Root = GetRoot();
  if (!Root || !Root->Node.IsValid())
  {
    return FVector2f::ZeroVector;
  }
  return Root->Node->AccumulatedLayoutTransform.GetTranslation();
}

FVector2f SUIWTSnapshotCanvas::GetImageSize() const
{
  const FUIWTSnapshotRoot *Root = GetRoot();
  if (!Root)
  {
    return FVector2f::ZeroVector;
  }
  return FVector2f(static_cast<float>(Root->Dimensions.X),
                   static_cast<float>(Root->Dimensions.Y));
}

bool SUIWTSnapshotCanvas::HasImage() const
{
  const FUIWTSnapshotRoot *Root = GetRoot();
  return Root && Root->HasImage();
}

FVector2f SUIWTSnapshotCanvas::SnapshotToLocal(const FVector2f &InPoint) const
{
  return (InPoint - GetRootTranslation()) * ZoomLevel + PanOffset;
}

FVector2f SUIWTSnapshotCanvas::LocalToSnapshot(const FVector2f &InPoint) const
{
  return (InPoint - PanOffset) / FMath::Max(ZoomLevel, KINDA_SMALL_NUMBER) +
         GetRootTranslation();
}

void SUIWTSnapshotCanvas::ResetView()
{
  Highlighted.Reset();
  Selected.Reset();
  bUserZoomed = false;
  PanOffset = FVector2f::ZeroVector;
  Invalidate(EInvalidateWidgetReason::Layout);
}

void SUIWTSnapshotCanvas::SetDocument(
    const TSharedPtr<FUIWTSnapshotDocument> &InDocument)
{
  Document = InDocument;
  RootIndex = (InDocument.IsValid() && InDocument->Roots.Num() > 0) ? 0
                                                                    : INDEX_NONE;
  ResetView();
  bIsPicking = HasImage();
}

void SUIWTSnapshotCanvas::SetRootIndex(int32 InIndex)
{
  if (RootIndex != InIndex)
  {
    RootIndex = InIndex;
    ResetView();
  }
}

void SUIWTSnapshotCanvas::SetSelection(
    const TArray<TSharedRef<FUIWTSnapshotNode>> &InNodes)
{
  Selected = InNodes;
  Invalidate(EInvalidateWidgetReason::Paint);
}

void SUIWTSnapshotCanvas::SetIsPicking(bool bInPicking)
{
  if (bIsPicking == bInPicking)
  {
    return;
  }
  bIsPicking = bInPicking;
  if (!bIsPicking)
  {
    Highlighted.Reset();
  }
  Invalidate(EInvalidateWidgetReason::Paint);
}

FVector2D SUIWTSnapshotCanvas::ComputeDesiredSize(float) const
{
  return FVector2D(320.0, 240.0);
}

void SUIWTSnapshotCanvas::Tick(const FGeometry &AllottedGeometry, const double,
                               const float)
{
  CachedPanelSize = AllottedGeometry.GetLocalSize();
  RecomputeFit(CachedPanelSize);
  ClampPan(CachedPanelSize);
}

void SUIWTSnapshotCanvas::RecomputeFit(const FVector2f &InPanelSize)
{
  const FVector2f ImageSize = GetImageSize();
  if (ImageSize.X <= 0.f || ImageSize.Y <= 0.f || InPanelSize.X <= 0.f ||
      InPanelSize.Y <= 0.f)
  {
    return;
  }

  FitZoom = FMath::Min(InPanelSize.X / ImageSize.X, InPanelSize.Y / ImageSize.Y);

  if (!bUserZoomed)
  {
    ZoomLevel = FitZoom;
    const FVector2f Scaled = ImageSize * ZoomLevel;
    PanOffset = (InPanelSize - Scaled) * 0.5f;
  }
}

void SUIWTSnapshotCanvas::ClampPan(const FVector2f &InPanelSize)
{
  const FVector2f ImageSize = GetImageSize();
  if (ImageSize.X <= 0.f || ImageSize.Y <= 0.f || InPanelSize.X <= 0.f)
  {
    return;
  }

  const FVector2f Scaled = ImageSize * ZoomLevel;

  auto ClampAxis = [](float Offset, float ScaledSize, float PanelSize)
  {
    if (ScaledSize <= PanelSize)
    {
      return (PanelSize - ScaledSize) * 0.5f;
    }
    return FMath::Clamp(Offset, PanelSize - ScaledSize, 0.f);
  };

  PanOffset.X = ClampAxis(PanOffset.X, Scaled.X, InPanelSize.X);
  PanOffset.Y = ClampAxis(PanOffset.Y, Scaled.Y, InPanelSize.Y);
}

void SUIWTSnapshotCanvas::ApplyZoom(float InZoom,
                                    const FVector2f &InAnchorLocal)
{
  const float Floor = FMath::Min(MinZoom, FitZoom);
  const float NewZoom = FMath::Clamp(InZoom, Floor, MaxZoom);
  if (FMath::IsNearlyEqual(NewZoom, ZoomLevel))
  {
    return;
  }

  const FVector2f AnchorSnapshot = LocalToSnapshot(InAnchorLocal);
  ZoomLevel = NewZoom;
  bUserZoomed = true;
  PanOffset =
      InAnchorLocal - (AnchorSnapshot - GetRootTranslation()) * ZoomLevel;

  ClampPan(CachedPanelSize);
  Invalidate(EInvalidateWidgetReason::Paint);
}

void SUIWTSnapshotCanvas::ZoomBySteps(int32 InSteps)
{
  ApplyZoom(ZoomLevel * FMath::Pow(ZoomStep, static_cast<float>(InSteps)),
            CachedPanelSize * 0.5f);
}

void SUIWTSnapshotCanvas::ZoomToActualSize()
{
  ZoomLevel = 1.f;
  bUserZoomed = true;
  // Centre, then let ClampPan pull an oversized image back to the edges.
  PanOffset = (CachedPanelSize - GetImageSize()) * 0.5f;
  ClampPan(CachedPanelSize);
  Invalidate(EInvalidateWidgetReason::Paint);
}

void SUIWTSnapshotCanvas::ZoomToFit()
{
  bUserZoomed = false;
  RecomputeFit(CachedPanelSize);
  Invalidate(EInvalidateWidgetReason::Paint);
}

void SUIWTSnapshotCanvas::FocusOnNode(
    const TSharedRef<FUIWTSnapshotNode> &InNode)
{
  if (!HasImage() || CachedPanelSize.X <= 0.f)
  {
    return;
  }

  const FVector2f Extent = TransformPoint(
      InNode->AccumulatedLayoutTransform.GetScale(), InNode->LocalSize);
  if (Extent.X <= 0.f || Extent.Y <= 0.f)
  {
    return;
  }

  const float Target = FMath::Clamp(
      FMath::Min(CachedPanelSize.X / Extent.X, CachedPanelSize.Y / Extent.Y),
      FMath::Min(MinZoom, FitZoom), MaxZoom);

  ZoomLevel = Target;
  bUserZoomed = true;

  const FVector2f CenterSnapshot =
      InNode->AccumulatedLayoutTransform.GetTranslation() + Extent * 0.5f;
  PanOffset = CachedPanelSize * 0.5f -
              (CenterSnapshot - GetRootTranslation()) * ZoomLevel;

  ClampPan(CachedPanelSize);
  Invalidate(EInvalidateWidgetReason::Paint);
}

int32 SUIWTSnapshotCanvas::OnPaint(const FPaintArgs &Args,
                                   const FGeometry &AllottedGeometry,
                                   const FSlateRect &MyCullingRect,
                                   FSlateWindowElementList &OutDrawElements,
                                   int32 LayerId,
                                   const FWidgetStyle &InWidgetStyle,
                                   bool bParentEnabled) const
{
  const FUIWTSnapshotRoot *Root = GetRoot();
  if (!Root || !Root->Node.IsValid())
  {
    return LayerId;
  }

  OutDrawElements.PushClip(FSlateClippingZone(AllottedGeometry));

  if (Root->Brush.IsValid())
  {
    const FVector2f Scaled = GetImageSize() * ZoomLevel;
    FSlateDrawElement::MakeBox(
        OutDrawElements, ++LayerId,
        AllottedGeometry.ToPaintGeometry(Scaled,
                                         FSlateLayoutTransform(PanOffset)),
        Root->Brush.Get(), ESlateDrawEffect::None, FLinearColor::White);
  }

  const FSlateBrush *Border = FCoreStyle::Get().GetBrush(DebugBorderBrushName);

  auto DrawNode = [&](const TSharedRef<FUIWTSnapshotNode> &InNode,
                      const FLinearColor &InColor)
  {
    const FVector2f Extent =
        TransformPoint(InNode->AccumulatedLayoutTransform.GetScale(),
                       InNode->LocalSize) *
        ZoomLevel;
    const FVector2f Offset =
        SnapshotToLocal(InNode->AccumulatedLayoutTransform.GetTranslation());
    FSlateDrawElement::MakeBox(
        OutDrawElements, ++LayerId,
        AllottedGeometry.ToPaintGeometry(Extent, FSlateLayoutTransform(Offset)),
        Border, ESlateDrawEffect::None, InColor);
  };

  if (bIsPicking && Highlighted.Num() > 0)
  {
    const FLinearColor TopColor(1.f, 0.f, 0.f);
    const FLinearColor LeafColor(0.f, 1.f, 0.f);
    for (int32 Index = 0; Index < Highlighted.Num(); ++Index)
    {
      const float Factor =
          static_cast<float>(Index) / static_cast<float>(Highlighted.Num());
      DrawNode(Highlighted[Index], FMath::Lerp(TopColor, LeafColor, Factor));
    }
  }
  else
  {
    for (const TSharedRef<FUIWTSnapshotNode> &Node : Selected)
    {
      DrawNode(Node, Node->Tint);
    }
  }

  OutDrawElements.PopClip();
  return LayerId;
}

FReply SUIWTSnapshotCanvas::OnMouseButtonDown(const FGeometry &MyGeometry,
                                              const FPointerEvent &MouseEvent)
{
  const bool bLeft =
      MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton;
  const bool bRight =
      MouseEvent.GetEffectingButton() == EKeys::RightMouseButton;
  const bool bMiddle =
      MouseEvent.GetEffectingButton() == EKeys::MiddleMouseButton;

  if (bIsPicking && bLeft)
  {
    if (Highlighted.Num() > 0)
    {
      Selected = Highlighted;
      OnNodesCommitted.ExecuteIfBound(Selected);
    }
    return FReply::Handled();
  }

  if ((bRight || bMiddle) && HasImage())
  {
    bIsPanning = true;
    PanStartLocal = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
    PanStartOffset = PanOffset;
    return FReply::Handled().CaptureMouse(AsShared()).SetUserFocus(AsShared(), EFocusCause::Mouse);
  }

  return FReply::Unhandled();
}

FReply SUIWTSnapshotCanvas::OnMouseButtonUp(const FGeometry &,
                                            const FPointerEvent &)
{
  if (bIsPanning)
  {
    bIsPanning = false;
    return FReply::Handled().ReleaseMouseCapture();
  }
  return FReply::Unhandled();
}

FReply SUIWTSnapshotCanvas::OnMouseMove(const FGeometry &MyGeometry,
                                        const FPointerEvent &MouseEvent)
{
  const FVector2f LocalPos =
      MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());

  if (bIsPanning)
  {
    PanOffset = PanStartOffset + (LocalPos - PanStartLocal);
    bUserZoomed = true;
    ClampPan(MyGeometry.GetLocalSize());
    Invalidate(EInvalidateWidgetReason::Paint);
    return FReply::Handled();
  }

  if (!bIsPicking)
  {
    return FReply::Unhandled();
  }

  const FUIWTSnapshotRoot *Root = GetRoot();
  if (!Root || !Root->Node.IsValid())
  {
    return FReply::Unhandled();
  }

  Highlighted.Reset();
  const FVector2f Point = LocalToSnapshot(LocalPos);
  if (!FindUMGNodesUnderPoint(Point, Root->Node.ToSharedRef(), Highlighted))
  {
    FindNodesUnderPoint(Point, Root->Node.ToSharedRef(), Highlighted);
  }

  if (Highlighted.Num() > 0)
  {
    OnNodesHovered.ExecuteIfBound(Highlighted);
  }

  Invalidate(EInvalidateWidgetReason::Paint);
  return FReply::Handled();
}

FReply SUIWTSnapshotCanvas::OnMouseWheel(const FGeometry &MyGeometry,
                                         const FPointerEvent &MouseEvent)
{
  if (!HasImage())
  {
    return FReply::Unhandled();
  }

  const float Delta = MouseEvent.GetWheelDelta();
  if (FMath::IsNearlyZero(Delta))
  {
    return FReply::Unhandled();
  }

  const FVector2f LocalPos =
      MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
  ApplyZoom(Delta > 0.f ? ZoomLevel * ZoomStep : ZoomLevel / ZoomStep,
            LocalPos);
  return FReply::Handled();
}

FReply SUIWTSnapshotCanvas::OnKeyDown(const FGeometry &,
                                      const FKeyEvent &InKeyEvent)
{
  if (bIsPicking && InKeyEvent.GetKey() == EKeys::Escape)
  {
    SetIsPicking(false);
    OnNodesCommitted.ExecuteIfBound(Selected);
    return FReply::Handled();
  }
  return FReply::Unhandled();
}

void SUIWTSnapshotCanvas::OnMouseLeave(const FPointerEvent &)
{
  if (Highlighted.Num() > 0)
  {
    Highlighted.Reset();
    Invalidate(EInvalidateWidgetReason::Paint);
  }
}

FCursorReply SUIWTSnapshotCanvas::OnCursorQuery(const FGeometry &,
                                                const FPointerEvent &) const
{
  if (bIsPanning)
  {
    return FCursorReply::Cursor(EMouseCursor::GrabHandClosed);
  }
  if (bIsPicking)
  {
    return FCursorReply::Cursor(EMouseCursor::Crosshairs);
  }
  return FCursorReply::Unhandled();
}

void SUIWTSnapshotImage::Construct(const FArguments &InArgs)
{
  OnNodesCommitted = InArgs._OnNodesCommitted;

  ChildSlot
      [SNew(SVerticalBox) +
       SVerticalBox::Slot().AutoHeight()
           [SNew(SBox).HeightOverride(28.f).Padding(FMargin(2.f))
                [SNew(SHorizontalBox) +
                 SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 2.f, 0.f))[SNew(SButton).ButtonStyle(FAppStyle::Get(), "SimpleButton").IsEnabled(this, &SUIWTSnapshotImage::HasImage).ToolTipText(LOCTEXT("ZoomOutTip", "Zoom out.")).OnClicked(this, &SUIWTSnapshotImage::OnZoomOutClicked)[SNew(SImage).Image(FAppStyle::Get().GetBrush("Icons.Minus")).ColorAndOpacity(FSlateColor::UseForeground())]] +
                 SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 6.f, 0.f))[SNew(SButton).ButtonStyle(FAppStyle::Get(), "SimpleButton").IsEnabled(this, &SUIWTSnapshotImage::HasImage).ToolTipText(LOCTEXT("ZoomInTip", "Zoom in.")).OnClicked(this, &SUIWTSnapshotImage::OnZoomInClicked)[SNew(SImage).Image(FAppStyle::Get().GetBrush("Icons.Plus")).ColorAndOpacity(FSlateColor::UseForeground())]] +
                 SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 2.f, 0.f))[SNew(SButton).IsEnabled(this, &SUIWTSnapshotImage::HasImage).Text(LOCTEXT("ResetBtn", "Reset")).ToolTipText(LOCTEXT("ResetTip", "Back to 100%: one image pixel per panel pixel.")).OnClicked(this, &SUIWTSnapshotImage::OnResetClicked)] +
                 SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 2.f, 0.f))[SNew(SButton).IsEnabled(this, &SUIWTSnapshotImage::HasImage).Text(LOCTEXT("FitBtn", "Fit")).ToolTipText(LOCTEXT("FitTip", "Fit the whole snapshot in the panel, and keep "
                                                                                                                                                                                                                                         "fitting it when the panel is resized."))
                                                                                                                   .OnClicked(this, &SUIWTSnapshotImage::OnFitClicked)] +
                 SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Fill).Padding(FMargin(4.f, 2.f))[SNew(SSeparator).Orientation(Orient_Vertical)] +
                 SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 2.f, 0.f))[SNew(SCheckBox).Style(FAppStyle::Get(), "ToggleButtonCheckbox").Padding(FMargin(8.f, 2.f)).IsEnabled(this, &SUIWTSnapshotImage::HasImage).ToolTipText(this, &SUIWTSnapshotImage::GetPickToolTip).IsChecked(this, &SUIWTSnapshotImage::GetPickCheckState).OnCheckStateChanged(this, &SUIWTSnapshotImage::OnPickToggled)[SNew(STextBlock).Text(LOCTEXT("PickBtn", "Pick Snapshot Widget"))]] +
                 SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).HAlign(HAlign_Right)[SNew(STextBlock).Text(this, &SUIWTSnapshotImage::GetZoomText).ColorAndOpacity(FSlateColor::UseSubduedForeground())]]] +
       SVerticalBox::Slot().FillHeight(1.f)
           [SAssignNew(Canvas, SUIWTSnapshotCanvas)
                .OnNodesHovered(InArgs._OnNodesHovered)
                .OnNodesCommitted(this,
                                  &SUIWTSnapshotImage::HandleNodesCommitted)]];
}

// A committed pick ends picking; the toggle turns it back on.
void SUIWTSnapshotImage::HandleNodesCommitted(
    const TArray<TSharedRef<FUIWTSnapshotNode>> &InNodes)
{
  Canvas->SetIsPicking(false);
  OnNodesCommitted.ExecuteIfBound(InNodes);
}

FReply SUIWTSnapshotImage::OnZoomInClicked()
{
  Canvas->ZoomBySteps(1);
  return FReply::Handled();
}

FReply SUIWTSnapshotImage::OnZoomOutClicked()
{
  Canvas->ZoomBySteps(-1);
  return FReply::Handled();
}

FReply SUIWTSnapshotImage::OnResetClicked()
{
  Canvas->ZoomToActualSize();
  return FReply::Handled();
}

FReply SUIWTSnapshotImage::OnFitClicked()
{
  Canvas->ZoomToFit();
  return FReply::Handled();
}

ECheckBoxState SUIWTSnapshotImage::GetPickCheckState() const
{
  return Canvas->IsPicking() ? ECheckBoxState::Checked
                             : ECheckBoxState::Unchecked;
}

void SUIWTSnapshotImage::OnPickToggled(ECheckBoxState InState)
{
  Canvas->SetIsPicking(InState == ECheckBoxState::Checked);
}

FText SUIWTSnapshotImage::GetPickToolTip() const
{
  if (!HasImage())
  {
    return LOCTEXT("PickNoImage",
                   "Load a snapshot with an image to pick from it.");
  }
  return LOCTEXT("PickTip",
                 "On after a load. Hover the snapshot to highlight the UMG "
                 "widget under the cursor and its chain; left-click to keep "
                 "it, Escape to cancel. Right-drag pans either way.");
}

FText SUIWTSnapshotImage::GetZoomText() const
{
  if (!HasImage())
  {
    return FText::GetEmpty();
  }
  const int32 Percent = FMath::RoundToInt(Canvas->GetZoomLevel() * 100.f);
  return FText::Format(LOCTEXT("ZoomPercent", "{0}%"), FText::AsNumber(Percent));
}

#undef LOCTEXT_NAMESPACE
