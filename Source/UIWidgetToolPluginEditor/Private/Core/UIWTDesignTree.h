#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// The design tree: the source-neutral form the Figma and Photoshop readers
// write, and the converter that turns it into a draft UIWTWidgetSpec. The
// schema and every conversion rule are specified in import-tree.md; this
// header only mirrors it. Nothing here knows about Figma or Photoshop, and
// the converter loads no assets and reads no settings, so it can be tested on
// fixture files alone.
namespace UIWTDesignTree
{
  // ---------------------------------------------------------------------
  // Schema (v1)

  enum class EKind : uint8
  {
    Frame,
    Group,
    Instance,
    Text,
    Shape,
    Image
  };

  // One axis of `constraints`: left/top, right/bottom, leftRight/topBottom,
  // center, scale.
  enum class EConstraint : uint8
  {
    Min,
    Max,
    Stretch,
    Center,
    Scale
  };

  enum class ESizing : uint8
  {
    Fixed,
    Hug,
    Fill
  };

  enum class ELayoutMode : uint8
  {
    Horizontal,
    Vertical
  };

  enum class EAlignMain : uint8
  {
    Start,
    Center,
    End,
    SpaceBetween
  };

  enum class EAlignCross : uint8
  {
    Start,
    Center,
    End,
    Stretch
  };

  enum class EStrokeAlign : uint8
  {
    Inside,
    Center,
    Outside
  };

  enum class ETextAlign : uint8
  {
    Left,
    Center,
    Right,
    Justify
  };

  enum class ETextVAlign : uint8
  {
    Top,
    Center,
    Bottom
  };

  enum class ETextSizing : uint8
  {
    Auto,
    FixedWidth,
    Fixed
  };

  enum class EImageMode : uint8
  {
    Stretch,
    Fit,
    Tile
  };

  enum class EImageOrigin : uint8
  {
    Fill,
    Rendered
  };

  // Absent means: screen when the root box is the reference size, else widget.
  enum class ERootMode : uint8
  {
    Auto,
    Screen,
    Widget
  };

  struct FRect
  {
    double X = 0.0;
    double Y = 0.0;
    double W = 0.0;
    double H = 0.0;
  };

  struct FEdges
  {
    double L = 0.0;
    double T = 0.0;
    double R = 0.0;
    double B = 0.0;

    bool IsZero() const { return L == 0.0 && T == 0.0 && R == 0.0 && B == 0.0; }
  };

  struct FLayout
  {
    ELayoutMode Mode = ELayoutMode::Horizontal;
    bool bWrap = false;
    double Spacing = 0.0;
    double CrossSpacing = 0.0;
    FEdges Padding;
    EAlignMain AlignMain = EAlignMain::Start;
    EAlignCross AlignCross = EAlignCross::Start;
  };

  struct FSizeLimits
  {
    TOptional<double> MinW;
    TOptional<double> MaxW;
    TOptional<double> MinH;
    TOptional<double> MaxH;
  };

  // `image` on an image node, or `fill.image` on a frame.
  struct FImageRef
  {
    // Relative to design.json. Also the texture's identity across imports.
    FString Path;
    // Texture pixels per 1x pixel.
    double Scale = 1.0;
    EImageOrigin Origin = EImageOrigin::Fill;
    EImageMode Mode = EImageMode::Stretch;
    // Texture size in pixels, when the reader knows it. `fit` and `tile`
    // need it; the converter falls back to the box and reports otherwise.
    TOptional<FVector2D> Size;
  };

  struct FFill
  {
    TOptional<FColor> Color;
    TSharedPtr<const FImageRef> Image;
  };

  struct FStroke
  {
    FColor Color = FColor::Black;
    FEdges Weights;
    EStrokeAlign Align = EStrokeAlign::Inside;
  };

  struct FEffect
  {
    FString Type;
    bool bBaked = false;
    // Shadows only: colour (set when the reader knows it), offset (x right,
    // y down), blur radius and spread, in design pixels.
    TOptional<FColor> Color;
    FVector2D Offset = FVector2D::ZeroVector;
    double Radius = 0.0;
    double Spread = 0.0;
  };

  struct FFontRef
  {
    FString Family;
    FString Style;
    FString PostScript;
  };

  struct FTextRun
  {
    int32 Start = 0;
    int32 Len = 0;
    FFontRef Font;
    // Design pixels.
    double Size = 0.0;
    FColor Color = FColor::Black;
    // 1/1000 em.
    double Tracking = 0.0;
    // Only one of these, or neither for the font's natural line height.
    TOptional<double> LineHeightPx;
    TOptional<double> LineHeightPercent;
  };

