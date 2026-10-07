#include "UIWTDesignTree.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

// The converter: design tree → draft WidgetSpec (import-tree.md →
// Converter). Three steps over the tree:
//   Collect (pre-order): each node's roles, sizes, widget count and depth.
//   Name: widget names from the sidecar, the base names and the roles.
//   Emit (pre-order): the spec JSON, owned widgets, textures and report.
// Property names and value forms are what UToolsetLibrary's JSON uses
// (short enum names, colours as {r,g,b,a}, object paths as strings).
namespace
{
  using namespace UIWTDesignTree;

  // ---------------------------------------------------------------------
  // Spec JSON helpers

  // The shortest text that reads back as the same double, so the spec (and
  // the golden files) show 10.1, not 10.0999999999999996.
  FString ShortNumber(double InValue)
  {
    if (!FMath::IsFinite(InValue) || InValue == 0.0)
    {
      return TEXT("0");
    }
    if (FMath::Abs(InValue) < 9007199254740992.0 &&
        InValue == FMath::FloorToDouble(InValue))
    {
      return FString::Printf(TEXT("%.0f"), InValue);
    }
    for (int32 Digits = 1; Digits < 17; ++Digits)
    {
      const FString Text = FString::Printf(TEXT("%.*g"), Digits, InValue);
      if (FCString::Atod(*Text) == InValue)
      {
        return Text;
      }
    }
    return FString::Printf(TEXT("%.17g"), InValue);
  }

  TSharedRef<FJsonValue> Num(double InValue)
  {
    return MakeShared<FJsonValueNumberString>(ShortNumber(InValue));
  }

  TSharedRef<FJsonValue> Str(const FString &InValue)
  {
    return MakeShared<FJsonValueString>(InValue);
  }

  TSharedRef<FJsonValue> Bool(bool bInValue)
  {
    return MakeShared<FJsonValueBoolean>(bInValue);
  }

  TSharedRef<FJsonValue> Obj(const TSharedRef<FJsonObject> &InObject)
  {
    return MakeShared<FJsonValueObject>(InObject);
  }

  TSharedRef<FJsonObject> Vec2(double InX, double InY)
  {
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetField(TEXT("X"), Num(InX));
    Object->SetField(TEXT("Y"), Num(InY));
    return Object;
  }

  TSharedRef<FJsonObject> Margin(double InL, double InT, double InR, double InB)
  {
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetField(TEXT("Left"), Num(InL));
    Object->SetField(TEXT("Top"), Num(InT));
    Object->SetField(TEXT("Right"), Num(InR));
    Object->SetField(TEXT("Bottom"), Num(InB));
    return Object;
  }

  // Colour channels with 5 decimals, as Export writes them.
  double Round5(double InValue)
  {
    return FMath::RoundToDouble(InValue * 100000.0) / 100000.0;
  }

  // The tree's colours are sRGB; UMG's FLinearColor values are linear.
  TSharedRef<FJsonObject> LinearColor(const FColor &InSRGB)
  {
    const FLinearColor Linear(InSRGB);
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetField(TEXT("r"), Num(Round5(Linear.R)));
    Object->SetField(TEXT("g"), Num(Round5(Linear.G)));
    Object->SetField(TEXT("b"), Num(Round5(Linear.B)));
    Object->SetField(TEXT("a"), Num(Round5(Linear.A)));
    return Object;
  }

  TSharedRef<FJsonObject> SlateColor(const FColor &InSRGB)
  {
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetObjectField(TEXT("SpecifiedColor"), LinearColor(InSRGB));
    Object->SetStringField(TEXT("ColorUseRule"), TEXT("UseColor_Specified"));
    return Object;
  }

