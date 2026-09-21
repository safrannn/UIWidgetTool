#pragma once

#include "CoreMinimal.h"
#include "Layout/Geometry.h"
#include "Rendering/SlateLayoutTransform.h"
#include "Rendering/SlateRenderTransform.h"
#include "Styling/SlateColor.h"

// The .widgetsnapshot file format, shared by the writer (UIWTWidgetSnapshot)
// and the reader (UIWTSnapshotReader). It mirrors what the engine's Widget
// Reflector writes, so a file produced here still opens in a stock reflector.

// One widget in a captured hierarchy.
struct FUIWTSnapshotNode
{
  FSlateLayoutTransform AccumulatedLayoutTransform;
  FSlateRenderTransform AccumulatedRenderTransform;
  FVector2f LocalSize = FVector2f::ZeroVector;

  bool bHitTestVisible = false;
  bool bChildrenHitTestVisible = false;
  FLinearColor Tint = FLinearColor::White;

  FText WidgetType;
  FText WidgetTypeAndShortName;
  FText WidgetVisibilityText;
  FText WidgetClippingText;
  FText WidgetReadableLocation;

  bool bVisible = false;
  bool bVisibleInherited = false;
  bool bFocusable = false;
  bool bNeedsTick = false;
  bool bIsVolatile = false;
  bool bIsVolatileIndirectly = false;
  bool bHasActiveTimers = false;
  bool bIsInvalidationRoot = false;
  bool bEnabled = false;

  bool bSupportsInvalidation = false;
  bool bSupportsInvalidationRecursive = false;

  int32 LayerId = -1;
  int32 LayerIdOut = -1;
  int32 LineNumber = 0;
  int32 AttributeCount = 0;
  int32 CollapsedAttributeCount = 0;

  FString File;
  FVector2D DesiredSize = FVector2D::ZeroVector;
  FSlateColor ForegroundColor = FSlateColor::UseForeground();

  uint64 WidgetAddress = 0;
  FString AssetPathString;
  // The UMG widget name this SWidget was built for (FReflectionMetaData::Name),
  // empty for internal Slate widgets. Lets a pick on the image map back to a
  // widget in the design-time tree. Added in 2.3; older files read as empty.
  FString UMGName;

  TArray<TSharedRef<FUIWTSnapshotNode>> ChildNodes;
};

namespace UIWTSnapshotFormat
{
  // The reflector schema version this plugin writes, and the engine release
  // the capture code was ported from.
  constexpr double JsonVersion = 2.3;
  inline const TCHAR *const PortedFromEngine = TEXT("5.8");

  // Deeper hierarchies are truncated on read rather than recursed into.
  constexpr int32 MaxNodeDepth = 512;

  // Keys for the structural and composite values. The flat per-node scalars
  // are listed in the field tables below instead, so both sides walk them
  // from one definition.
  namespace Key
  {
    inline const TCHAR *const Version = TEXT("Version");
    inline const TCHAR *const PortedFromEngine = TEXT("UIWTPortedFromEngine");
    inline const TCHAR *const Windows = TEXT("Windows");
    inline const TCHAR *const NavigationData = TEXT("NavigationData");
    inline const TCHAR *const Textures = TEXT("Textures");
    inline const TCHAR *const ChildNodes = TEXT("ChildNodes");

    inline const TCHAR *const AccumulatedLayoutTransform =
        TEXT("AccumulatedLayoutTransform");
    inline const TCHAR *const AccumulatedRenderTransform =
        TEXT("AccumulatedRenderTransform");
    inline const TCHAR *const Scale = TEXT("Scale");
    inline const TCHAR *const Matrix = TEXT("Matrix");
    inline const TCHAR *const Translation = TEXT("Translation");
    inline const TCHAR *const LocalSize = TEXT("LocalSize");
    inline const TCHAR *const WidgetDesiredSize = TEXT("WidgetDesiredSize");

    inline const TCHAR *const HitTestInfo = TEXT("HitTestInfo");
    inline const TCHAR *const IsHitTestVisible = TEXT("IsHitTestVisible");
    inline const TCHAR *const AreChildrenHitTestVisible =
        TEXT("AreChildrenHitTestVisible");

    inline const TCHAR *const Tint = TEXT("Tint");
    inline const TCHAR *const WidgetForegroundColor =
        TEXT("WidgetForegroundColor");
    inline const TCHAR *const IsColorSpecified = TEXT("IsColorSpecified");
    inline const TCHAR *const Color = TEXT("Color");

    inline const TCHAR *const WidgetAddress = TEXT("WidgetAddress");

    inline const TCHAR *const Dimensions = TEXT("Dimensions");
    inline const TCHAR *const IsCompressed = TEXT("IsCompressed");
    inline const TCHAR *const UncompressedSize = TEXT("UncompressedSize");
    inline const TCHAR *const TextureData = TEXT("TextureData");
  }
}

// Flat per-node fields: one row is (JSON key, FUIWTSnapshotNode member).
// Expand with a two-argument macro to emit one write or one read per row.

#define UIWT_SNAPSHOT_BOOL_FIELDS(X)                                          \
  X(WidgetVisible, bVisible)                                                  \
  X(WidgetVisibleInherited, bVisibleInherited)                                \
  X(WidgetFocusable, bFocusable)                                              \
  X(WidgetNeedsTick, bNeedsTick)                                              \
  X(WidgetIsVolatile, bIsVolatile)                                            \
  X(WidgetIsVolatileIndirectly, bIsVolatileIndirectly)                        \
  X(WidgetHasActiveTimers, bHasActiveTimers)                                  \
  X(WidgetIsInvalidationRoot, bIsInvalidationRoot)                            \
  X(WidgetEnabled, bEnabled)                                                  \
  X(SupportsInvalidation, bSupportsInvalidation)                              \
  X(SupportsInvalidationRecursive, bSupportsInvalidationRecursive)

#define UIWT_SNAPSHOT_INT_FIELDS(X)                                           \
  X(WidgetLayerId, LayerId)                                                   \
  X(WidgetLayerIdOut, LayerIdOut)                                             \
  X(WidgetLineNumber, LineNumber)                                             \
  X(WidgetAttributeCount, AttributeCount)                                     \
  X(WidgetCollapsedAttributeCount, CollapsedAttributeCount)

#define UIWT_SNAPSHOT_STRING_FIELDS(X)                                        \
  X(WidgetFile, File)                                                         \
  X(WidgetAssetPath, AssetPathString)                                         \
  X(WidgetUMGName, UMGName)

#define UIWT_SNAPSHOT_TEXT_FIELDS(X)                                          \
  X(WidgetType, WidgetType)                                                   \
  X(WidgetTypeAndShortName, WidgetTypeAndShortName)                           \
  X(WidgetVisibilityText, WidgetVisibilityText)                               \
  X(WidgetClippingText, WidgetClippingText)                                   \
  X(WidgetReadableLocation, WidgetReadableLocation)