  struct FTextBlock
  {
    FString Content;
    TArray<FTextRun> Runs;
    ETextAlign Align = ETextAlign::Left;
    ETextVAlign VAlign = ETextVAlign::Top;
    ETextSizing Sizing = ETextSizing::Auto;
  };

  struct FComponentRef
  {
    FString Key;
    FString Name;
    bool bHasOverrides = false;
  };

  // A design node. The common fields are inline; the blocks only some nodes
  // have are shared, immutable allocations, so copying a node (for a
  // component tree) copies the structure and shares the blocks.
  struct FNode
  {
    FString Id;
    FString Name;
    EKind Kind = EKind::Frame;
    FRect Box;
    double Rotation = 0.0;
    double Opacity = 1.0;
    bool bVisible = true;
    FString BlendMode = TEXT("normal");
    bool bClip = false;
    bool bHasConstraints = false;
    EConstraint ConstraintH = EConstraint::Min;
    EConstraint ConstraintV = EConstraint::Min;
    ESizing SizingH = ESizing::Fixed;
    ESizing SizingV = ESizing::Fixed;
    bool bAbsolute = false;
    TOptional<FSizeLimits> SizeLimits;
    // [tl, tr, br, bl]
    TOptional<FVector4> Radii;
    TArray<FEffect> Effects;
    TArray<FString> Hints;
    TSharedPtr<const FLayout> Layout;
    TSharedPtr<const FFill> Fill;
    TSharedPtr<const FStroke> Stroke;
    TSharedPtr<const FTextBlock> Text;
    TSharedPtr<const FImageRef> Image;
    TSharedPtr<const FComponentRef> Component;
    TArray<FNode> Children;
  };

  struct FNote
  {
    FString Node;
    FString Category;
    FString Detail;
  };

  // An entry of the document's `components` map: the main component a child
  // WBP is built from.
  struct FComponentDef
  {
    FString Name;
    FString FileKey;
    FString NodeId;
    FString Version;
    // Decides whether the child WBP needs updating (import-tree.md →
    // Components).
    FString Hash;
    FNode Root;
  };

  struct FDocument
  {
    int32 Version = 1;
    // "figma" | "psd"
    FString Source;
    // Kept as written: its fields depend on the source.
    TSharedPtr<FJsonObject> SourceRef;
    FVector2D ReferenceSize = FVector2D(1920.0, 1080.0);
    ERootMode RootMode = ERootMode::Auto;
    TArray<FNote> Notes;
    TMap<FString, FComponentDef> Components;
    FNode Root;
  };

  // Reads design.json. Returns false with OutErrors set when the document
  // can't be used; unknown fields are ignored (import-tree.md → Rules).
  bool ReadDocument(const FString &InJson, FDocument &OutDocument,
                    TArray<FString> &OutErrors);
  bool ReadDocument(const TSharedRef<FJsonObject> &InJson, FDocument &OutDocument,
                    TArray<FString> &OutErrors);

  // Writes the document as design.json, leaving defaults out.
  TSharedRef<FJsonObject> WriteDocument(const FDocument &InDocument);
  FString WriteDocumentString(const FDocument &InDocument);

  // SHA-1 (hex) of the node as design.json writes it (keys in a fixed
  // order, defaults left out) followed by InExtra: the basis of a component
  // entry's `hash` (import-tree.md → Components).
  FString HashNode(const FNode &InNode, const FString &InExtra = FString());

  // Lengths to 1/100 design pixel, as the readers write them: source floats
  // carry noise (100.0000076).
  double Round2(double InValue);

  // A readers' cache-folder or file name: letters, digits and '-' kept,
  // everything else '_'; InFallback when that leaves nothing.
  FString FileSafeName(const FString &InName, const TCHAR *InFallback);

  // ---------------------------------------------------------------------
  // Converter (import-tree.md → Converter)

  struct FResolvedFont
  {
    // Object path of a UFont, e.g. /Game/Fonts/Inter.Inter
    FString FontObject;
    FName Typeface;
  };

  // Role → widget name for one node, e.g. { main: Card, bg: Card_Bg }.
  using FRoleNames = TMap<FString, FString>;
  // Node id → its roles.
  using FOwnedMap = TMap<FString, FRoleNames>;

  struct FComponentIndexEntry
  {
    // Package path of the child WBP, e.g. /Game/UI/Components/WBP_Button/WBP_Button
    FString AssetPath;
    FString Hash;
    // Where the main component it was last built from lives; the converter
    // doesn't read these.
    FString FileKey;
    FString NodeId;
    FString Version;
  };