  // A colour lightened towards white (InShade > 0) or darkened (< 0) by that
  // fraction, alpha kept: a Button's hovered and pressed states.
  FColor Shade(const FColor &InColor, double InShade)
  {
    if (InShade == 0.0)
    {
      return InColor;
    }
    auto Channel = [InShade](uint8 InValue)
    {
      const double Value = InShade > 0.0 ? InValue + (255.0 - InValue) * InShade
                                         : InValue * (1.0 + InShade);
      return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Value), 0, 255));
    };
    return FColor(Channel(InColor.R), Channel(InColor.G), Channel(InColor.B), InColor.A);
  }
  constexpr double ButtonHoverShade = 0.1;
  constexpr double ButtonPressShade = -0.15;

  bool HasHint(const FNode &InNode, const TCHAR *InHint)
  {
    return InNode.Hints.Contains(InHint);
  }

  // A text stroke as a font OutlineSize, in whole Slate units. Slate draws
  // text outlines outside the glyphs only, so `outside` keeps the weight,
  // `center` keeps its outer half and `inside` has nothing to draw (0).
  int32 TextOutlineSize(const FNode &InNode)
  {
    if (InNode.Kind != EKind::Text || !InNode.Stroke.IsValid() ||
        InNode.Stroke->Align == EStrokeAlign::Inside)
    {
      return 0;
    }
    const FEdges &Weights = InNode.Stroke->Weights;
    const double Width =
        FMath::Max(FMath::Max(Weights.L, Weights.T), FMath::Max(Weights.R, Weights.B));
    const double Outer = InNode.Stroke->Align == EStrokeAlign::Center ? Width * 0.5 : Width;
    return Outer > 0.0 ? FMath::Max(1, FMath::RoundToInt(Outer)) : 0;
  }

  // The drop shadow a text node's TextBlock draws: the first one with a
  // colour and an offset (Slate draws no shadow at offset 0).
  const FEffect *TextShadow(const FNode &InNode)
  {
    if (InNode.Kind != EKind::Text)
    {
      return nullptr;
    }
    return InNode.Effects.FindByPredicate(
        [](const FEffect &Effect)
        {
          return !Effect.bBaked && Effect.Type == TEXT("dropShadow") && Effect.Color.IsSet() &&
                 !Effect.Offset.IsZero();
        });
  }

  // How far a TextBlock outgrows the text's box. Slate's text runs measure
  // the outline in on every side, and the shadow on the side it falls
  // towards (a negative offset moves the glyphs away from it instead).
  FEdges TextGrowth(const FNode &InNode)
  {
    const double Outline = TextOutlineSize(InNode);
    FEdges Growth = {Outline, Outline, Outline, Outline};
    if (const FEffect *Shadow = TextShadow(InNode))
    {
      Growth.L += FMath::Max(0.0, -Shadow->Offset.X);
      Growth.T += FMath::Max(0.0, -Shadow->Offset.Y);
      Growth.R += FMath::Max(0.0, Shadow->Offset.X);
      Growth.B += FMath::Max(0.0, Shadow->Offset.Y);
    }
    return Growth;
  }

  // A spec node being built.
  struct FWidget
  {
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> Slot = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Children;

    FWidget(const FString &InClass, const FString &InName)
    {
      Object->SetStringField(TEXT("class"), InClass);
      Object->SetStringField(TEXT("name"), InName);
      // Explicit: UWidget defaults to a variable (import-tree.md → Widget
      // chains and roles).
      Object->SetBoolField(TEXT("variable"), false);
    }

    void AddChild(FWidget &InChild) { Children.Add(Obj(InChild.Finish())); }

    TSharedRef<FJsonObject> Finish()
    {
      if (!Props->Values.IsEmpty())
      {
        Object->SetObjectField(TEXT("props"), Props);
      }
      if (!Slot->Values.IsEmpty())
      {
        Object->SetObjectField(TEXT("slot"), Slot);
      }
      if (!Children.IsEmpty())
      {
        Object->SetArrayField(TEXT("children"), Children);
      }
      return Object;
    }
  };

  // ---------------------------------------------------------------------
  // Hashing and names

  // SHA-1 of an id, lowercase hex: the source of stable name suffixes.
  FString HashHex(const FString &InText)
  {
    FTCHARToUTF8 Utf8(*InText);
    FSHAHash Hash;
    FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash.Hash);
    return Hash.ToString().ToLower();
  }

  // Names compare ignoring case, like FName.
  FString NameKey(const FString &InName) { return InName.ToLower(); }

  // Gives each name in InGroup (all wanting InWanted, ignoring case) a free
  // name: the item with the lowest id keeps the plain name when it's free,
  // the others get _ + 4, then 8, then all hex digits of their id's hash.
  // Depends only on the ids, never on tree order. Each item keeps its own
  // spelling (Wanted) when it has one, so Title and title stay as written.
  struct FNameRequest
  {
    FString Id;
    FString Hash;
    FString *Out = nullptr;
    FString Wanted;
  };

  void SettleNames(const FString &InWanted, TArray<FNameRequest> &InGroup,
                   TSet<FString> &InOutTaken)
  {
    InGroup.Sort([](const FNameRequest &A, const FNameRequest &B)
                 { return A.Id.Compare(B.Id, ESearchCase::CaseSensitive) < 0; });
    auto Base = [&InWanted](const FNameRequest &InRequest) -> const FString &
    { return InRequest.Wanted.IsEmpty() ? InWanted : InRequest.Wanted; };
    int32 First = 0;
    if (!InOutTaken.Contains(NameKey(InWanted)))
    {
      *InGroup[0].Out = Base(InGroup[0]);
      InOutTaken.Add(NameKey(InWanted));
      First = 1;
    }
    for (int32 Index = First; Index < InGroup.Num(); ++Index)
    {
      FNameRequest &Request = InGroup[Index];
      for (const int32 Digits : {4, 8, 40})
      {
        const FString Candidate = Base(Request) + TEXT("_") + Request.Hash.Left(Digits);
        if (!InOutTaken.Contains(NameKey(Candidate)))
        {
          *Request.Out = Candidate;
          InOutTaken.Add(NameKey(Candidate));
          break;
        }
      }
    }
  }

  // ---------------------------------------------------------------------
  // Plans

  enum class EParent : uint8
  {
    Root,
    Canvas,
    Layout
  };

  // Roles in chain order, outermost first (import-tree.md → Widget chains
  // and roles), with their name suffixes.
  const TCHAR *const RoleSize = TEXT("size");
  const TCHAR *const RoleFit = TEXT("fit");
  const TCHAR *const RoleLayers = TEXT("layers");
  const TCHAR *const RoleBg = TEXT("bg");
  // In place of bg on a frame hinted role:button (import-image.md, section 2).
  const TCHAR *const RoleButton = TEXT("button");
  const TCHAR *const RoleClip = TEXT("clip");
  // A ScrollBox around a laid-out frame hinted role:list.
  const TCHAR *const RoleScroll = TEXT("scroll");
  const TCHAR *const RoleMain = TEXT("main");
  const TCHAR *const RoleAbs = TEXT("abs");

  FString RoleSuffix(const FString &InRole)
  {
    if (InRole.StartsWith(TEXT("spacer")))
    {
      return TEXT("Spacer") + InRole.Mid(6);
    }
    FString Suffix = InRole;
    Suffix[0] = FChar::ToUpper(Suffix[0]);
    return Suffix;
  }

  // What the collect step decides for one node.
  struct FPlan
  {
    const FNode *Node = nullptr;
    FString Hash;
    FString BaseName;
    EParent Parent = EParent::Root;
    // A group in a canvas parent: a canvas covering the parent.
    bool bCover = false;
    // An instance without overrides: its main widget is the child WBP.
    bool bChildWbp = false;
    // A frame, or an instance expanded inline.
    bool bFrameLike = false;
    FString MainClass;
    ESizing SizingH = ESizing::Fixed;
    ESizing SizingV = ESizing::Fixed;
    // How far the outermost widget reaches past the node's box on each side
    // (a stroke, a text outline or shadow), without moving anything else.
    FEdges Growth;
    bool bSize = false;
    bool bFit = false;
    bool bLayers = false;
    bool bBg = false;
    // The bg role is a Button (role:button hint).
    bool bButton = false;
    bool bClip = false;
    bool bScroll = false;
    int32 Spacers = 0;
    TOptional<double> WidthOverride;
    TOptional<double> HeightOverride;
    bool bAutoSize = false;
    int32 SubtreeWidgets = 0;
    FRoleNames Names;

    // The roles present, outermost first.
    TArray<FString> Roles() const
    {
      TArray<FString> Result;
      if (bSize)
      {
        Result.Add(RoleSize);
      }
      if (bFit)
      {
        Result.Add(RoleFit);
      }
      if (bLayers)
      {
        Result.Add(RoleLayers);
      }
      if (bBg)
      {
        Result.Add(bButton ? RoleButton : RoleBg);
      }
      if (bClip)
      {
        Result.Add(RoleClip);
      }
      if (bScroll)
      {
        Result.Add(RoleScroll);
      }
      Result.Add(RoleMain);
      if (bLayers)
      {
        Result.Add(RoleAbs);
      }
      for (int32 Index = 1; Index <= Spacers; ++Index)
      {
        Result.Add(FString::Printf(TEXT("spacer%d"), Index));
      }
      return Result;
    }
  };

  // `sizing` as the converter uses it: text derives it from text.sizing,
  // and `fill` only means something in a layout parent.
  void EffectiveSizing(const FNode &InNode, EParent InParent, ESizing &OutH,
                       ESizing &OutV)
  {
    OutH = InNode.SizingH;
    OutV = InNode.SizingV;
    if (InNode.Kind == EKind::Text && InNode.Text.IsValid())
    {
      const ETextSizing Sizing = InNode.Text->Sizing;
      if (OutH != ESizing::Fill)
      {
        OutH = Sizing == ETextSizing::Auto ? ESizing::Hug : ESizing::Fixed;
      }
      if (OutV != ESizing::Fill)
      {
        OutV = Sizing == ETextSizing::Fixed ? ESizing::Fixed : ESizing::Hug;
      }
    }
    if (InParent != EParent::Layout)
    {
      OutH = OutH == ESizing::Fill ? ESizing::Fixed : OutH;
      OutV = OutV == ESizing::Fill ? ESizing::Fixed : OutV;
    }
  }

  bool IsStretching(EConstraint InConstraint)
  {
    return InConstraint == EConstraint::Stretch || InConstraint == EConstraint::Scale;
  }

  // ---------------------------------------------------------------------
  // The conversion

  class FConverter
  {
  public:
    FConverter(const FDocument &InDocument, const FConvertOptions &InOptions)
        : Doc(InDocument), Options(InOptions)
    {
    }

    FConvertResult Run();

  private:
    const FDocument &Doc;
    const FConvertOptions &Options;
    FConvertResult Result;
    ERootMode RootMode = ERootMode::Screen;

    TArray<FPlan> Plans;
    TMap<const FNode *, int32> PlanIndex;
    int32 WidgetCount = 0;
    int32 MaxDepthSeen = 0;
    // component.key → the first instance using it, in tree order.
    TMap<FString, const FNode *> FirstInstance;
    TArray<FString> ComponentKeys;
    TMap<FString, FString> ComponentPaths;

    TSet<FString> TextureNamesTaken;
    TMap<FString, TOptional<FResolvedFont>> FontCache;
    TMap<FString, double> LineHeightCache;

    void Report(const FString &InNode, const TCHAR *InCategory, const FString &InDetail)
    {
      Result.Report.Add({InNode, InCategory, InDetail});
    }

    int32 Collect(const FNode &InNode, EParent InParent, const FLayout *InParentLayout,
                  int32 InDepth);
    void NameWidgets();
    void NameComponents();
    void EmitNode(const FNode &InNode, EParent InParent, const FLayout *InParentLayout,
                  bool bInFirstFlow, double InPW, double InPH, double InOX, double InOY,
                  TArray<TSharedPtr<FJsonValue>> &OutSiblings);
    void EmitFlowChildren(const FNode &InNode, const FPlan &InPlan, FWidget &InMain);
    void EmitMainProps(const FNode &InNode, const FPlan &InPlan, FWidget &InMain);
    // InShade > 0 lightens the fill towards white, < 0 darkens it.
    TSharedRef<FJsonObject> SolidBrush(const FNode &InNode, double InShade = 0.0);
    TSharedRef<FJsonObject> ImageBrush(const FNode &InNode, const FImageRef &InImage,
                                       const FString &InMainName, double InW, double InH,
                                       bool bInAllowOutline);
    FString TextureFor(const FString &InPath, const FString &InMainName);
    void BuildComponentJobs();
    TSharedPtr<FDocument> ComponentTree(const FNode &InRoot, bool bInFromInstance,
                                        const FComponentDef *InDef);
    TOptional<FResolvedFont> ResolveFont(const FFontRef &InFont);
    double NaturalLineHeight(const FFontRef &InFont, double InSizePx);
  };

  // --- Collect -----------------------------------------------------------

  int32 FConverter::Collect(const FNode &InNode, EParent InParent,
                            const FLayout *InParentLayout, int32 InDepth)
  {
    const int32 Index = Plans.AddDefaulted();
    PlanIndex.Add(&InNode, Index);
    {
      FPlan &Plan = Plans[Index];
      Plan.Node = &InNode;
      Plan.Hash = HashHex(InNode.Id);
      Plan.BaseName = SanitizeName(InNode.Name, InNode.Kind);
      Plan.Parent = InParent;
    }
    // Plans may reallocate while children are collected: work on a copy and
    // store it back at the end.
    FPlan Plan = Plans[Index];

    const bool bInstance = InNode.Kind == EKind::Instance;
    Plan.bChildWbp = bInstance && InNode.Component.IsValid() &&
                     !InNode.Component->bHasOverrides && !Options.bInlineInstances;
    Plan.bFrameLike = InNode.Kind == EKind::Frame || (bInstance && !Plan.bChildWbp);
    EffectiveSizing(InNode, InParent, Plan.SizingH, Plan.SizingV);

    // Groups are never folded away: the blueprint keeps the design's layer
    // hierarchy.
    if (InNode.Kind == EKind::Group)
    {
      if (InParent == EParent::Canvas)
      {
        if (InNode.bClip)
        {
          Report(InNode.Id, TEXT("groupClip"),
                 TEXT("the group clips, so it keeps a canvas of its own size and "
                      "its children's constraints resolve against the group"));
        }
        else
        {
          Plan.bCover = true;
        }
      }
    }

    // A stroke drawn by this node's brush grows the widget (UMG draws
    // outlines inside the brush box).
    const bool bDrawsStroke =
        InNode.Stroke.IsValid() &&
        (Plan.bFrameLike || InNode.Kind == EKind::Shape ||
         (InNode.Kind == EKind::Image && InNode.Image.IsValid() &&
          InNode.Image->Mode == EImageMode::Stretch));
    if (bDrawsStroke)
    {
      const FEdges &Weights = InNode.Stroke->Weights;
      const double Width = FMath::Max(FMath::Max(Weights.L, Weights.T),
                                      FMath::Max(Weights.R, Weights.B));
      const double Grow = InNode.Stroke->Align == EStrokeAlign::Center    ? Width * 0.5
                          : InNode.Stroke->Align == EStrokeAlign::Outside ? Width
                                                                          : 0.0;
      Plan.Growth = {Grow, Grow, Grow, Grow};
    }
    else if (InNode.Kind == EKind::Text)
    {
      Plan.Growth = TextGrowth(InNode);
    }

    const FLayout *Layout = Plan.bFrameLike ? InNode.Layout.Get() : nullptr;
    if (Plan.bFrameLike)
    {
      Plan.bBg = InNode.Fill.IsValid() || InNode.Stroke.IsValid() ||
                 InNode.Radii.IsSet() || (Layout && !Layout->Padding.IsZero());
      Plan.bLayers = Layout && InNode.Children.ContainsByPredicate(
                                   [](const FNode &Child) { return Child.bAbsolute; });
      // Hints from readers that can't express these themselves (the image
      // reader; import-image.md, section 2). Figma and Photoshop don't write
      // them.
      if (HasHint(InNode, TEXT("role:button")))
      {
        Plan.bButton = true;
        Plan.bBg = true;
      }
      if (HasHint(InNode, TEXT("role:list")))
      {
        if (Layout && !Layout->bWrap)
        {
          Plan.bScroll = true;
        }
        else
        {
          Report(InNode.Id, TEXT("hintIgnored"),
                 TEXT("role:list needs a row or column (layout:row / layout:column); the "
                      "frame stays as it is"));
        }
      }
      Plan.bClip = Plan.bBg && InNode.bClip && !Plan.Growth.IsZero();
    }
    Plan.bFit = InNode.Kind == EKind::Image && InNode.Image.IsValid() &&
                InNode.Image->Mode == EImageMode::Fit;

    if (InNode.Kind == EKind::Group)
    {
      Plan.MainClass = TEXT("CanvasPanel");
    }
    else if (Plan.bFrameLike)
    {
      Plan.MainClass = !Layout                                 ? TEXT("CanvasPanel")
                       : Layout->bWrap                         ? TEXT("WrapBox")
                       : Layout->Mode == ELayoutMode::Horizontal ? TEXT("HorizontalBox")
                                                               : TEXT("VerticalBox");
    }
    else if (InNode.Kind == EKind::Text)
    {
      Plan.MainClass = TEXT("TextBlock");
    }
    else if (InNode.Kind == EKind::Shape)
    {
      Plan.MainClass = TEXT("Border");
    }
    else if (InNode.Kind == EKind::Image)
    {
      Plan.MainClass = TEXT("Image");
    }
    // Child WBPs get their class path once components are named.

    // Alignment Spacers (H/VBox only).
    if (Layout && !Layout->bWrap && Layout->AlignMain != EAlignMain::Start)
    {
      int32 Flow = 0;
      bool bAnyFill = false;
      for (const FNode &Child : InNode.Children)
      {
        if (Child.bAbsolute)
        {
          continue;
        }
        ++Flow;
        ESizing ChildH;
        ESizing ChildV;
        EffectiveSizing(Child, EParent::Layout, ChildH, ChildV);
        bAnyFill |= (Layout->Mode == ELayoutMode::Horizontal ? ChildH : ChildV) ==
                    ESizing::Fill;
      }
      if (Flow > 0 && !bAnyFill)
      {
        Plan.Spacers = Layout->AlignMain == EAlignMain::End      ? 1
                       : Layout->AlignMain == EAlignMain::Center ? 2
                                                                 : Flow - 1;
      }
    }

    // Size overrides (the `size` role).
    if (!Plan.bCover)
    {
      const double W = InNode.Box.W + Plan.Growth.L + Plan.Growth.R;
      const double H = InNode.Box.H + Plan.Growth.T + Plan.Growth.B;
      bool bStretchedH = false;
      bool bStretchedV = false;
      if (InParent == EParent::Layout && InParentLayout)
      {
        const bool bMainH = InParentLayout->Mode == ELayoutMode::Horizontal;
        const bool bCrossStretch = InParentLayout->AlignCross == EAlignCross::Stretch;
        bStretchedH = Plan.SizingH == ESizing::Fill || (!bMainH && bCrossStretch);
        bStretchedV = Plan.SizingV == ESizing::Fill || (bMainH && bCrossStretch);
        if (Plan.SizingH == ESizing::Fixed && !bStretchedH)
        {
          Plan.WidthOverride = W;
        }
        if (Plan.SizingV == ESizing::Fixed && !bStretchedV)
        {
          Plan.HeightOverride = H;
        }
      }
      else if (InParent == EParent::Canvas)
      {
        const bool bHugH = Plan.SizingH == ESizing::Hug;
        const bool bHugV = Plan.SizingV == ESizing::Hug;
        const bool bAnyStretch =
            InNode.bHasConstraints &&
            (IsStretching(InNode.ConstraintH) || IsStretching(InNode.ConstraintV));
        bStretchedH = InNode.bHasConstraints && IsStretching(InNode.ConstraintH);
        bStretchedV = InNode.bHasConstraints && IsStretching(InNode.ConstraintV);
        if ((bHugH || bHugV) && bAnyStretch)
        {
          Report(InNode.Id, TEXT("hugApprox"),
                 TEXT("the node hugs its content but a constraint stretches it; "
                      "it keeps its designed size"));
          Plan.SizingH = bHugH ? ESizing::Fixed : Plan.SizingH;
          Plan.SizingV = bHugV ? ESizing::Fixed : Plan.SizingV;
        }
        else if (bHugH || bHugV)
        {
          Plan.bAutoSize = true;
          if (!bHugH)
          {
            Plan.WidthOverride = W;
          }
          if (!bHugV)
          {
            Plan.HeightOverride = H;
          }
        }
      }
      else if (RootMode == ERootMode::Widget && !Options.bChildComponent)
      {
        if (Plan.SizingH != ESizing::Hug)
        {
          Plan.WidthOverride = W;
        }
        if (Plan.SizingV != ESizing::Hug)
        {
          Plan.HeightOverride = H;
        }
      }
      // Wrapping text needs its width, and fixed text both sizes (its
      // vertical alignment sits in the SizeBox), whatever the parent.
      if (InNode.Kind == EKind::Text && InNode.Text.IsValid() &&
          !(InParent == EParent::Root && Options.bChildComponent))
      {
        if (InNode.Text->Sizing != ETextSizing::Auto && Plan.SizingH == ESizing::Fixed &&
            !bStretchedH && !Plan.WidthOverride.IsSet())
        {
          Plan.WidthOverride = W;
        }
        if (InNode.Text->Sizing == ETextSizing::Fixed && Plan.SizingV == ESizing::Fixed &&
            !bStretchedV && !Plan.HeightOverride.IsSet())
        {
          Plan.HeightOverride = H;
        }
      }
      Plan.bSize = Plan.WidthOverride.IsSet() || Plan.HeightOverride.IsSet() ||
                   InNode.SizeLimits.IsSet();
    }

    // Widget count and depth.
    const int32 Size = Plan.bSize ? 1 : 0;
    const int32 Fit = Plan.bFit ? 1 : 0;
    const int32 Layers = Plan.bLayers ? 1 : 0;
    const int32 Bg = Plan.bBg ? 1 : 0;
    const int32 Clip = Plan.bClip ? 1 : 0;
    const int32 Scroll = Plan.bScroll ? 1 : 0;
    int32 Widgets = Size + Fit + Layers * 2 + Bg + Clip + Scroll + 1 + Plan.Spacers;
    const int32 MainDepth = InDepth + Size + Fit + Layers + Bg + Clip + Scroll;
    const int32 AbsDepth = InDepth + Size + Fit + Layers;
    MaxDepthSeen = FMath::Max(MaxDepthSeen, Plan.Spacers > 0 ? MainDepth + 1 : MainDepth);
    MaxDepthSeen = FMath::Max(MaxDepthSeen, AbsDepth);

    // Children.
    if (Plan.bChildWbp)
    {
      FirstInstance.FindOrAdd(InNode.Component->Key, &InNode);
      ComponentKeys.AddUnique(InNode.Component->Key);
    }
    else if (Plan.bFrameLike && Layout)
    {
      for (const FNode &Child : InNode.Children)
      {
        Widgets += Child.bAbsolute ? Collect(Child, EParent::Canvas, nullptr, AbsDepth + 1)
                                   : Collect(Child, EParent::Layout, Layout, MainDepth + 1);
      }
    }
    else
    {
      for (const FNode &Child : InNode.Children)
      {
        Widgets += Collect(Child, EParent::Canvas, nullptr, MainDepth + 1);
      }
    }

    Plan.SubtreeWidgets = Widgets;
    Plans[Index] = MoveTemp(Plan);
    return Widgets;
  }

  // --- Names --------------------------------------------------------------

  void FConverter::NameWidgets()
  {
    TSet<FString> Taken;
    TSet<FString> Reserved;
    for (const FString &Name : Options.ReservedNames)
    {
      Reserved.Add(NameKey(Name));
    }
    // FName("None") is NAME_None, which Apply can't use as a name.
    Reserved.Add(NameKey(TEXT("None")));
    Taken.Append(Reserved);

    // Names from the last import win, for the same node and role.
    for (FPlan &Plan : Plans)
    {
      const FRoleNames *Old = Options.Names.Find(Plan.Node->Id);
      if (!Old)
      {
        continue;
      }
      for (const FString &Role : Plan.Roles())
      {
        const FString *Name = Old->Find(Role);
        if (!Name || Name->IsEmpty())
        {
          continue;
        }
        if (Taken.Contains(NameKey(*Name)))
        {
          if (Reserved.Contains(NameKey(*Name)))
          {
            Report(Plan.Node->Id, TEXT("nameReserved"),
                   FString::Printf(TEXT("'%s' is now used by the blueprint; the "
                                        "widget gets a new name"),
                                   **Name));
          }
          continue;
        }
        Taken.Add(NameKey(*Name));
        Plan.Names.Add(Role, *Name);
      }
    }

    // Main names from the base names.
    TMap<FString, TArray<FNameRequest>> MainGroups;
    TMap<FString, FString> MainWanted;
    for (FPlan &Plan : Plans)
    {
      if (Plan.Names.Contains(RoleMain))
      {
        continue;
      }
      const FString Key = NameKey(Plan.BaseName);
      MainWanted.FindOrAdd(Key, Plan.BaseName);
      MainGroups.FindOrAdd(Key).Add(
          {Plan.Node->Id, Plan.Hash, &Plan.Names.Add(RoleMain), Plan.BaseName});
    }
    TArray<FString> Keys;
    MainGroups.GetKeys(Keys);
    Keys.Sort();
    for (const FString &Key : Keys)
    {
      SettleNames(MainWanted[Key], MainGroups[Key], Taken);
    }

    // Role names from the final main names. A main name never moves to make
    // room for a role name.
    TMap<FString, TArray<FNameRequest>> RoleGroups;
    TMap<FString, FString> RoleWanted;
    for (FPlan &Plan : Plans)
    {
      const FString MainName = Plan.Names[RoleMain];
      // Add every missing role first: the requests point into Plan.Names,
      // which must not grow afterwards.
      TArray<FString> Missing;
      for (const FString &Role : Plan.Roles())
      {
        if (Role != RoleMain && !Plan.Names.Contains(Role))
        {
          Missing.Add(Role);
          Plan.Names.Add(Role);
        }
      }
      for (const FString &Role : Missing)
      {
        const FString Wanted = MainName + TEXT("_") + RoleSuffix(Role);
        const FString Key = NameKey(Wanted);
        RoleWanted.FindOrAdd(Key, Wanted);
        RoleGroups.FindOrAdd(Key).Add(
            {Plan.Node->Id + TEXT("/") + Role, Plan.Hash, &Plan.Names[Role], Wanted});
      }
    }
    Keys.Reset();
    RoleGroups.GetKeys(Keys);
    Keys.Sort();
    for (const FString &Key : Keys)
    {
      SettleNames(RoleWanted[Key], RoleGroups[Key], Taken);
    }
  }

  void FConverter::NameComponents()
  {
    // Existing child WBPs keep their paths; new ones get WBP_<name>, with
    // the same collision rule as widget names.
    TSet<FString> Taken;
    for (const TPair<FString, FComponentIndexEntry> &Pair : Options.ComponentIndex)
    {
      Taken.Add(NameKey(FPaths::GetBaseFilename(Pair.Value.AssetPath)));
    }
    for (const FString &Name : Options.TakenComponentNames)
    {
      Taken.Add(NameKey(Name));
    }
    TMap<FString, TArray<FNameRequest>> Groups;
    TMap<FString, FString> Wanted;
    TMap<FString, FString> NewNames;
    TArray<FString> SortedKeys = ComponentKeys;
    SortedKeys.Sort();
    for (const FString &Key : SortedKeys)
    {
      if (const FComponentIndexEntry *Entry = Options.ComponentIndex.Find(Key))
      {
        ComponentPaths.Add(Key, Entry->AssetPath);
      }
      else
      {
        // All added before any request points into the map.
        NewNames.Add(Key);
      }
    }
    for (TPair<FString, FString> &Pair : NewNames)
    {
      const FComponentDef *Def = Doc.Components.Find(Pair.Key);
      const FString DisplayName =
          Def ? Def->Name : FirstInstance[Pair.Key]->Component->Name;
      const FString Name = TEXT("WBP_") + SanitizeName(DisplayName, EKind::Frame);
      Wanted.FindOrAdd(NameKey(Name), Name);
      Groups.FindOrAdd(NameKey(Name)).Add({Pair.Key, HashHex(Pair.Key), &Pair.Value});
    }
    TArray<FString> GroupKeys;
    Groups.GetKeys(GroupKeys);
    GroupKeys.Sort();
    for (const FString &GroupKey : GroupKeys)
    {
      SettleNames(Wanted[GroupKey], Groups[GroupKey], Taken);
    }
    for (const TPair<FString, FString> &Pair : NewNames)
    {
      ComponentPaths.Add(Pair.Key, Options.ComponentsFolder / Pair.Value / Pair.Value);
    }
  }

  // --- Fonts and textures -------------------------------------------------

  FString FontKey(const FFontRef &InFont)
  {
    return InFont.Family + TEXT("|") + InFont.Style + TEXT("|") + InFont.PostScript;
  }

  TOptional<FResolvedFont> FConverter::ResolveFont(const FFontRef &InFont)
  {
    const FString Key = FontKey(InFont);
    if (const TOptional<FResolvedFont> *Found = FontCache.Find(Key))
    {
      return *Found;
    }
    TOptional<FResolvedFont> Resolved;
    if (Options.ResolveFont)
    {
      Resolved = Options.ResolveFont(InFont);
    }
    FontCache.Add(Key, Resolved);
    return Resolved;
  }

  double FConverter::NaturalLineHeight(const FFontRef &InFont, double InSizePx)
  {
    const FString Key = FontKey(InFont) + TEXT("|") + ShortNumber(InSizePx);
    if (const double *Found = LineHeightCache.Find(Key))
    {
      return *Found;
    }
    const double Height =
        Options.NaturalLineHeight ? Options.NaturalLineHeight(ResolveFont(InFont), InSizePx)
                                  : 0.0;
    LineHeightCache.Add(Key, Height);
    return Height;
  }

  FString FConverter::TextureFor(const FString &InPath, const FString &InMainName)
  {
    if (const FString *Known = Result.Textures.Find(InPath))
    {
      return *Known;
    }
    FString ObjectPath;
    const bool bExisting = Options.Textures.Contains(InPath);
    if (bExisting)
    {
      ObjectPath = Options.Textures[InPath];
    }
    else
    {
      // Named once, after the first node using it (import-tree.md → Assets).
      const FString Wanted =
          TEXT("T_") + Options.BlueprintName + TEXT("_") + InMainName;
      FString AssetName;
      TArray<FNameRequest> Group = {{InPath, HashHex(InPath), &AssetName}};
      SettleNames(Wanted, Group, TextureNamesTaken);
      const FString Package =
          Options.TargetFolder / Options.BlueprintName / TEXT("Textures") / AssetName;
      ObjectPath = Package + TEXT(".") + AssetName;
    }
    Result.Textures.Add(InPath, ObjectPath);
    Result.Assets.Add({InPath, ObjectPath, bExisting});
    return ObjectPath;
  }

  // --- Brushes --------------------------------------------------------------

  // A rounded-box brush for a fill colour, stroke and radii. Padding-only
  // frames get a brush that draws nothing.
  TSharedRef<FJsonObject> FConverter::SolidBrush(const FNode &InNode, double InShade)
  {
    TSharedRef<FJsonObject> Brush = MakeShared<FJsonObject>();
    const TOptional<FColor> FillColor =
        InNode.Fill.IsValid() ? InNode.Fill->Color : TOptional<FColor>();
    if (!FillColor.IsSet() && !InNode.Stroke.IsValid() && !InNode.Radii.IsSet())
    {
      Brush->SetStringField(TEXT("DrawAs"), TEXT("NoDrawType"));
      return Brush;
    }
    Brush->SetStringField(TEXT("DrawAs"), TEXT("RoundedBox"));
    const FColor Tint = Shade(FillColor.Get(FColor(0, 0, 0, 0)), InShade);
    Brush->SetObjectField(TEXT("TintColor"), SlateColor(Tint));
    TSharedRef<FJsonObject> Outline = MakeShared<FJsonObject>();
    const FVector4 Radii = InNode.Radii.Get(FVector4(0.0, 0.0, 0.0, 0.0));
    TSharedRef<FJsonObject> Corners = MakeShared<FJsonObject>();
    Corners->SetField(TEXT("X"), Num(Radii.X));
    Corners->SetField(TEXT("Y"), Num(Radii.Y));
    Corners->SetField(TEXT("Z"), Num(Radii.Z));
    Corners->SetField(TEXT("W"), Num(Radii.W));
    Outline->SetObjectField(TEXT("CornerRadii"), Corners);
    Outline->SetStringField(TEXT("RoundingType"), TEXT("FixedRadius"));
    if (InNode.Stroke.IsValid())
    {
      const FEdges &Weights = InNode.Stroke->Weights;
      Outline->SetField(TEXT("Width"), Num(FMath::Max(FMath::Max(Weights.L, Weights.T),
                                                      FMath::Max(Weights.R, Weights.B))));
      Outline->SetObjectField(TEXT("Color"), SlateColor(InNode.Stroke->Color));
    }
    else
    {
      // A zero-width outline still tints the anti-aliased edge: give it the
      // fill colour.
      Outline->SetField(TEXT("Width"), Num(0.0));
      Outline->SetObjectField(TEXT("Color"), SlateColor(Tint));
    }
    Brush->SetObjectField(TEXT("OutlineSettings"), Outline);
    return Brush;
  }

  TSharedRef<FJsonObject> FConverter::ImageBrush(const FNode &InNode,
                                                 const FImageRef &InImage,
                                                 const FString &InMainName, double InW,
                                                 double InH, bool bInAllowOutline)
  {
    TSharedRef<FJsonObject> Brush = MakeShared<FJsonObject>();
    Brush->SetStringField(TEXT("ResourceObject"), TextureFor(InImage.Path, InMainName));
    const bool bOutline = InNode.Radii.IsSet() || InNode.Stroke.IsValid();
    auto TextureSize = [&]() -> FVector2D
    {
      if (InImage.Size.IsSet())
      {
        return *InImage.Size / InImage.Scale;
      }
      Report(InNode.Id, TEXT("imageSize"),
             TEXT("the image has no \"size\", so its box is used for the texture "
                  "size"));
      return FVector2D(InW, InH);
    };
    if (InImage.Mode == EImageMode::Tile)
    {
      const FVector2D Size = TextureSize();
      Brush->SetStringField(TEXT("DrawAs"), TEXT("Image"));
      Brush->SetStringField(TEXT("Tiling"), TEXT("Both"));
      Brush->SetObjectField(TEXT("ImageSize"), Vec2(Size.X, Size.Y));
    }
    else if (InImage.Mode == EImageMode::Fit && !bInAllowOutline)
    {
      // An Image in a ScaleBox: only the aspect ratio matters.
      const FVector2D Size = TextureSize();
      Brush->SetStringField(TEXT("DrawAs"), TEXT("Image"));
      Brush->SetObjectField(TEXT("ImageSize"), Vec2(Size.X, Size.Y));
    }
    else
    {
      if (InImage.Mode == EImageMode::Fit)
      {
        Report(InNode.Id, TEXT("imageFit"),
               TEXT("an image fill in fit mode is drawn stretched to the frame"));
      }
      Brush->SetObjectField(TEXT("ImageSize"), Vec2(InW, InH));
      if (bOutline)
      {
        // UE 5.8's rounded-box shader multiplies the brush texture by the
        // tint, so the image is drawn inside the rounded outline.
        TSharedRef<FJsonObject> Rounded = SolidBrush(InNode);
        Rounded->SetObjectField(TEXT("TintColor"), SlateColor(FColor::White));
        for (const auto &Pair : Rounded->Values)
        {
          Brush->SetField(FString(Pair.Key), Pair.Value);
        }
        return Brush;
      }
      Brush->SetStringField(TEXT("DrawAs"), TEXT("Image"));
      return Brush;
    }
    if (bOutline)
    {
      Report(InNode.Id, TEXT("imageCorners"),
             TEXT("a tiled or fitted image can't be drawn with rounded corners or a "
                  "stroke; they were dropped"));
    }
    return Brush;
  }

  // --- Emit -----------------------------------------------------------------

  const TCHAR *HAlign(EAlignCross InAlign)
  {
    switch (InAlign)
    {
    case EAlignCross::Center:
      return TEXT("HAlign_Center");
    case EAlignCross::End:
      return TEXT("HAlign_Right");
    case EAlignCross::Stretch:
      return TEXT("HAlign_Fill");
    default:
      return TEXT("HAlign_Left");
    }
  }

  const TCHAR *VAlign(EAlignCross InAlign)
  {
    switch (InAlign)
    {
    case EAlignCross::Center:
      return TEXT("VAlign_Center");
    case EAlignCross::End:
      return TEXT("VAlign_Bottom");
    case EAlignCross::Stretch:
      return TEXT("VAlign_Fill");
    default:
      return TEXT("VAlign_Top");
    }
  }

  void FConverter::EmitMainProps(const FNode &InNode, const FPlan &InPlan, FWidget &InMain)
  {
    const FString &MainName = InPlan.Names[RoleMain];
    const double W = InNode.Box.W + InPlan.Growth.L + InPlan.Growth.R;
    const double H = InNode.Box.H + InPlan.Growth.T + InPlan.Growth.B;
    if (InPlan.MainClass == TEXT("WrapBox"))
    {
      const FLayout &Layout = *InNode.Layout;
      const bool bHorizontal = Layout.Mode == ELayoutMode::Horizontal;
      InMain.Props->SetStringField(TEXT("Orientation"),
                                   bHorizontal ? TEXT("Orient_Horizontal")
                                               : TEXT("Orient_Vertical"));
      InMain.Props->SetObjectField(
          TEXT("InnerSlotPadding"),
          bHorizontal ? Vec2(Layout.Spacing, Layout.CrossSpacing)
                      : Vec2(Layout.CrossSpacing, Layout.Spacing));
      if (bHorizontal)
      {
        const TCHAR *Align = Layout.AlignMain == EAlignMain::Center ? TEXT("HAlign_Center")
                             : Layout.AlignMain == EAlignMain::End  ? TEXT("HAlign_Right")
                                                                    : TEXT("HAlign_Left");
        InMain.Props->SetStringField(TEXT("HorizontalAlignment"), Align);
        if (Layout.AlignMain == EAlignMain::SpaceBetween)
        {
          Report(InNode.Id, TEXT("alignApprox"),
                 TEXT("space-between in a wrapping layout is aligned to the start"));
        }
      }
      else if (Layout.AlignMain != EAlignMain::Start)
      {
        Report(InNode.Id, TEXT("alignApprox"),
               TEXT("a vertically wrapping layout has no main-axis alignment in UMG; "
                    "aligned to the start"));
      }
    }
    else if (InNode.Kind == EKind::Shape)
    {
      InMain.Props->SetObjectField(TEXT("Background"), SolidBrush(InNode));
      InMain.Props->SetObjectField(TEXT("Padding"), Margin(0, 0, 0, 0));
    }
    else if (InNode.Kind == EKind::Image && InNode.Image.IsValid())
    {
      InMain.Props->SetObjectField(
          TEXT("Brush"), ImageBrush(InNode, *InNode.Image, MainName, W, H, !InPlan.bFit));
    }
    else if (InNode.Kind == EKind::Text && InNode.Text.IsValid() &&
             !InNode.Text->Runs.IsEmpty())
    {
      const FTextBlock &Text = *InNode.Text;
      const FTextRun &Run = Text.Runs[0];
      if (Text.Runs.Num() > 1)
      {
        Report(InNode.Id, TEXT("textRuns"),
               FString::Printf(TEXT("%d style runs; the first run's style is used"),
                               Text.Runs.Num()));
      }
      InMain.Props->SetStringField(TEXT("Text"), Text.Content);
      TSharedRef<FJsonObject> Font = MakeShared<FJsonObject>();
      const TOptional<FResolvedFont> Resolved = ResolveFont(Run.Font);
      if (Resolved.IsSet())
      {
        Font->SetStringField(TEXT("FontObject"), Resolved->FontObject);
        if (!Resolved->Typeface.IsNone())
        {
          Font->SetStringField(TEXT("TypefaceFontName"), Resolved->Typeface.ToString());
        }
      }
      else
      {
        // The default font stays, in the weight the design asked for.
        const FName Typeface = DefaultTypeface(Run.Font);
        Font->SetStringField(TEXT("TypefaceFontName"), Typeface.ToString());
        Report(InNode.Id, TEXT("fontUnmapped"),
               FString::Printf(TEXT("no UE font for '%s %s' (%s); the default font is "
                                    "used (Roboto %s)"),
                               *Run.Font.Family, *Run.Font.Style, *Run.Font.PostScript,
                               *Typeface.ToString()));
      }
      // UMG font sizes are points at 96 DPI; design pixels are at 72.
      Font->SetField(TEXT("Size"), Num(FMath::RoundToDouble(Run.Size * 0.75 * 100.0) / 100.0));
      Font->SetField(TEXT("LetterSpacing"), Num(FMath::RoundToDouble(Run.Tracking)));
      if (InNode.Stroke.IsValid())
      {
        const int32 OutlineSize = TextOutlineSize(InNode);
        if (OutlineSize > 0)
        {
          TSharedRef<FJsonObject> Outline = MakeShared<FJsonObject>();
          Outline->SetField(TEXT("OutlineSize"), Num(OutlineSize));
          Outline->SetObjectField(TEXT("OutlineColor"), LinearColor(InNode.Stroke->Color));
          // Figma's shadow falls from the stroked glyphs.
          if (TextShadow(InNode))
          {
            Outline->SetBoolField(TEXT("bApplyOutlineToDropShadows"), true);
          }
          Font->SetObjectField(TEXT("OutlineSettings"), Outline);
        }
        if (InNode.Stroke->Align == EStrokeAlign::Inside)
        {
          Report(InNode.Id, TEXT("textStroke"),
                 TEXT("UMG draws text outlines outside the glyphs only; the inside stroke "
                      "was dropped"));
        }
        else if (InNode.Stroke->Align == EStrokeAlign::Center)
        {
          Report(InNode.Id, TEXT("textStroke"),
                 TEXT("UMG draws text outlines outside the glyphs only; the centered "
                      "stroke's outer half is used"));
        }
      }
      InMain.Props->SetObjectField(TEXT("Font"), Font);
      InMain.Props->SetObjectField(TEXT("ColorAndOpacity"), SlateColor(Run.Color));
      if (const FEffect *Shadow = TextShadow(InNode))
      {
        InMain.Props->SetObjectField(TEXT("ShadowOffset"), Vec2(Shadow->Offset.X, Shadow->Offset.Y));
        InMain.Props->SetObjectField(TEXT("ShadowColorAndOpacity"), LinearColor(*Shadow->Color));
      }
      const TCHAR *Justify = Text.Align == ETextAlign::Center  ? TEXT("Center")
                             : Text.Align == ETextAlign::Right ? TEXT("Right")
                                                               : TEXT("Left");
      InMain.Props->SetStringField(TEXT("Justification"), Justify);
      if (Text.Align == ETextAlign::Justify)
      {
        Report(InNode.Id, TEXT("alignApprox"),
               TEXT("justified text is aligned left; UMG's TextBlock can't justify"));
      }
      const double LineHeightPx =
          Run.LineHeightPx.IsSet() ? *Run.LineHeightPx
          : Run.LineHeightPercent.IsSet() ? *Run.LineHeightPercent / 100.0 * Run.Size
                                          : 0.0;
      if (LineHeightPx > 0.0)
      {
        const double Natural = NaturalLineHeight(Run.Font, Run.Size);
        if (Natural > 0.0)
        {
          InMain.Props->SetField(
              TEXT("LineHeightPercentage"),
              Num(FMath::RoundToDouble(LineHeightPx / Natural * 10000.0) / 10000.0));
        }
      }
      if (Text.Sizing != ETextSizing::Auto)
      {
        InMain.Props->SetBoolField(TEXT("AutoWrapText"), true);
      }
    }
  }

  void FConverter::EmitFlowChildren(const FNode &InNode, const FPlan &InPlan,
                                    FWidget &InMain)
  {
    const FLayout &Layout = *InNode.Layout;
    TArray<const FNode *> Flow;
    for (const FNode &Child : InNode.Children)
    {
      if (!Child.bAbsolute)
      {
        Flow.Add(&Child);
      }
    }
    int32 SpacerIndex = 0;
    auto AddSpacer = [&]()
    {
      ++SpacerIndex;
      FWidget Spacer(TEXT("Spacer"),
                     InPlan.Names[FString::Printf(TEXT("spacer%d"), SpacerIndex)]);
      Spacer.Props->SetObjectField(TEXT("Size"), Vec2(0.0, 0.0));
      TSharedRef<FJsonObject> Size = MakeShared<FJsonObject>();
      Size->SetStringField(TEXT("SizeRule"), TEXT("Fill"));
      Size->SetField(TEXT("Value"), Num(1.0));
      Spacer.Slot->SetObjectField(TEXT("Size"), Size);
      InMain.AddChild(Spacer);
    };
    const bool bSpacers = InPlan.Spacers > 0;
    if (bSpacers && Layout.AlignMain != EAlignMain::SpaceBetween)
    {
      AddSpacer();
    }
    for (int32 Index = 0; Index < Flow.Num(); ++Index)
    {
      if (bSpacers && Layout.AlignMain == EAlignMain::SpaceBetween && Index > 0)
      {
        AddSpacer();
      }
      EmitNode(*Flow[Index], EParent::Layout, &Layout, Index == 0, InNode.Box.W,
               InNode.Box.H, 0.0, 0.0, InMain.Children);
    }
    if (bSpacers && Layout.AlignMain == EAlignMain::Center)
    {
      AddSpacer();
    }
  }

  void FConverter::EmitNode(const FNode &InNode, EParent InParent,
                            const FLayout *InParentLayout, bool bInFirstFlow, double InPW,
                            double InPH, double InOX, double InOY,
                            TArray<TSharedPtr<FJsonValue>> &OutSiblings)
  {
    const FPlan &Plan = Plans[PlanIndex[&InNode]];
    Result.Owned.Add(InNode.Id, Plan.Names);

    if (InNode.BlendMode != TEXT("normal"))
    {
      Report(InNode.Id, TEXT("blendMode"),
             FString::Printf(TEXT("blend mode '%s' has no UMG equivalent"),
                             *InNode.BlendMode));
    }
    const FEffect *Shadow = TextShadow(InNode);
    for (const FEffect &Effect : InNode.Effects)
    {
      if (&Effect == Shadow)
      {
        if (Effect.Radius > 0.0 || Effect.Spread != 0.0)
        {
          Report(InNode.Id, TEXT("shadowApprox"),
                 FString::Printf(TEXT("UMG text shadows are sharp; the shadow's blur (%g) and "
                                      "spread (%g) were dropped"),
                                 Effect.Radius, Effect.Spread));
        }
      }
      else if (!Effect.bBaked)
      {
        Report(InNode.Id, TEXT("effectDropped"),
               FString::Printf(TEXT("'%s' effect dropped"), *Effect.Type));
      }
    }
    if (InNode.Rotation != 0.0 && InParent == EParent::Layout)
    {
      Report(InNode.Id, TEXT("rotatedInLayout"),
             TEXT("UMG lays out the unrotated box; the design's auto layout used the "
                  "rotated bounds"));
    }
    if (InNode.Stroke.IsValid())
    {
      const FEdges &Weights = InNode.Stroke->Weights;
      if (Weights.L != Weights.T || Weights.L != Weights.R || Weights.L != Weights.B)
      {
        Report(InNode.Id, TEXT("strokeUneven"),
               TEXT("UMG outlines have one width; the largest weight is used"));
      }
    }

    const FEdges &G = Plan.Growth;
    const double W = InNode.Box.W + G.L + G.R;
    const double H = InNode.Box.H + G.T + G.B;

    // main, with its children.
    const FString MainClass =
        Plan.bChildWbp ? ComponentPaths[InNode.Component->Key] : Plan.MainClass;
    FWidget Main(MainClass, Plan.Names[RoleMain]);
    EmitMainProps(InNode, Plan, Main);
    TOptional<FWidget> Abs;
    if (Plan.bLayers)
    {
      Abs.Emplace(TEXT("CanvasPanel"), Plan.Names[RoleAbs]);
    }
    if (!Plan.bChildWbp)
    {
      if (Plan.bFrameLike && InNode.Layout.IsValid())
      {
        EmitFlowChildren(InNode, Plan, Main);
        for (const FNode &Child : InNode.Children)
        {
          if (Child.bAbsolute)
          {
            EmitNode(Child, EParent::Canvas, nullptr, false, InNode.Box.W, InNode.Box.H,
                     0.0, 0.0, Abs->Children);
          }
        }
      }
      else if (Plan.bCover)
      {
        // Covers the parent canvas, so the children stay in the parent's
        // coordinates and their constraints resolve against the frame.
        for (const FNode &Child : InNode.Children)
        {
          EmitNode(Child, EParent::Canvas, nullptr, false, InPW, InPH,
                   InOX + InNode.Box.X, InOY + InNode.Box.Y, Main.Children);
        }
      }
      else
      {
        for (const FNode &Child : InNode.Children)
        {
          EmitNode(Child, EParent::Canvas, nullptr, false, InNode.Box.W, InNode.Box.H,
                   0.0, 0.0, Main.Children);
        }
      }
    }

    // The chain, from the inside out.
    const FEdges Padding =
        Plan.bFrameLike && InNode.Layout.IsValid() ? InNode.Layout->Padding : FEdges();
    FWidget *Inner = &Main;
    TOptional<FWidget> Scroll;
    if (Plan.bScroll)
    {
      Scroll.Emplace(TEXT("ScrollBox"), Plan.Names[RoleScroll]);
      Scroll->Props->SetStringField(TEXT("Orientation"),
                                    InNode.Layout->Mode == ELayoutMode::Horizontal
                                        ? TEXT("Orient_Horizontal")
                                        : TEXT("Orient_Vertical"));
      Scroll->AddChild(*Inner);
      Inner = &*Scroll;
    }
    TOptional<FWidget> Clip;
    if (Plan.bClip)
    {
      Clip.Emplace(TEXT("Border"), Plan.Names[RoleClip]);
      TSharedRef<FJsonObject> Brush = MakeShared<FJsonObject>();
      Brush->SetStringField(TEXT("DrawAs"), TEXT("NoDrawType"));
      Clip->Props->SetObjectField(TEXT("Background"), Brush);
      Clip->Props->SetObjectField(TEXT("Padding"),
                                  Margin(Padding.L, Padding.T, Padding.R, Padding.B));
      Clip->Props->SetStringField(TEXT("Clipping"), TEXT("ClipToBounds"));
      Clip->AddChild(*Inner);
      Inner = &*Clip;
    }
    TOptional<FWidget> Bg;
    if (Plan.bButton)
    {
      // The frame's look is the Button's normal style, slightly lighter when
      // hovered and darker when pressed; its content fills the Button.
      Bg.Emplace(TEXT("Button"), Plan.Names[RoleButton]);
      const bool bImageFill = InNode.Fill.IsValid() && InNode.Fill->Image.IsValid();
      auto StateBrush = [&](double InShade)
      {
        if (!bImageFill)
        {
          return SolidBrush(InNode, InShade);
        }
        TSharedRef<FJsonObject> Brush = MakeShared<FJsonObject>(
            *ImageBrush(InNode, *InNode.Fill->Image, Plan.Names[RoleMain], W, H, true));
        Brush->SetObjectField(TEXT("TintColor"), SlateColor(Shade(FColor::White, InShade)));
        return Brush;
      };
      TSharedRef<FJsonObject> Style = MakeShared<FJsonObject>();
      Style->SetObjectField(TEXT("Normal"), StateBrush(0.0));
      Style->SetObjectField(TEXT("Hovered"), StateBrush(ButtonHoverShade));
      Style->SetObjectField(TEXT("Pressed"), StateBrush(ButtonPressShade));
      Style->SetObjectField(TEXT("NormalPadding"), Margin(0.0, 0.0, 0.0, 0.0));
      Style->SetObjectField(TEXT("PressedPadding"), Margin(0.0, 0.0, 0.0, 0.0));
      Bg->Props->SetObjectField(TEXT("WidgetStyle"), Style);
      const FEdges Pad = Plan.bClip ? FEdges() : Padding;
      Inner->Slot->SetObjectField(TEXT("Padding"),
                                  Margin(Pad.L + G.L, Pad.T + G.T, Pad.R + G.R, Pad.B + G.B));
      Inner->Slot->SetStringField(TEXT("HorizontalAlignment"), TEXT("HAlign_Fill"));
      Inner->Slot->SetStringField(TEXT("VerticalAlignment"), TEXT("VAlign_Fill"));
      Bg->AddChild(*Inner);
      Inner = &*Bg;
    }
    else if (Plan.bBg)
    {
      Bg.Emplace(TEXT("Border"), Plan.Names[RoleBg]);
      const bool bImageFill = InNode.Fill.IsValid() && InNode.Fill->Image.IsValid();
      Bg->Props->SetObjectField(
          TEXT("Background"),
          bImageFill
              ? ImageBrush(InNode, *InNode.Fill->Image, Plan.Names[RoleMain], W, H, true)
              : SolidBrush(InNode));
      // Explicit even when zero: UBorder defaults to 4 x 2.
      const FEdges Pad = Plan.bClip ? FEdges() : Padding;
      Bg->Props->SetObjectField(TEXT("Padding"),
                                Margin(Pad.L + G.L, Pad.T + G.T, Pad.R + G.R, Pad.B + G.B));
      Bg->AddChild(*Inner);
      Inner = &*Bg;
    }
    TOptional<FWidget> Layers;
    if (Plan.bLayers)
    {
      Layers.Emplace(TEXT("Overlay"), Plan.Names[RoleLayers]);
      // Overlay slots default to left / top.
      Inner->Slot->SetStringField(TEXT("HorizontalAlignment"), TEXT("HAlign_Fill"));
      Inner->Slot->SetStringField(TEXT("VerticalAlignment"), TEXT("VAlign_Fill"));
      Layers->AddChild(*Inner);
      Abs->Slot->SetStringField(TEXT("HorizontalAlignment"), TEXT("HAlign_Fill"));
      Abs->Slot->SetStringField(TEXT("VerticalAlignment"), TEXT("VAlign_Fill"));
      // The Overlay has grown with the stroke; abs stays at the frame's box.
      Abs->Slot->SetObjectField(TEXT("Padding"), Margin(G.L, G.T, G.R, G.B));
      if (InNode.bClip && Plan.bClip)
      {
        Abs->Props->SetStringField(TEXT("Clipping"), TEXT("ClipToBounds"));
      }
      Layers->AddChild(*Abs);
      Inner = &*Layers;
    }
    TOptional<FWidget> Fit;
    if (Plan.bFit)
    {
      Fit.Emplace(TEXT("ScaleBox"), Plan.Names[RoleFit]);
      Fit->Props->SetStringField(TEXT("Stretch"), TEXT("ScaleToFit"));
      Fit->AddChild(*Inner);
      Inner = &*Fit;
    }
    TOptional<FWidget> Size;
    if (Plan.bSize)
    {
      Size.Emplace(TEXT("SizeBox"), Plan.Names[RoleSize]);
      const TSharedRef<FJsonObject> &Props = Size->Props;
      if (Plan.WidthOverride.IsSet())
      {
        Props->SetBoolField(TEXT("bOverride_WidthOverride"), true);
        Props->SetField(TEXT("WidthOverride"), Num(*Plan.WidthOverride));
      }
      if (Plan.HeightOverride.IsSet())
      {
        Props->SetBoolField(TEXT("bOverride_HeightOverride"), true);
        Props->SetField(TEXT("HeightOverride"), Num(*Plan.HeightOverride));
      }
      if (InNode.SizeLimits.IsSet())
      {
        const FSizeLimits &Limits = *InNode.SizeLimits;
        auto Limit = [&Props](const TCHAR *InName, const TOptional<double> &InValue)
        {
          if (InValue.IsSet())
          {
            Props->SetBoolField(FString(TEXT("bOverride_")) + InName, true);
            Props->SetField(InName, Num(*InValue));
          }
        };
        Limit(TEXT("MinDesiredWidth"), Limits.MinW);
        Limit(TEXT("MaxDesiredWidth"), Limits.MaxW);
        Limit(TEXT("MinDesiredHeight"), Limits.MinH);
        Limit(TEXT("MaxDesiredHeight"), Limits.MaxH);
      }
      if (InNode.Kind == EKind::Text && InNode.Text.IsValid() &&
          InNode.Text->Sizing == ETextSizing::Fixed && Inner == &Main)
      {
        const ETextVAlign Align = InNode.Text->VAlign;
        Main.Slot->SetStringField(TEXT("VerticalAlignment"),
                                  Align == ETextVAlign::Center   ? TEXT("VAlign_Center")
                                  : Align == ETextVAlign::Bottom ? TEXT("VAlign_Bottom")
                                                                 : TEXT("VAlign_Top"));
      }
      Size->AddChild(*Inner);
      Inner = &*Size;
    }
    FWidget &Outer = *Inner;

    // Node-level properties on the outermost widget.
    if (InNode.Opacity != 1.0)
    {
      Outer.Props->SetField(TEXT("RenderOpacity"), Num(InNode.Opacity));
    }
    if (!InNode.bVisible)
    {
      Outer.Props->SetStringField(TEXT("Visibility"), TEXT("Collapsed"));
    }
    if (InNode.Rotation != 0.0)
    {
      TSharedRef<FJsonObject> Transform = MakeShared<FJsonObject>();
      Transform->SetField(TEXT("Angle"), Num(InNode.Rotation));
      Outer.Props->SetObjectField(TEXT("RenderTransform"), Transform);
      if (Plan.bCover && InPW > 0.0 && InPH > 0.0)
      {
        // The canvas covers the parent; turn about the group's own center.
        Outer.Props->SetObjectField(
            TEXT("RenderTransformPivot"),
            Vec2((InOX + InNode.Box.X + InNode.Box.W * 0.5) / InPW,
                 (InOY + InNode.Box.Y + InNode.Box.H * 0.5) / InPH));
      }
    }
    if (InNode.bClip && !Plan.bClip)
    {
      Outer.Props->SetStringField(TEXT("Clipping"), TEXT("ClipToBounds"));
    }

    // The slot in the parent.
    if (InParent == EParent::Canvas)
    {
      double Offsets[4] = {0.0, 0.0, 0.0, 0.0};
      double Anchors[4] = {0.0, 0.0, 0.0, 0.0};
      auto Axis = [&](int32 InAxis, double InX, double InLead, double InW, double InP,
                      EConstraint InC)
      {
        const double X = InX - InLead;
        double &Min = Anchors[InAxis];
        double &Max = Anchors[InAxis + 2];
        double &Pos = Offsets[InAxis];
        double &Far = Offsets[InAxis + 2];
        if (InC == EConstraint::Scale && InP <= 0.0)
        {
          InC = EConstraint::Min;
        }
        switch (InC)
        {
        case EConstraint::Max:
          Min = Max = 1.0;
          Pos = X - InP;
          Far = InW;
          break;
        case EConstraint::Center:
          Min = Max = 0.5;
          Pos = X - InP * 0.5;
          Far = InW;
          break;
        case EConstraint::Stretch:
          Min = 0.0;
          Max = 1.0;
          Pos = X;
          Far = InP - (X + InW);
          break;
        case EConstraint::Scale:
          Min = X / InP;
          Max = (X + InW) / InP;
          Pos = 0.0;
          Far = 0.0;
          break;
        default:
          Min = Max = 0.0;
          Pos = X;
          Far = InW;
          break;
        }
      };
      if (Plan.bCover)
      {
        Anchors[2] = Anchors[3] = 1.0;
      }
      else
      {
        const bool bConstrained = InNode.bHasConstraints;
        Axis(0, InOX + InNode.Box.X, G.L, W, InPW,
             bConstrained ? InNode.ConstraintH : EConstraint::Min);
        Axis(1, InOY + InNode.Box.Y, G.T, H, InPH,
             bConstrained ? InNode.ConstraintV : EConstraint::Min);
      }
      TSharedRef<FJsonObject> LayoutData = MakeShared<FJsonObject>();
      LayoutData->SetObjectField(TEXT("Offsets"),
                                 Margin(Offsets[0], Offsets[1], Offsets[2], Offsets[3]));
      TSharedRef<FJsonObject> AnchorsObject = MakeShared<FJsonObject>();
      AnchorsObject->SetObjectField(TEXT("Minimum"), Vec2(Anchors[0], Anchors[1]));
      AnchorsObject->SetObjectField(TEXT("Maximum"), Vec2(Anchors[2], Anchors[3]));
      LayoutData->SetObjectField(TEXT("Anchors"), AnchorsObject);
      Outer.Slot->SetObjectField(TEXT("LayoutData"), LayoutData);
      if (Plan.bAutoSize)
      {
        Outer.Slot->SetBoolField(TEXT("bAutoSize"), true);
      }
    }
    else if (InParent == EParent::Layout && InParentLayout)
    {
      const FLayout &Layout = *InParentLayout;
      const bool bHorizontal = Layout.Mode == ELayoutMode::Horizontal;
      const ESizing MainSizing = bHorizontal ? Plan.SizingH : Plan.SizingV;
      const ESizing CrossSizing = bHorizontal ? Plan.SizingV : Plan.SizingH;
      const EAlignCross Cross =
          CrossSizing == ESizing::Fill ? EAlignCross::Stretch : Layout.AlignCross;
      const TCHAR *CrossField =
          bHorizontal ? TEXT("VerticalAlignment") : TEXT("HorizontalAlignment");
      Outer.Slot->SetStringField(CrossField, bHorizontal ? VAlign(Cross) : HAlign(Cross));
      if (Layout.bWrap)
      {
        Outer.Slot->SetObjectField(TEXT("Padding"), Margin(-G.L, -G.T, -G.R, -G.B));
        if (MainSizing == ESizing::Fill)
        {
          Outer.Slot->SetBoolField(TEXT("bFillEmptySpace"), true);
        }
      }
      else
      {
        const double Lead =
            !bInFirstFlow && Layout.AlignMain != EAlignMain::SpaceBetween ? Layout.Spacing
                                                                          : 0.0;
        Outer.Slot->SetObjectField(
            TEXT("Padding"), Margin((bHorizontal ? Lead : 0.0) - G.L,
                                    (bHorizontal ? 0.0 : Lead) - G.T, -G.R, -G.B));
        TSharedRef<FJsonObject> SizeRule = MakeShared<FJsonObject>();
        SizeRule->SetStringField(TEXT("SizeRule"),
                                 MainSizing == ESizing::Fill ? TEXT("Fill") : TEXT("Automatic"));
        SizeRule->SetField(TEXT("Value"), Num(1.0));
        Outer.Slot->SetObjectField(TEXT("Size"), SizeRule);
      }
    }

    OutSiblings.Add(Obj(Outer.Finish()));
  }

  // --- Components -------------------------------------------------------------

  void CollectIds(const FNode &InNode, TSet<FString> &OutIds)
  {
    OutIds.Add(InNode.Id);
    for (const FNode &Child : InNode.Children)
    {
      CollectIds(Child, OutIds);
    }
  }

  // component.keys used by child-WBP instances inside InNode.
  void CollectKeys(const FNode &InNode, TSet<FString> &OutKeys)
  {
    if (InNode.Kind == EKind::Instance && InNode.Component.IsValid() &&
        !InNode.Component->bHasOverrides)
    {
      OutKeys.Add(InNode.Component->Key);
      return;
    }
    for (const FNode &Child : InNode.Children)
    {
      CollectKeys(Child, OutKeys);
    }
  }

  TSharedPtr<FDocument> FConverter::ComponentTree(const FNode &InRoot,
                                                  bool bInFromInstance,
                                                  const FComponentDef *InDef)
  {
    TSharedRef<FDocument> Tree = MakeShared<FDocument>();
    Tree->Version = 1;
    Tree->Source = Doc.Source;
    Tree->ReferenceSize = Doc.ReferenceSize;
    Tree->RootMode = ERootMode::Widget;
    Tree->Root = InRoot;
    Tree->Root.Box.X = 0.0;
    Tree->Root.Box.Y = 0.0;
    Tree->Root.bAbsolute = false;
    Tree->Root.bHasConstraints = false;
    if (bInFromInstance)
    {
      // The instance's own subtree, drawn as a frame. Its rotation, opacity
      // and visibility are the instance's, set on its slot in the parent.
      Tree->Root.Kind = EKind::Frame;
      Tree->Root.Component.Reset();
      Tree->Root.Rotation = 0.0;
      Tree->Root.Opacity = 1.0;
      Tree->Root.bVisible = true;
    }
    if (InDef)
    {
      TSharedRef<FJsonObject> SourceRef = MakeShared<FJsonObject>();
      SourceRef->SetStringField(TEXT("fileKey"), InDef->FileKey);
      SourceRef->SetStringField(TEXT("nodeId"), InDef->NodeId);
      SourceRef->SetStringField(TEXT("version"), InDef->Version);
      Tree->SourceRef = SourceRef;
    }
    TSet<FString> Ids;
    CollectIds(Tree->Root, Ids);
    for (const FNote &Note : Doc.Notes)
    {
      if (Ids.Contains(Note.Node))
      {
        Tree->Notes.Add(Note);
      }
    }
    // Only the components this subtree reaches, transitively.
    TArray<FString> Pending;
    TSet<FString> Keys;
    CollectKeys(Tree->Root, Keys);
    Pending = Keys.Array();
    while (!Pending.IsEmpty())
    {
      const FString Key = Pending.Pop();
      if (Tree->Components.Contains(Key))
      {
        continue;
      }
      if (const FComponentDef *Def = Doc.Components.Find(Key))
      {
        Tree->Components.Add(Key, *Def);
        TSet<FString> Nested;
        CollectKeys(Def->Root, Nested);
        Pending.Append(Nested.Array());
      }
    }
    return Tree;
  }

  void FConverter::BuildComponentJobs()
  {
    TArray<FString> Keys = ComponentKeys;
    Keys.Sort();
    for (const FString &Key : Keys)
    {
      FComponentJob &Job = Result.Components.AddDefaulted_GetRef();
      Job.Key = Key;
      Job.AssetPath = ComponentPaths[Key];
      const FComponentDef *Def = Doc.Components.Find(Key);
      const FComponentIndexEntry *Entry = Options.ComponentIndex.Find(Key);
      const FNode &Instance = *FirstInstance[Key];
      if (Entry)
      {
        if (Def)
        {
          Job.Hash = Def->Hash;
          Job.Action = !Def->Hash.IsEmpty() && Def->Hash == Entry->Hash
                           ? EComponentAction::Reuse
                           : EComponentAction::Update;
          if (Job.Action == EComponentAction::Update)
          {
            Job.Tree = ComponentTree(Def->Root, false, Def);
          }
        }
        else
        {
          Job.Action = EComponentAction::Reuse;
          Report(Instance.Id, TEXT("componentFallback"),
                 FString::Printf(TEXT("the main component of '%s' couldn't be read; "
                                      "the existing child WBP is kept unchanged"),
                                 *Instance.Component->Name));
        }
      }
      else
      {
        Job.Action = EComponentAction::Create;
        if (Def)
        {
          Job.Hash = Def->Hash;
          Job.Tree = ComponentTree(Def->Root, false, Def);
        }
        else
        {
          Job.Tree = ComponentTree(Instance, true, nullptr);
          Report(Instance.Id, TEXT("componentFallback"),
                 FString::Printf(TEXT("the main component of '%s' couldn't be read; "
                                      "the child WBP is built from this instance"),
                                 *Instance.Component->Name));
        }
      }
    }
  }

  // --- Run ----------------------------------------------------------------------

  FConvertResult FConverter::Run()
  {
    for (const FNote &Note : Doc.Notes)
    {
      Result.Report.Add({Note.Node, Note.Category, Note.Detail});
    }

    const FNode &Root = Doc.Root;
    RootMode = Options.bChildComponent ? ERootMode::Widget : Doc.RootMode;
    if (RootMode == ERootMode::Auto)
    {
      RootMode = Root.Box.W == Doc.ReferenceSize.X && Root.Box.H == Doc.ReferenceSize.Y
                     ? ERootMode::Screen
                     : ERootMode::Widget;
    }
    else if (RootMode == ERootMode::Screen &&
             (Root.Box.W != Doc.ReferenceSize.X || Root.Box.H != Doc.ReferenceSize.Y))
    {
      Report(Root.Id, TEXT("rootSize"),
             FString::Printf(TEXT("the root is %gx%g but the reference size is %gx%g; "
                                  "children are placed as designed"),
                             Root.Box.W, Root.Box.H, Doc.ReferenceSize.X,
                             Doc.ReferenceSize.Y));
    }
    Result.RootMode = RootMode;

    WidgetCount = Collect(Root, EParent::Root, nullptr, 0);
    Result.WidgetCount = WidgetCount;
    Result.MaxDepth = MaxDepthSeen;
    if (WidgetCount > MaxWidgets || MaxDepthSeen > MaxDepth)
    {
      Report(Root.Id, TEXT("limits"),
             FString::Printf(TEXT("the spec would have %d widgets and %d levels; Apply "
                                  "allows %d and %d. Nothing was converted."),
                             WidgetCount, MaxDepthSeen + 1, MaxWidgets, MaxDepth + 1));
      TArray<const FPlan *> Largest;
      for (const FNode &Child : Root.Children)
      {
        Largest.Add(&Plans[PlanIndex[&Child]]);
      }
      Largest.Sort([](const FPlan &A, const FPlan &B)
                   { return A.SubtreeWidgets > B.SubtreeWidgets; });
      for (int32 Index = 0; Index < FMath::Min(5, Largest.Num()); ++Index)
      {
        Report(Largest[Index]->Node->Id, TEXT("limits"),
               FString::Printf(TEXT("'%s' holds %d widgets"), *Largest[Index]->Node->Name,
                               Largest[Index]->SubtreeWidgets));
      }
      return MoveTemp(Result);
    }

    NameWidgets();
    NameComponents();

    for (const TPair<FString, FString> &Pair : Options.Textures)
    {
      FString AssetName;
      Pair.Value.Split(TEXT("."), nullptr, &AssetName, ESearchCase::CaseSensitive,
                       ESearchDir::FromEnd);
      TextureNamesTaken.Add(NameKey(AssetName));
    }
    for (const FString &AssetName : Options.TakenAssetNames)
    {
      TextureNamesTaken.Add(NameKey(AssetName));
    }

    TArray<TSharedPtr<FJsonValue>> RootWidgets;
    EmitNode(Root, EParent::Root, nullptr, true, Root.Box.W, Root.Box.H, 0.0, 0.0,
             RootWidgets);
    TSharedRef<FJsonObject> Spec = MakeShared<FJsonObject>();
    if (!RootWidgets.IsEmpty())
    {
      Spec->SetField(TEXT("root"), RootWidgets[0]);
    }
    Result.Spec = Spec;

    for (const TPair<FString, FString> &Pair : Options.Textures)
    {
      if (!Result.Textures.Contains(Pair.Key))
      {
        Report(FString(), TEXT("textureUnused"),
               FString::Printf(TEXT("%s (%s) is no longer used; it was left in place"),
                               *Pair.Value, *Pair.Key));
      }
    }
    BuildComponentJobs();
    return MoveTemp(Result);
  }
}

