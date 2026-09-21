#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateTypes.h"
#include "UIWTSnapshotReader.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

DECLARE_DELEGATE_OneParam(FUIWTOnNodesPicked,
                          const TArray<TSharedRef<FUIWTSnapshotNode>> &);

class SUIWTSnapshotCanvas : public SCompoundWidget
{
public:
  SLATE_BEGIN_ARGS(SUIWTSnapshotCanvas) {}
  SLATE_EVENT(FUIWTOnNodesPicked, OnNodesHovered)
  SLATE_EVENT(FUIWTOnNodesPicked, OnNodesCommitted)
  SLATE_END_ARGS()

  void Construct(const FArguments &InArgs);

  void SetDocument(const TSharedPtr<FUIWTSnapshotDocument> &InDocument);
  void SetRootIndex(int32 InIndex);
  void SetSelection(const TArray<TSharedRef<FUIWTSnapshotNode>> &InNodes);

  void SetIsPicking(bool bInPicking);
  bool IsPicking() const { return bIsPicking; }

  bool HasImage() const;

  // Zooms InSteps notches about the panel centre; negative zooms out.
  void ZoomBySteps(int32 InSteps);
  void ZoomToActualSize();
  void ZoomToFit();
  float GetZoomLevel() const { return ZoomLevel; }

  void FocusOnNode(const TSharedRef<FUIWTSnapshotNode> &InNode);

  // Reads the panel size and keeps the fit current. A childless widget with
  // its own OnPaint is never asked to arrange children, so this cannot live
  // in OnArrangeChildren; Slate ticks every painted widget with its allotted
  // geometry right before painting it.
  virtual void Tick(const FGeometry &AllottedGeometry, const double InCurrentTime,
                    const float InDeltaTime) override;
  virtual FVector2D ComputeDesiredSize(float) const override;
  virtual int32 OnPaint(const FPaintArgs &Args, const FGeometry &AllottedGeometry,
                        const FSlateRect &MyCullingRect,
                        FSlateWindowElementList &OutDrawElements, int32 LayerId,
                        const FWidgetStyle &InWidgetStyle,
                        bool bParentEnabled) const override;
  virtual FReply OnMouseButtonDown(const FGeometry &MyGeometry,
                                   const FPointerEvent &MouseEvent) override;
  virtual FReply OnMouseButtonUp(const FGeometry &MyGeometry,
                                 const FPointerEvent &MouseEvent) override;
  virtual FReply OnMouseMove(const FGeometry &MyGeometry,
                             const FPointerEvent &MouseEvent) override;
  virtual FReply OnMouseWheel(const FGeometry &MyGeometry,
                              const FPointerEvent &MouseEvent) override;
  virtual FReply OnKeyDown(const FGeometry &MyGeometry,
                           const FKeyEvent &InKeyEvent) override;
  virtual void OnMouseLeave(const FPointerEvent &MouseEvent) override;
  virtual FCursorReply OnCursorQuery(const FGeometry &MyGeometry,
                                     const FPointerEvent &CursorEvent) const override;
  virtual bool SupportsKeyboardFocus() const override { return true; }

private:
  static constexpr float MinZoom = 0.05f;
  static constexpr float MaxZoom = 4.f;
  static constexpr float ZoomStep = 1.25f;

  const FUIWTSnapshotRoot *GetRoot() const;
  FVector2f GetRootTranslation() const;
  FVector2f GetImageSize() const;

  FVector2f SnapshotToLocal(const FVector2f &InPoint) const;
  FVector2f LocalToSnapshot(const FVector2f &InPoint) const;

  // Forgets highlight, selection and any user zoom; the next arrange re-fits.
  void ResetView();
  void RecomputeFit(const FVector2f &InPanelSize);
  void ClampPan(const FVector2f &InPanelSize);
  void ApplyZoom(float InZoom, const FVector2f &InAnchorLocal);

  TSharedPtr<FUIWTSnapshotDocument> Document;
  int32 RootIndex = INDEX_NONE;

  float ZoomLevel = 1.f;
  float FitZoom = 1.f;
  bool bUserZoomed = false;
  FVector2f PanOffset = FVector2f::ZeroVector;
  FVector2f CachedPanelSize = FVector2f::ZeroVector;

  bool bIsPicking = false;
  bool bIsPanning = false;
  FVector2f PanStartLocal = FVector2f::ZeroVector;
  FVector2f PanStartOffset = FVector2f::ZeroVector;

  TArray<TSharedRef<FUIWTSnapshotNode>> Highlighted;
  TArray<TSharedRef<FUIWTSnapshotNode>> Selected;

  FUIWTOnNodesPicked OnNodesHovered;
  FUIWTOnNodesPicked OnNodesCommitted;
};

// The canvas plus its zoom / pick toolbar.
class SUIWTSnapshotImage : public SCompoundWidget
{
public:
  SLATE_BEGIN_ARGS(SUIWTSnapshotImage) {}
  SLATE_EVENT(FUIWTOnNodesPicked, OnNodesHovered)
  SLATE_EVENT(FUIWTOnNodesPicked, OnNodesCommitted)
  SLATE_END_ARGS()

  void Construct(const FArguments &InArgs);

  // The canvas the toolbar drives; the owner feeds it the document and
  // selection directly.
  TSharedRef<SUIWTSnapshotCanvas> GetCanvas() const { return Canvas.ToSharedRef(); }

private:
  bool HasImage() const { return Canvas->HasImage(); }
  FReply OnZoomInClicked();
  FReply OnZoomOutClicked();
  FReply OnResetClicked();
  FReply OnFitClicked();
  FText GetZoomText() const;
  ECheckBoxState GetPickCheckState() const;
  void OnPickToggled(ECheckBoxState InState);
  FText GetPickToolTip() const;
  void HandleNodesCommitted(const TArray<TSharedRef<FUIWTSnapshotNode>> &InNodes);

  TSharedPtr<SUIWTSnapshotCanvas> Canvas;
  FUIWTOnNodesPicked OnNodesCommitted;
};