  struct FConvertOptions
  {
    // The WBP being built, e.g. WBP_Shop.
    FString BlueprintName;
    // /Game/... folder that holds the blueprint's own folder.
    FString TargetFolder;
    // /Game/... folder that holds the child WBPs' own folders.
    FString ComponentsFolder;
    // The sidecar's owned-widget map from the last import.
    FOwnedMap Names;
    // Names no new widget may take. Compared ignoring case, like FName.
    TSet<FString> ReservedNames;
    TMap<FString, FComponentIndexEntry> ComponentIndex;
    // Image path → texture object path, from the sidecar.
    TMap<FString, FString> Textures;
    // Asset names already in the blueprint's folder or its subfolders: a new
    // texture never takes one (it would overwrite an asset the import doesn't
    // own).
    TSet<FString> TakenAssetNames;
    // Asset names already in the components folder: a new child WBP never
    // takes one, even when the components index doesn't know it.
    TSet<FString> TakenComponentNames;
    // Unset or returning an empty optional = unmapped (default font).
    TFunction<TOptional<FResolvedFont>(const FFontRef &)> ResolveFont;
    // The font's natural line height, in design pixels, at a design pixel
    // size. Unset or <= 0: LineHeightPercentage is left at 1.
    TFunction<double(const TOptional<FResolvedFont> &, double)> NaturalLineHeight;
    // The tree is a child WBP built from a component: its root gets no
    // size role (each instance's slot sizes it).
    bool bChildComponent = false;
    // Every instance is expanded inline, overrides or not, and no child WBP
    // jobs are returned. Imports use this until child WBPs are built
    // (import-tree.md → Implementation order, Phase 5).
    bool bInlineInstances = false;
  };

  struct FReportEntry
  {
    FString Node;
    FString Category;
    FString Detail;
  };

  struct FTextureAsset
  {
    // Image path as in the tree (relative to design.json).
    FString SourcePath;
    // Object path, e.g. /Game/UI/WBP_Shop/Textures/T_WBP_Shop_Icon.T_WBP_Shop_Icon
    FString ObjectPath;
    // Already imported before (from options.Textures): write pixels in place.
    bool bExisting = false;
  };

  enum class EComponentAction : uint8
  {
    Create,
    Update,
    Reuse
  };

  struct FComponentJob
  {
    FString Key;
    FString AssetPath;
    EComponentAction Action = EComponentAction::Reuse;
    // The main component's hash, for the components index. Empty when the
    // job had to fall back to an instance.
    FString Hash;
    // The child WBP's own design tree (widget mode), for Create and Update.
    TSharedPtr<FDocument> Tree;
  };

  struct FConvertResult
  {
    // Null when the spec would exceed Apply's limits (see Report).
    TSharedPtr<FJsonObject> Spec;
    TArray<FTextureAsset> Assets;
    TArray<FComponentJob> Components;
    FOwnedMap Owned;
    TMap<FString, FString> Textures;
    TArray<FReportEntry> Report;
    int32 WidgetCount = 0;
    int32 MaxDepth = 0;
    // The root mode used: the document's, or Screen / Widget decided from
    // the root box when the document leaves it out. Never Auto.
    ERootMode RootMode = ERootMode::Screen;
  };

  // Apply's limits.
  constexpr int32 MaxWidgets = 4000;
  constexpr int32 MaxDepth = 64;

  // Deterministic and pure: the same tree and options always give the same
  // result. A tree with layout:row / layout:column hints is converted as
  // ApplyLayoutHints leaves it.
  FConvertResult Convert(const FDocument &InDocument, const FConvertOptions &InOptions);

  // Hints a reader that can't measure layout itself (the image reader)
  // leaves for the converter (import-image.md, section 2):
  // - layout:row / layout:column on a frame or group: a layout block
  //   measured from the children's boxes (spacing from the gaps, padding
  //   from the margins, cross alignment from the edges or centres), children
  //   in box order. Uneven gaps and alignments are noted (layoutApprox);
  //   overlapping children leave the node as it is (hintIgnored).
  // - role:button on a frame: its bg role is a Button with the frame's look
  //   as the normal style (the converter).
  // - role:list on a laid-out frame: a ScrollBox around its content (the
  //   converter).
  bool HasLayoutHints(const FDocument &InDocument);
  // Returns whether anything changed; notes go into the document's notes.
  bool ApplyLayoutHints(FDocument &InOutDocument);

  // The base name a node's `name` sanitizes to (import-tree.md → Names).
  FString SanitizeName(const FString &InName, EKind InKind);

  // The typeface of UMG's default font (Roboto: Regular, Bold, Italic,
  // Bold Italic, Light) nearest to an unmapped font's style, from its style
  // name or, without one, its PostScript name. UMG's own default is Bold, so
  // an unmapped Regular text would otherwise render bold.
  FName DefaultTypeface(const FFontRef &InFont);
}