FString UIWTDesignTree::SanitizeName(const FString &InName, EKind InKind)
{
  FString Name;
  Name.Reserve(InName.Len());
  bool bLastUnderscore = false;
  for (const TCHAR Char : InName)
  {
    const bool bKeep = (Char >= TEXT('A') && Char <= TEXT('Z')) ||
                       (Char >= TEXT('a') && Char <= TEXT('z')) ||
                       (Char >= TEXT('0') && Char <= TEXT('9'));
    if (bKeep)
    {
      Name.AppendChar(Char);
      bLastUnderscore = false;
    }
    else if (!bLastUnderscore)
    {
      Name.AppendChar(TEXT('_'));
      bLastUnderscore = true;
    }
  }
  while (Name.StartsWith(TEXT("_")))
  {
    Name.RightChopInline(1);
  }
  while (Name.EndsWith(TEXT("_")))
  {
    Name.LeftChopInline(1);
  }
  if (Name.IsEmpty())
  {
    static const TCHAR *const KindNames[] = {TEXT("Frame"), TEXT("Group"), TEXT("Instance"),
                                             TEXT("Text"),  TEXT("Shape"), TEXT("Image")};
    Name = KindNames[static_cast<int32>(InKind)];
  }
  if (FChar::IsDigit(Name[0]))
  {
    Name = TEXT("W_") + Name;
  }
  // Room for the role and hash suffixes within Blueprint name limits.
  if (Name.Len() > 60)
  {
    Name.LeftInline(60);
  }
  return Name;
}

UIWTDesignTree::FConvertResult UIWTDesignTree::Convert(const FDocument &InDocument,
                                                       const FConvertOptions &InOptions)
{
  if (HasLayoutHints(InDocument))
  {
    FDocument Hinted = InDocument;
    ApplyLayoutHints(Hinted);
    return FConverter(Hinted, InOptions).Run();
  }
  return FConverter(InDocument, InOptions).Run();
}

FName UIWTDesignTree::DefaultTypeface(const FFontRef &InFont)
{
  // "Bold Italic", "SemiBold", or a PostScript name like Inter-SemiBoldItalic.
  FString Style = InFont.Style.IsEmpty() ? InFont.PostScript : InFont.Style;
  Style.ToLowerInline();
  const bool bItalic = Style.Contains(TEXT("italic")) || Style.Contains(TEXT("oblique"));
  const bool bBold = Style.Contains(TEXT("bold")) || Style.Contains(TEXT("black")) ||
                     Style.Contains(TEXT("heavy"));
  // "semilight" and "extralight" are light; "semibold" is caught above.
  const bool bLight = !bBold && (Style.Contains(TEXT("light")) || Style.Contains(TEXT("thin")));
  if (bBold)
  {
    return bItalic ? FName(TEXT("Bold Italic")) : FName(TEXT("Bold"));
  }
  if (bItalic)
  {
    return FName(TEXT("Italic"));
  }
  return bLight ? FName(TEXT("Light")) : FName(TEXT("Regular"));
}
