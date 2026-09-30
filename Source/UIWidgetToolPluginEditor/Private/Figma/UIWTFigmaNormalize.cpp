#include "UIWTFigmaNormalize.h"

#include "Core/UIWTRunImages.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Crc.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

// The Figma → design tree mapping (import-figma.md step 2). Units and
// coordinates are the tree's (import-tree.md → Units, Coordinates).
//
// Placement uses each node's absoluteBoundingBox centre, its own size and
// its rotation from relativeTransform (all present with geometry=paths):
// the centre of a rotated box is the centre of its bounding box, so a
// child's box in its tree parent is its centre turned into the parent's
// unrotated frame. This needs no knowledge of which ancestor Figma measures
// relativeTransform from, except for the rotation, which is relative to the
// nearest ancestor that isn't a group or boolean operation.
namespace
{
  using namespace UIWTDesignTree;
  using namespace UIWTFigmaNormalize;

  using FJsonArray = TArray<TSharedPtr<FJsonValue>>;

  // ---------------------------------------------------------------------
  // JSON

  double Num(const FJsonObject &InObject, const TCHAR *InField, double InDefault = 0.0)
  {
    double Value = InDefault;
    return InObject.TryGetNumberField(InField, Value) ? Value : InDefault;
  }

  FString Str(const FJsonObject &InObject, const TCHAR *InField)
  {
    FString Value;
    InObject.TryGetStringField(InField, Value);
    return Value;
  }

  bool Bool(const FJsonObject &InObject, const TCHAR *InField, bool bInDefault)
  {
    bool bValue = bInDefault;
    return InObject.TryGetBoolField(InField, bValue) ? bValue : bInDefault;
  }

  const FJsonObject *Obj(const FJsonObject &InObject, const TCHAR *InField)
  {
    const TSharedPtr<FJsonObject> *Value = nullptr;
    return InObject.TryGetObjectField(InField, Value) && Value->IsValid() ? Value->Get()
                                                                            : nullptr;
  }

  const FJsonArray *Arr(const FJsonObject &InObject, const TCHAR *InField)
  {
    const FJsonArray *Value = nullptr;
    return InObject.TryGetArrayField(InField, Value) ? Value : nullptr;
  }

  // Design pixels to 1/100 px: Figma's floats carry noise (100.0000076).
  double Round2(double InValue)
  {
    return FMath::RoundToDouble(InValue * 100.0) / 100.0;
  }

  // ---------------------------------------------------------------------
  // Colours and paints

  struct FRgba
  {
    double R = 0.0;
    double G = 0.0;
    double B = 0.0;
    double A = 0.0;
  };

  FRgba ColorOf(const FJsonObject *InColor, double InOpacity)
  {
    FRgba Color;
    if (InColor)
    {
      Color.R = Num(*InColor, TEXT("r"));
      Color.G = Num(*InColor, TEXT("g"));
      Color.B = Num(*InColor, TEXT("b"));
      Color.A = Num(*InColor, TEXT("a"), 1.0);
    }
    Color.A *= InOpacity;
    return Color;
  }

  // Source over destination, in sRGB as Figma blends.
  FRgba Over(const FRgba &InDst, const FRgba &InSrc)
  {
    const double A = InSrc.A + InDst.A * (1.0 - InSrc.A);
    if (A <= 0.0)
    {
      return FRgba();
    }
    auto Channel = [&](double InS, double InD)
    { return (InS * InSrc.A + InD * InDst.A * (1.0 - InSrc.A)) / A; };
    return {Channel(InSrc.R, InDst.R), Channel(InSrc.G, InDst.G), Channel(InSrc.B, InDst.B), A};
  }

  FColor ToColor(const FRgba &InColor)
  {
    auto Byte = [](double InValue)
    { return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(InValue * 255.0), 0, 255)); };
    return FColor(Byte(InColor.R), Byte(InColor.G), Byte(InColor.B), Byte(InColor.A));
  }

  bool IsGradient(const FString &InType)
  {
    return InType.StartsWith(TEXT("GRADIENT_"));
  }

  // The mean of a gradient's stops, with the paint's opacity.
  FRgba GradientAverage(const FJsonObject &InPaint)
  {
    FRgba Sum;
    int32 Count = 0;
    if (const FJsonArray *Stops = Arr(InPaint, TEXT("gradientStops")))
    {
      for (const TSharedPtr<FJsonValue> &Stop : *Stops)
      {
        const TSharedPtr<FJsonObject> StopObject = Stop->AsObject();
        if (!StopObject.IsValid())
        {
          continue;
        }
        const FRgba Color = ColorOf(Obj(*StopObject, TEXT("color")), 1.0);
        Sum.R += Color.R;
        Sum.G += Color.G;
        Sum.B += Color.B;
        Sum.A += Color.A;
        ++Count;
      }
    }
    if (Count > 0)
    {
      Sum.R /= Count;
      Sum.G /= Count;
      Sum.B /= Count;
      Sum.A /= Count;
    }
    Sum.A *= Num(InPaint, TEXT("opacity"), 1.0);
    return Sum;
  }

  // The visible paints of a fills or strokes array, bottom first as Figma
  // lists them.
  TArray<const FJsonObject *> VisiblePaints(const FJsonObject &InNode, const TCHAR *InField)
  {
    TArray<const FJsonObject *> Paints;
    if (const FJsonArray *Array = Arr(InNode, InField))
    {
      for (const TSharedPtr<FJsonValue> &Value : *Array)
      {
        const TSharedPtr<FJsonObject> Paint = Value->AsObject();
        if (Paint.IsValid() && Bool(*Paint, TEXT("visible"), true))
        {
          Paints.Add(Paint.Get());
        }
      }
    }
    return Paints;
  }

  bool AllSolid(const TArray<const FJsonObject *> &InPaints)
  {
    for (const FJsonObject *Paint : InPaints)
    {
      if (Str(*Paint, TEXT("type")) != TEXT("SOLID"))
      {
        return false;
      }
    }
    return true;
  }

  // Solid and gradient paints blended into one colour, gradients averaged.
  FRgba BlendPaints(const TArray<const FJsonObject *> &InPaints, bool &bOutAveraged)
  {
    FRgba Color;
    for (const FJsonObject *Paint : InPaints)
    {
      const FString Type = Str(*Paint, TEXT("type"));
      if (Type == TEXT("SOLID"))
      {
        Color = Over(Color, ColorOf(Obj(*Paint, TEXT("color")), Num(*Paint, TEXT("opacity"), 1.0)));
      }
      else if (IsGradient(Type))
      {
        Color = Over(Color, GradientAverage(*Paint));
        bOutAveraged = true;
      }
    }
    return Color;
  }

  // ---------------------------------------------------------------------
  // Placement

  // A node's box in page space: its centre, unrotated size and rotation
  // (radians, counterclockwise as Figma turns).
  struct FPlace
  {
    FVector2D Center = FVector2D::ZeroVector;
    FVector2D Size = FVector2D::ZeroVector;
    double Theta = 0.0;
  };

  bool ReadRect(const FJsonObject &InNode, const TCHAR *InField, FVector2D &OutPos,
                FVector2D &OutSize)
  {
    const FJsonObject *Rect = Obj(InNode, InField);
    if (!Rect)
    {
      return false;
    }
    OutPos = FVector2D(Num(*Rect, TEXT("x")), Num(*Rect, TEXT("y")));
    OutSize = FVector2D(Num(*Rect, TEXT("width")), Num(*Rect, TEXT("height")));
    return true;
  }

  double WrapAngle(double InTheta)
  {
    double Theta = FMath::Fmod(InTheta, UE_DOUBLE_TWO_PI);
    if (Theta > UE_DOUBLE_PI)
    {
      Theta -= UE_DOUBLE_TWO_PI;
    }
    else if (Theta <= -UE_DOUBLE_PI)
    {
      Theta += UE_DOUBLE_TWO_PI;
    }
    return FMath::Abs(Theta) < 1e-6 ? 0.0 : Theta;
  }

  // The rotation relativeTransform holds, and whether it mirrors.
  double RelativeTheta(const FJsonObject &InNode, bool &bOutFlipped)
  {
    bOutFlipped = false;
    const FJsonArray *Rows = Arr(InNode, TEXT("relativeTransform"));
    if (!Rows || Rows->Num() < 2)
    {
      return 0.0;
    }
    const FJsonArray &Row0 = (*Rows)[0]->AsArray();
    const FJsonArray &Row1 = (*Rows)[1]->AsArray();
    if (Row0.Num() < 2 || Row1.Num() < 2)
    {
      return 0.0;
    }
    const double M00 = Row0[0]->AsNumber();
    const double M01 = Row0[1]->AsNumber();
    const double M10 = Row1[0]->AsNumber();
    const double M11 = Row1[1]->AsNumber();
    bOutFlipped = M00 * M11 - M01 * M10 < 0.0;
    return FMath::Atan2(-M10, M00);
  }

  FVector2D SizeFromBounds(const FVector2D &InBounds, double InTheta, bool &bOutApprox)
  {
    const double C = FMath::Abs(FMath::Cos(InTheta));
    const double S = FMath::Abs(FMath::Sin(InTheta));
    const double Det = C * C - S * S;
    if (FMath::Abs(Det) < 0.2)
    {
      // Near 45 degrees the bounds don't tell width from height.
      bOutApprox = true;
      const double Side = FMath::Max(InBounds.X, InBounds.Y) / (C + S);
      return FVector2D(Side, Side);
    }
    return FVector2D((InBounds.X * C - InBounds.Y * S) / Det, (InBounds.Y * C - InBounds.X * S) / Det);
  }

  // InChild's box in InParent's unrotated frame.
  FRect LocalBox(const FPlace &InChild, const FPlace &InParent)
  {
    const FVector2D D = InChild.Center - InParent.Center;
    const double C = FMath::Cos(InParent.Theta);
    const double S = FMath::Sin(InParent.Theta);
    // The parent's axes in page space (y down): x = (C, -S), y = (S, C).
    const FVector2D Center(D.X * C - D.Y * S + InParent.Size.X * 0.5,
                           D.X * S + D.Y * C + InParent.Size.Y * 0.5);
    return {Round2(Center.X - InChild.Size.X * 0.5), Round2(Center.Y - InChild.Size.Y * 0.5),
            Round2(InChild.Size.X), Round2(InChild.Size.Y)};
  }

  // The tree's rotation: degrees clockwise, relative to the parent.
  double TreeRotation(const FPlace &InChild, const FPlace &InParent)
  {
    const double Degrees = -FMath::RadiansToDegrees(WrapAngle(InChild.Theta - InParent.Theta));
    const double Rounded = FMath::RoundToDouble(Degrees * 1000.0) / 1000.0;
    return FMath::Abs(Rounded) < 0.01 ? 0.0 : Rounded;
  }

  // ---------------------------------------------------------------------
  // Enum mappings

  EConstraint ConstraintOf(const FString &InValue)
  {
    if (InValue == TEXT("RIGHT") || InValue == TEXT("BOTTOM"))
    {
      return EConstraint::Max;
    }
    if (InValue == TEXT("LEFT_RIGHT") || InValue == TEXT("TOP_BOTTOM"))
    {
      return EConstraint::Stretch;
    }
    if (InValue == TEXT("CENTER"))
    {
      return EConstraint::Center;
    }
    if (InValue == TEXT("SCALE"))
    {
      return EConstraint::Scale;
    }
    return EConstraint::Min;
  }

  ESizing SizingOf(const FString &InValue)
  {
    return InValue == TEXT("HUG")    ? ESizing::Hug
           : InValue == TEXT("FILL") ? ESizing::Fill
                                     : ESizing::Fixed;
  }

  // MULTIPLY → multiply, COLOR_DODGE → colorDodge; normal for Figma's
  // defaults.
  FString BlendModeOf(const FString &InValue)
  {
    if (InValue.IsEmpty() || InValue == TEXT("NORMAL") || InValue == TEXT("PASS_THROUGH"))
    {
      return TEXT("normal");
    }
    TArray<FString> Parts;
    InValue.ToLower().ParseIntoArray(Parts, TEXT("_"));
    FString Result;
    for (int32 Index = 0; Index < Parts.Num(); ++Index)
    {
      FString Part = Parts[Index];
      if (Index > 0 && !Part.IsEmpty())
      {
        Part[0] = FChar::ToUpper(Part[0]);
      }
      Result += Part;
    }
    return Result;
  }

  FString EffectTypeOf(const FString &InValue)
  {
    return InValue == TEXT("DROP_SHADOW")    ? TEXT("dropShadow")
           : InValue == TEXT("INNER_SHADOW") ? TEXT("innerShadow")
           : InValue == TEXT("LAYER_BLUR") || InValue == TEXT("BACKGROUND_BLUR")
               ? TEXT("blur")
               : TEXT("other");
  }

  // Fields an instance may change on itself without it counting as an
  // override: its size and placement, which the child WBP's slot takes.
  bool IsPlacementField(const FString &InField)
  {
    static const TSet<FString> Fields = {
        TEXT("x"), TEXT("y"), TEXT("width"), TEXT("height"), TEXT("size"),
        TEXT("relativeTransform"), TEXT("rotation"), TEXT("constraints"), TEXT("name"),
        TEXT("layoutSizingHorizontal"), TEXT("layoutSizingVertical"), TEXT("layoutGrow"),
        TEXT("layoutAlign"), TEXT("layoutPositioning"), TEXT("primaryAxisSizingMode"),
        TEXT("counterAxisSizingMode")};
    return Fields.Contains(InField);
  }

  // ---------------------------------------------------------------------
  // The walk

  struct FContext
  {
    // The tree parent's box; null for the root.
    const FPlace *Parent = nullptr;
    // Absolute rotation of the node children measure relativeTransform from.
    double RefTheta = 0.0;
    // The tree parent lays its children out (auto layout), and along which axis.
    bool bAutoLayout = false;
    ELayoutMode ParentMode = ELayoutMode::Horizontal;
    // The node or an ancestor is hidden: Figma renders nothing for it.
    bool bHidden = false;
  };

  enum class EAs : uint8
  {
    Skip,
    Frame,
    Group,
    Instance,
    Text,
    Shape,
    ImageFill,
    Render
  };

  class FNormalizer
  {
  public:
    FNormalizer(const FJsonObject *InComponents, const FJsonObject *InSets,
                const FOptions &InOptions, FResult &InOutResult)
        : Components(InComponents), Sets(InSets), Options(InOptions), Result(InOutResult)
    {
    }

    bool Convert(const FJsonObject &InNode, const FContext &InContext, bool bInRoot,
                 FNode &OutNode);

  private:
    const FJsonObject *Components;
    const FJsonObject *Sets;
    const FOptions &Options;
    FResult &Result;

    void Note(const FString &InNode, const TCHAR *InCategory, const FString &InDetail)
    {
      Result.Document.Notes.Add({InNode, InCategory, InDetail});
    }

    EAs Decide(const FJsonObject &InNode, const FString &InType, const FString &InId,
               const FVector2D &InSize);
    void ContainerFill(const FJsonObject &InNode, const FString &InId, const FVector2D &InSize,
                       FNode &OutNode);
    void Stroke(const FJsonObject &InNode, const FString &InId, FNode &OutNode);
    void Radii(const FJsonObject &InNode, FNode &OutNode);
    void Effects(const FJsonObject &InNode, bool bInBaked, FNode &OutNode);
    void Layout(const FJsonObject &InNode, const FString &InId, FNode &OutNode);
    void Sizing(const FJsonObject &InNode, const FContext &InContext, FNode &OutNode);
    void Text(const FJsonObject &InNode, const FString &InId, FNode &OutNode);
    void Component(const FJsonObject &InNode, const FString &InId, FNode &OutNode);
    TSharedRef<FImageRef> ImageFillJob(const FJsonObject &InPaint, const FString &InId,
                                       const FVector2D &InSize);
    TSharedRef<FImageRef> RenderJob(const FString &InId, bool bInAbsoluteBounds,
                                    const FVector2D &InSize);
  };

  EAs FNormalizer::Decide(const FJsonObject &InNode, const FString &InType, const FString &InId,
                          const FVector2D &InSize)
  {
    if (InType == TEXT("SLICE"))
    {
      return EAs::Skip;
    }
    if (InType == TEXT("FRAME") || InType == TEXT("COMPONENT") ||
        InType == TEXT("COMPONENT_SET") || InType == TEXT("SECTION"))
    {
      return EAs::Frame;
    }
    if (InType == TEXT("INSTANCE"))
    {
      return EAs::Instance;
    }
    if (InType == TEXT("TEXT"))
    {
      return EAs::Text;
    }
    if (InType == TEXT("GROUP"))
    {
      // A mask clips the siblings above it; only a rendered image keeps that.
      if (const FJsonArray *Children = Arr(InNode, TEXT("children")))
      {
        for (const TSharedPtr<FJsonValue> &Child : *Children)
        {
          const TSharedPtr<FJsonObject> ChildObject = Child->AsObject();
          if (ChildObject.IsValid() && Bool(*ChildObject, TEXT("isMask"), false))
          {
            Note(InId, TEXT("rasterized"), TEXT("a group with a mask is rendered as one image"));
            return EAs::Render;
          }
        }
      }
      return EAs::Group;
    }

    // Leaves: drawn by the converter when it can, else rendered.
    const TArray<const FJsonObject *> Fills = VisiblePaints(InNode, TEXT("fills"));
    const TArray<const FJsonObject *> Strokes = VisiblePaints(InNode, TEXT("strokes"));
    bool bEffects = false;
    if (const FJsonArray *Effects = Arr(InNode, TEXT("effects")))
    {
      for (const TSharedPtr<FJsonValue> &Effect : *Effects)
      {
        const TSharedPtr<FJsonObject> EffectObject = Effect->AsObject();
        bEffects |= EffectObject.IsValid() && Bool(*EffectObject, TEXT("visible"), true);
      }
    }
    const bool bSolidStrokes = AllSolid(Strokes);
    if (!bEffects && bSolidStrokes)
    {
      if (InType == TEXT("RECTANGLE"))
      {
        if (AllSolid(Fills))
        {
          return EAs::Shape;
        }
        if (Fills.Num() == 1 && Str(*Fills[0], TEXT("type")) == TEXT("IMAGE"))
        {
          return EAs::ImageFill;
        }
      }
      if (InType == TEXT("ELLIPSE") && AllSolid(Fills) &&
          FMath::IsNearlyEqual(InSize.X, InSize.Y, 0.01))
      {
        const FJsonObject *ArcData = Obj(InNode, TEXT("arcData"));
        const bool bFullCircle =
            !ArcData || (FMath::IsNearlyZero(Num(*ArcData, TEXT("startingAngle")), 1e-4) &&
                         FMath::IsNearlyEqual(Num(*ArcData, TEXT("endingAngle"), UE_DOUBLE_TWO_PI),
                                              UE_DOUBLE_TWO_PI, 1e-4) &&
                         FMath::IsNearlyZero(Num(*ArcData, TEXT("innerRadius")), 1e-4));
        if (bFullCircle)
        {
          return EAs::Shape;
        }
      }
    }
    Note(InId, TEXT("rasterized"),
         FString::Printf(TEXT("%s rendered as an image"), *InType.ToLower()));
    return EAs::Render;
  }

  void FNormalizer::ContainerFill(const FJsonObject &InNode, const FString &InId,
                                  const FVector2D &InSize, FNode &OutNode)
  {
    const TArray<const FJsonObject *> Paints = VisiblePaints(InNode, TEXT("fills"));
    if (Paints.IsEmpty())
    {
      return;
    }
    TSharedRef<FFill> Fill = MakeShared<FFill>();
    const FJsonObject *TopImage = nullptr;
    for (const FJsonObject *Paint : Paints)
    {
      if (Str(*Paint, TEXT("type")) == TEXT("IMAGE"))
      {
        TopImage = Paint;
      }
    }
    if (TopImage)
    {
      Fill->Image = ImageFillJob(*TopImage, InId, InSize);
      if (Paints.Num() > 1)
      {
        Note(InId, TEXT("fillsFlattened"),
             FString::Printf(TEXT("%d fills; only the top image fill is kept"), Paints.Num()));
      }
    }
    else
    {
      bool bAveraged = false;
      Fill->Color = ToColor(BlendPaints(Paints, bAveraged));
      if (bAveraged)
      {
        Note(InId, TEXT("gradientAveraged"),
             TEXT("a gradient fill on a container was reduced to its average colour"));
      }
      for (const FJsonObject *Paint : Paints)
      {
        const FString Type = Str(*Paint, TEXT("type"));
        if (Type != TEXT("SOLID") && !IsGradient(Type))
        {
          Note(InId, TEXT("fillUnsupported"),
               FString::Printf(TEXT("a %s fill was dropped"), *Type.ToLower()));
        }
      }
    }
    OutNode.Fill = Fill;
  }

  // Leaves get here only with solid strokes (Decide renders the others), so
  // gradients and image strokes only reach containers.
  void FNormalizer::Stroke(const FJsonObject &InNode, const FString &InId, FNode &OutNode)
  {
    const TArray<const FJsonObject *> Paints = VisiblePaints(InNode, TEXT("strokes"));
    if (Paints.IsEmpty())
    {
      return;
    }
    TSharedRef<FStroke> Block = MakeShared<FStroke>();
    FEdges Weights;
    if (const FJsonObject *Individual = Obj(InNode, TEXT("individualStrokeWeights")))
    {
      Weights = {Num(*Individual, TEXT("left")), Num(*Individual, TEXT("top")),
                 Num(*Individual, TEXT("right")), Num(*Individual, TEXT("bottom"))};
    }
    else
    {
      const double Weight = Num(InNode, TEXT("strokeWeight"), 1.0);
      Weights = {Weight, Weight, Weight, Weight};
    }
    if (Weights.IsZero())
    {
      return;
    }
    Block->Weights = Weights;
    const FString Align = Str(InNode, TEXT("strokeAlign"));
    Block->Align = Align == TEXT("CENTER")    ? EStrokeAlign::Center
                   : Align == TEXT("OUTSIDE") ? EStrokeAlign::Outside
                                              : EStrokeAlign::Inside;
    bool bAveraged = false;
    Block->Color = ToColor(BlendPaints(Paints, bAveraged));
    if (bAveraged)
    {
      Note(InId, TEXT("gradientAveraged"), TEXT("a gradient stroke was reduced to its average colour"));
    }
    for (const FJsonObject *Paint : Paints)
    {
      const FString Type = Str(*Paint, TEXT("type"));
      if (Type != TEXT("SOLID") && !IsGradient(Type))
      {
        Note(InId, TEXT("strokeUnsupported"),
             FString::Printf(TEXT("a %s stroke was dropped"), *Type.ToLower()));
      }
    }
    const FJsonArray *Dashes = Arr(InNode, TEXT("strokeDashes"));
    if (Dashes && !Dashes->IsEmpty())
    {
      Note(InId, TEXT("strokeDashes"), TEXT("a dashed stroke is drawn solid"));
    }
    OutNode.Stroke = Block;
  }

  void FNormalizer::Radii(const FJsonObject &InNode, FNode &OutNode)
  {
    if (const FJsonArray *Corners = Arr(InNode, TEXT("rectangleCornerRadii")))
    {
      if (Corners->Num() == 4)
      {
        const FVector4 Radii((*Corners)[0]->AsNumber(), (*Corners)[1]->AsNumber(),
                             (*Corners)[2]->AsNumber(), (*Corners)[3]->AsNumber());
        if (Radii.X > 0 || Radii.Y > 0 || Radii.Z > 0 || Radii.W > 0)
        {
          OutNode.Radii = Radii;
        }
        return;
      }
    }
    const double Radius = Num(InNode, TEXT("cornerRadius"));
    if (Radius > 0.0)
    {
      OutNode.Radii = FVector4(Radius, Radius, Radius, Radius);
    }
  }

  void FNormalizer::Effects(const FJsonObject &InNode, bool bInBaked, FNode &OutNode)
  {
    if (const FJsonArray *Effects = Arr(InNode, TEXT("effects")))
    {
      for (const TSharedPtr<FJsonValue> &Value : *Effects)
      {
        const TSharedPtr<FJsonObject> Effect = Value->AsObject();
        if (Effect.IsValid() && Bool(*Effect, TEXT("visible"), true))
        {
          OutNode.Effects.Add({EffectTypeOf(Str(*Effect, TEXT("type"))), bInBaked});
        }
      }
    }
  }

  void FNormalizer::Layout(const FJsonObject &InNode, const FString &InId, FNode &OutNode)
  {
    const FString Mode = Str(InNode, TEXT("layoutMode"));
    if (Mode == TEXT("GRID"))
    {
      Note(InId, TEXT("gridLayout"), TEXT("grid auto layout is imported as absolute positions"));
      return;
    }
    if (Mode != TEXT("HORIZONTAL") && Mode != TEXT("VERTICAL"))
    {
      return;
    }
    TSharedRef<FLayout> Layout = MakeShared<FLayout>();
    Layout->Mode = Mode == TEXT("VERTICAL") ? ELayoutMode::Vertical : ELayoutMode::Horizontal;
    Layout->bWrap = Str(InNode, TEXT("layoutWrap")) == TEXT("WRAP");
    Layout->Spacing = Round2(Num(InNode, TEXT("itemSpacing")));
    Layout->CrossSpacing = Round2(Num(InNode, TEXT("counterAxisSpacing")));
    Layout->Padding = {Round2(Num(InNode, TEXT("paddingLeft"))), Round2(Num(InNode, TEXT("paddingTop"))),
                       Round2(Num(InNode, TEXT("paddingRight"))),
                       Round2(Num(InNode, TEXT("paddingBottom")))};
    const FString Main = Str(InNode, TEXT("primaryAxisAlignItems"));
    Layout->AlignMain = Main == TEXT("CENTER")          ? EAlignMain::Center
                        : Main == TEXT("MAX")           ? EAlignMain::End
                        : Main == TEXT("SPACE_BETWEEN") ? EAlignMain::SpaceBetween
                                                        : EAlignMain::Start;
    const FString Cross = Str(InNode, TEXT("counterAxisAlignItems"));
    Layout->AlignCross = Cross == TEXT("CENTER") ? EAlignCross::Center
                         : Cross == TEXT("MAX")  ? EAlignCross::End
                                                 : EAlignCross::Start;
    if (Cross == TEXT("BASELINE"))
    {
      Note(InId, TEXT("alignBaseline"), TEXT("baseline alignment is imported as top alignment"));
    }
    OutNode.Layout = Layout;
  }

  void FNormalizer::Sizing(const FJsonObject &InNode, const FContext &InContext, FNode &OutNode)
  {
    FString Horizontal = Str(InNode, TEXT("layoutSizingHorizontal"));
    FString Vertical = Str(InNode, TEXT("layoutSizingVertical"));
    // Files older than layoutSizing* carry the same in the legacy fields.
    const bool bFlow = InContext.bAutoLayout && !OutNode.bAbsolute;
    if (Horizontal.IsEmpty() || Vertical.IsEmpty())
    {
      const bool bParentHorizontal = InContext.ParentMode == ELayoutMode::Horizontal;
      FString LegacyH;
      FString LegacyV;
      if (bFlow)
      {
        const bool bGrow = Num(InNode, TEXT("layoutGrow")) >= 1.0;
        const bool bStretch = Str(InNode, TEXT("layoutAlign")) == TEXT("STRETCH");
        LegacyH = (bParentHorizontal ? bGrow : bStretch) ? TEXT("FILL") : TEXT("");
        LegacyV = (bParentHorizontal ? bStretch : bGrow) ? TEXT("FILL") : TEXT("");
      }
      if (OutNode.Layout.IsValid())
      {
        const bool bHorizontal = OutNode.Layout->Mode == ELayoutMode::Horizontal;
        const bool bMainAuto = Str(InNode, TEXT("primaryAxisSizingMode")) == TEXT("AUTO");
        const bool bCrossAuto = Str(InNode, TEXT("counterAxisSizingMode")) == TEXT("AUTO");
        if ((bHorizontal ? bMainAuto : bCrossAuto) && LegacyH.IsEmpty())
        {
          LegacyH = TEXT("HUG");
        }
        if ((bHorizontal ? bCrossAuto : bMainAuto) && LegacyV.IsEmpty())
        {
          LegacyV = TEXT("HUG");
        }
      }
      Horizontal = Horizontal.IsEmpty() ? LegacyH : Horizontal;
      Vertical = Vertical.IsEmpty() ? LegacyV : Vertical;
    }
    OutNode.SizingH = SizingOf(Horizontal);
    OutNode.SizingV = SizingOf(Vertical);

    const double MinW = Num(InNode, TEXT("minWidth"), -1.0);
    const double MaxW = Num(InNode, TEXT("maxWidth"), -1.0);
    const double MinH = Num(InNode, TEXT("minHeight"), -1.0);
    const double MaxH = Num(InNode, TEXT("maxHeight"), -1.0);
    if (MinW >= 0.0 || MaxW >= 0.0 || MinH >= 0.0 || MaxH >= 0.0)
    {
      FSizeLimits Limits;
      if (MinW >= 0.0)
      {
        Limits.MinW = Round2(MinW);
      }
      if (MaxW >= 0.0)
      {
        Limits.MaxW = Round2(MaxW);
      }
      if (MinH >= 0.0)
      {
        Limits.MinH = Round2(MinH);
      }
      if (MaxH >= 0.0)
      {
        Limits.MaxH = Round2(MaxH);
      }
      OutNode.SizeLimits = Limits;
    }
  }

  // One style run's values, for comparing neighbouring runs.
  bool SameRun(const FTextRun &InA, const FTextRun &InB)
  {
    return InA.Font.Family == InB.Font.Family && InA.Font.Style == InB.Font.Style &&
           InA.Font.PostScript == InB.Font.PostScript && InA.Size == InB.Size &&
           InA.Color == InB.Color && InA.Tracking == InB.Tracking &&
           InA.LineHeightPx == InB.LineHeightPx && InA.LineHeightPercent == InB.LineHeightPercent;
  }

  void FNormalizer::Text(const FJsonObject &InNode, const FString &InId, FNode &OutNode)
  {
    TSharedRef<FTextBlock> Text = MakeShared<FTextBlock>();
    Text->Content = Str(InNode, TEXT("characters"));
    // Figma's soft line break.
    Text->Content.ReplaceInline(TEXT(" "), TEXT("\n"));
    static const FJsonObject Empty;
    const FJsonObject &Base = Obj(InNode, TEXT("style")) ? *Obj(InNode, TEXT("style")) : Empty;

    const FString Case = Str(Base, TEXT("textCase"));
    if (Case == TEXT("UPPER"))
    {
      Text->Content.ToUpperInline();
    }
    else if (Case == TEXT("LOWER"))
    {
      Text->Content.ToLowerInline();
    }
    else if (Case == TEXT("TITLE"))
    {
      bool bStart = true;
      for (TCHAR &Char : Text->Content)
      {
        Char = bStart ? FChar::ToUpper(Char) : Char;
        bStart = FChar::IsWhitespace(Char);
      }
    }
    else if (!Case.IsEmpty() && Case != TEXT("ORIGINAL"))
    {
      Note(InId, TEXT("textCase"), TEXT("small caps are imported as the original casing"));
    }

    const FString Align = Str(Base, TEXT("textAlignHorizontal"));
    Text->Align = Align == TEXT("CENTER")      ? ETextAlign::Center
                  : Align == TEXT("RIGHT")     ? ETextAlign::Right
                  : Align == TEXT("JUSTIFIED") ? ETextAlign::Justify
                                               : ETextAlign::Left;
    const FString VAlign = Str(Base, TEXT("textAlignVertical"));
    Text->VAlign = VAlign == TEXT("CENTER")   ? ETextVAlign::Center
                   : VAlign == TEXT("BOTTOM") ? ETextVAlign::Bottom
                                              : ETextVAlign::Top;
    const FString Resize = Str(Base, TEXT("textAutoResize"));
    Text->Sizing = Resize == TEXT("WIDTH_AND_HEIGHT") ? ETextSizing::Auto
                   : Resize == TEXT("HEIGHT")         ? ETextSizing::FixedWidth
                                                      : ETextSizing::Fixed;
    if (Resize == TEXT("TRUNCATE") || Str(Base, TEXT("textTruncation")) == TEXT("ENDING"))
    {
      Note(InId, TEXT("textTruncate"), TEXT("truncation with an ellipsis isn't imported"));
    }

    // Runs: characterStyleOverrides gives a style id per character (0 = the
    // node's style, and the array may stop early).
    TArray<int32> Overrides;
    if (const FJsonArray *Array = Arr(InNode, TEXT("characterStyleOverrides")))
    {
      for (const TSharedPtr<FJsonValue> &Value : *Array)
      {
        Overrides.Add(static_cast<int32>(Value->AsNumber()));
      }
    }
    const FJsonObject *Table = Obj(InNode, TEXT("styleOverrideTable"));
    const TArray<const FJsonObject *> NodeFills = VisiblePaints(InNode, TEXT("fills"));

    auto MakeRun = [&](int32 InStyleId, int32 InStart, int32 InLen)
    {
      // The override's fields replace the node style's.
      TSharedRef<FJsonObject> Style = MakeShared<FJsonObject>();
      Style->Values = Base.Values;
      const FJsonObject *Override =
          InStyleId != 0 && Table ? Obj(*Table, *FString::FromInt(InStyleId)) : nullptr;
      if (Override)
      {
        for (const auto &Pair : Override->Values)
        {
          Style->Values.Add(Pair.Key, Pair.Value);
        }
      }
      FTextRun Run;
      Run.Start = InStart;
      Run.Len = InLen;
      Run.Font.Family = Str(*Style, TEXT("fontFamily"));
      Run.Font.Style = Str(*Style, TEXT("fontStyle"));
      Run.Font.PostScript = Str(*Style, TEXT("fontPostScriptName"));
      Run.Size = Round2(Num(*Style, TEXT("fontSize"), 12.0));
      const double LetterSpacing = Num(*Style, TEXT("letterSpacing"));
      Run.Tracking = Run.Size > 0.0 ? FMath::RoundToDouble(LetterSpacing / Run.Size * 1000.0) : 0.0;
      const FString Unit = Str(*Style, TEXT("lineHeightUnit"));
      const double Percent = Num(*Style, TEXT("lineHeightPercent"), 100.0);
      if (Unit == TEXT("PIXELS"))
      {
        Run.LineHeightPx = Round2(Num(*Style, TEXT("lineHeightPx")));
      }
      else if (Unit == TEXT("FONT_SIZE_%") || (Unit.IsEmpty() && Style->HasField(TEXT("lineHeightPercentFontSize"))))
      {
        Run.LineHeightPercent = Round2(Num(*Style, TEXT("lineHeightPercentFontSize"), 100.0));
      }
      else if (!FMath::IsNearlyEqual(Percent, 100.0, 0.01) && Style->HasField(TEXT("lineHeightPx")))
      {
        // A percentage of the font's own line height: only the pixels say it.
        Run.LineHeightPx = Round2(Num(*Style, TEXT("lineHeightPx")));
      }
      const TArray<const FJsonObject *> Fills =
          Override && Override->HasField(TEXT("fills")) ? VisiblePaints(*Override, TEXT("fills"))
                                                         : NodeFills;
      bool bAveraged = false;
      Run.Color = ToColor(BlendPaints(Fills, bAveraged));
      if (bAveraged || !AllSolid(Fills))
      {
        Note(InId, TEXT("textFill"), TEXT("a text fill that isn't a solid colour was reduced to one colour"));
      }
      return Run;
    };

    const int32 Length = Text->Content.Len();
    int32 Start = 0;
    int32 CurrentId = Length > 0 && Overrides.Num() > 0 ? Overrides[0] : 0;
    for (int32 Index = 1; Index <= Length; ++Index)
    {
      const int32 Id = Index < Length && Index < Overrides.Num() ? Overrides[Index] : 0;
      if (Index == Length || Id != CurrentId)
      {
        FTextRun Run = MakeRun(CurrentId, Start, Index - Start);
        if (!Text->Runs.IsEmpty() && SameRun(Text->Runs.Last(), Run))
        {
          Text->Runs.Last().Len += Run.Len;
        }
        else
        {
          Text->Runs.Add(MoveTemp(Run));
        }
        Start = Index;
        CurrentId = Id;
      }
    }
    if (Text->Runs.IsEmpty())
    {
      Text->Runs.Add(MakeRun(0, 0, 0));
    }
    OutNode.Text = Text;
  }

  void FNormalizer::Component(const FJsonObject &InNode, const FString &InId, FNode &OutNode)
  {
    const FString ComponentId = Str(InNode, TEXT("componentId"));
    const FJsonObject *Definition = Components ? Obj(*Components, *ComponentId) : nullptr;
    TSharedRef<FComponentRef> Component = MakeShared<FComponentRef>();
    Component->Key = Definition ? Str(*Definition, TEXT("key")) : FString();
    if (Component->Key.IsEmpty())
    {
      Component->Key = TEXT("local:") + ComponentId;
    }
    if (!Result.ComponentNodeIds.Contains(Component->Key))
    {
      Result.ComponentNodeIds.Add(Component->Key, ComponentId);
    }
    Component->Name = Definition ? Str(*Definition, TEXT("name")) : OutNode.Name;
    const FString SetId = Definition ? Str(*Definition, TEXT("componentSetId")) : FString();
    const FJsonObject *Set = !SetId.IsEmpty() && Sets ? Obj(*Sets, *SetId) : nullptr;
    if (Set)
    {
      // Variants of different sets often share their values (State=Hover).
      const FString SetName = Str(*Set, TEXT("name"));
      OutNode.Hints.Add(TEXT("componentSet:") + SetName);
      OutNode.Hints.Add(TEXT("variant:") + Component->Name);
      Component->Name = SetName + TEXT(" ") + Component->Name;
    }
    if (const FJsonArray *Overrides = Arr(InNode, TEXT("overrides")))
    {
      for (const TSharedPtr<FJsonValue> &Value : *Overrides)
      {
        const TSharedPtr<FJsonObject> Override = Value->AsObject();
        if (!Override.IsValid())
        {
          continue;
        }
        if (Str(*Override, TEXT("id")) != InId)
        {
          Component->bHasOverrides = true;
          break;
        }
        if (const FJsonArray *Fields = Arr(*Override, TEXT("overriddenFields")))
        {
          for (const TSharedPtr<FJsonValue> &Field : *Fields)
          {
            Component->bHasOverrides |= !IsPlacementField(Field->AsString());
          }
        }
      }
    }
    OutNode.Component = Component;
  }

  TSharedRef<FImageRef> FNormalizer::ImageFillJob(const FJsonObject &InPaint, const FString &InId,
                                                  const FVector2D &InSize)
  {
    FImageJob Job;
    Job.FileKey = Options.FileKey;
    Job.NodeId = InId;
    Job.ImageRef = Str(InPaint, TEXT("imageRef"));
    Job.ScaleMode = Str(InPaint, TEXT("scaleMode"));
    Job.BoxSize = InSize;
    TSharedRef<FImageRef> Image = MakeShared<FImageRef>();
    Image->Origin = EImageOrigin::Fill;
    const FString Base = TEXT("images/") + Job.ImageRef;
    if (Job.ScaleMode == TEXT("FIT"))
    {
      Image->Mode = EImageMode::Fit;
      Image->Path = Base + TEXT(".png");
    }
    else if (Job.ScaleMode == TEXT("TILE"))
    {
      Image->Mode = EImageMode::Tile;
      Image->Path = Base + TEXT(".png");
      Job.ScalingFactor = FMath::Max(Num(InPaint, TEXT("scalingFactor"), 1.0), 0.001);
      // Figma draws each tile at scalingFactor times the image's pixels.
      Image->Scale = FMath::RoundToDouble(1000.0 / Job.ScalingFactor) / 1000.0;
    }
    else if (Job.ScaleMode == TEXT("STRETCH"))
    {
      // Figma's CROP: imageTransform picks the part of the image shown.
      FString Key;
      if (const FJsonArray *Rows = Arr(InPaint, TEXT("imageTransform")))
      {
        for (const TSharedPtr<FJsonValue> &Row : *Rows)
        {
          for (const TSharedPtr<FJsonValue> &Value : Row->AsArray())
          {
            Job.Transform.Add(Value->AsNumber());
            Key += FString::Printf(TEXT("%.5f,"), Value->AsNumber());
          }
        }
      }
      Image->Path = FString::Printf(TEXT("%s_crop_%08x.png"), *Base, FCrc::StrCrc32(*Key));
    }
    else
    {
      // FILL: the image covers the box, centred; cropped to the box's shape.
      Job.ScaleMode = TEXT("FILL");
      const double Aspect = InSize.Y > 0.0 ? InSize.X / InSize.Y : 1.0;
      Image->Path = FString::Printf(TEXT("%s_fill_%d.png"), *Base,
                                    FMath::RoundToInt(Aspect * 1000.0));
    }
    if (!FMath::IsNearlyZero(Num(InPaint, TEXT("rotation"))))
    {
      Note(InId, TEXT("imageRotation"), TEXT("a rotated image fill is imported unrotated"));
    }
    if (Obj(InPaint, TEXT("filters")))
    {
      Note(InId, TEXT("imageFilters"), TEXT("image adjustments (exposure, contrast...) are dropped"));
    }
    Job.Image = Image;
    Result.Images.Add(MoveTemp(Job));
    return Image;
  }

  TSharedRef<FImageRef> FNormalizer::RenderJob(const FString &InId, bool bInAbsoluteBounds,
                                               const FVector2D &InSize)
  {
    FImageJob Job;
    Job.FileKey = Options.FileKey;
    Job.NodeId = InId;
    Job.RenderId = InId;
    Job.bAbsoluteBounds = bInAbsoluteBounds;
    Job.BoxSize = InSize;
    TSharedRef<FImageRef> Image = MakeShared<FImageRef>();
    Image->Origin = EImageOrigin::Rendered;
    Image->Path = RenderFile(InId, Options.RenderFileKey);
    Image->Scale = Options.RenderScale;
    Job.Image = Image;
    Result.Images.Add(MoveTemp(Job));
    return Image;
  }

  bool FNormalizer::Convert(const FJsonObject &InNode, const FContext &InContext, bool bInRoot,
                            FNode &OutNode)
  {
    const FString Type = Str(InNode, TEXT("type"));
    const FString Id = Str(InNode, TEXT("id"));
    const bool bVisible = Bool(InNode, TEXT("visible"), true);
    const bool bHidden = InContext.bHidden || !bVisible;
    if (Bool(InNode, TEXT("isMask"), false))
    {
      if (!bInRoot)
      {
        Note(Id, TEXT("maskDropped"),
             TEXT("a mask directly in a frame is dropped; the layers it masked show whole"));
        return false;
      }
    }

    bool bFlipped = false;
    bool bApprox = false;
    FPlace Place;
    FVector2D BoundsPos;
    FVector2D BoundsSize;
    if (!ReadRect(InNode, TEXT("absoluteBoundingBox"), BoundsPos, BoundsSize))
    {
      Note(Id, TEXT("nodeSkipped"), FString::Printf(TEXT("%s has no bounds"), *Type.ToLower()));
      return false;
    }
    Place.Center = BoundsPos + BoundsSize * 0.5;
    Place.Theta = WrapAngle(InContext.RefTheta + RelativeTheta(InNode, bFlipped));
    if (const FJsonObject *Size = Obj(InNode, TEXT("size")))
    {
      Place.Size = FVector2D(Num(*Size, TEXT("x")), Num(*Size, TEXT("y")));
    }
    else
    {
      Place.Size = Place.Theta == 0.0 ? BoundsSize : SizeFromBounds(BoundsSize, Place.Theta, bApprox);
    }

    EAs As = Decide(InNode, Type, Id, Place.Size);
    if (As == EAs::Skip)
    {
      return false;
    }
    if (bInRoot && As == EAs::Group)
    {
      // A group has no box of its own to be a root in.
      As = EAs::Frame;
    }
    if (As == EAs::Render && bHidden)
    {
      Note(Id, TEXT("hiddenSkipped"), TEXT("a hidden layer that would need rendering is left out"));
      return false;
    }
    ++Result.NodeCount;

    OutNode.Id = Id;
    OutNode.Name = Str(InNode, TEXT("name"));
    OutNode.bVisible = bVisible;
    OutNode.Opacity = FMath::RoundToDouble(Num(InNode, TEXT("opacity"), 1.0) * 1000.0) / 1000.0;
    OutNode.BlendMode = BlendModeOf(Str(InNode, TEXT("blendMode")));
    OutNode.bAbsolute = Str(InNode, TEXT("layoutPositioning")) == TEXT("ABSOLUTE");

    // Placement. A rendered image covers the layer's render bounds (effects
    // included), page-aligned; one an auto layout places keeps the layer's
    // box, since the layout sizes it by that.
    const bool bFlow = InContext.bAutoLayout && !OutNode.bAbsolute;
    if (As == EAs::Render)
    {
      FVector2D RenderPos = BoundsPos;
      FVector2D RenderSize = BoundsSize;
      FVector2D Pos;
      FVector2D Size;
      if (ReadRect(InNode, TEXT("absoluteRenderBounds"), Pos, Size))
      {
        if (!bFlow)
        {
          RenderPos = Pos;
          RenderSize = Size;
        }
        else if (Pos.X < BoundsPos.X - 0.5 || Pos.Y < BoundsPos.Y - 0.5 ||
                 Pos.X + Size.X > BoundsPos.X + BoundsSize.X + 0.5 ||
                 Pos.Y + Size.Y > BoundsPos.Y + BoundsSize.Y + 0.5)
        {
          Note(Id, TEXT("renderClipped"),
               TEXT("effects or strokes outside the layer are cut off, since the auto "
                    "layout sizes it by its box"));
        }
      }
      Place.Center = RenderPos + RenderSize * 0.5;
      Place.Size = RenderSize;
      Place.Theta = 0.0;
      bFlipped = false;
      bApprox = false;
    }
    if (bFlipped)
    {
      Note(Id, TEXT("flipped"), TEXT("a mirrored layer is imported unmirrored"));
    }
    if (bApprox)
    {
      Note(Id, TEXT("rotationApprox"),
           TEXT("the layer's size was estimated from its rotated bounds"));
    }
    if (InContext.Parent)
    {
      OutNode.Box = LocalBox(Place, *InContext.Parent);
      OutNode.Rotation = TreeRotation(Place, *InContext.Parent);
    }
    else
    {
      OutNode.Box = {0.0, 0.0, Round2(Place.Size.X), Round2(Place.Size.Y)};
      if (Place.Theta != 0.0)
      {
        Note(Id, TEXT("rootRotation"), TEXT("the frame's own rotation is ignored"));
      }
    }
    if (!bInRoot && !bFlow)
    {
      if (const FJsonObject *Constraints = Obj(InNode, TEXT("constraints")))
      {
        OutNode.ConstraintH = ConstraintOf(Str(*Constraints, TEXT("horizontal")));
        OutNode.ConstraintV = ConstraintOf(Str(*Constraints, TEXT("vertical")));
        OutNode.bHasConstraints =
            OutNode.ConstraintH != EConstraint::Min || OutNode.ConstraintV != EConstraint::Min;
      }
    }

    // Contents.
    switch (As)
    {
    case EAs::Frame:
    case EAs::Instance:
      OutNode.Kind = As == EAs::Instance ? EKind::Instance : EKind::Frame;
      OutNode.bClip = Bool(InNode, TEXT("clipsContent"), false);
      Layout(InNode, Id, OutNode);
      ContainerFill(InNode, Id, Place.Size, OutNode);
      Stroke(InNode, Id, OutNode);
      Radii(InNode, OutNode);
      Effects(InNode, false, OutNode);
      if (As == EAs::Instance)
      {
        Component(InNode, Id, OutNode);
      }
      break;
    case EAs::Group:
      OutNode.Kind = EKind::Group;
      Effects(InNode, false, OutNode);
      break;
    case EAs::Text:
      OutNode.Kind = EKind::Text;
      Text(InNode, Id, OutNode);
      Effects(InNode, false, OutNode);
      break;
    case EAs::Shape:
    {
      OutNode.Kind = EKind::Shape;
      const TArray<const FJsonObject *> Fills = VisiblePaints(InNode, TEXT("fills"));
      if (!Fills.IsEmpty())
      {
        bool bAveraged = false;
        TSharedRef<FFill> Fill = MakeShared<FFill>();
        Fill->Color = ToColor(BlendPaints(Fills, bAveraged));
        OutNode.Fill = Fill;
        if (Fills.Num() > 1)
        {
          Note(Id, TEXT("fillsFlattened"),
               FString::Printf(TEXT("%d solid fills blended into one colour"), Fills.Num()));
        }
      }
      Stroke(InNode, Id, OutNode);
      if (Type == TEXT("ELLIPSE"))
      {
        const double Radius = Round2(Place.Size.X * 0.5);
        OutNode.Radii = FVector4(Radius, Radius, Radius, Radius);
      }
      else
      {
        Radii(InNode, OutNode);
      }
      break;
    }
    case EAs::ImageFill:
    {
      OutNode.Kind = EKind::Image;
      const FJsonObject &Paint = *VisiblePaints(InNode, TEXT("fills"))[0];
      // The paint's opacity is the image's; with one fill that is the node's.
      OutNode.Opacity = FMath::RoundToDouble(
                            OutNode.Opacity * Num(Paint, TEXT("opacity"), 1.0) * 1000.0) /
                        1000.0;
      OutNode.Image = ImageFillJob(Paint, Id, Place.Size);
      Stroke(InNode, Id, OutNode);
      Radii(InNode, OutNode);
      break;
    }
    case EAs::Render:
      OutNode.Kind = EKind::Image;
      OutNode.Image = RenderJob(Id, bFlow, Place.Size);
      Effects(InNode, true, OutNode);
      break;
    default:
      break;
    }
    Sizing(InNode, InContext, OutNode);

    // Children, in the tree parent's frame. Figma measures a group's
    // children's relativeTransform from the group's own reference.
    if (As == EAs::Frame || As == EAs::Instance || As == EAs::Group)
    {
      FContext Child;
      Child.Parent = &Place;
      Child.RefTheta = Type == TEXT("GROUP") ? InContext.RefTheta : Place.Theta;
      Child.bAutoLayout = OutNode.Layout.IsValid();
      Child.ParentMode = OutNode.Layout.IsValid() ? OutNode.Layout->Mode : ELayoutMode::Horizontal;
      Child.bHidden = bHidden;
      if (const FJsonArray *Children = Arr(InNode, TEXT("children")))
      {
        OutNode.Children.Reserve(Children->Num());
        for (const TSharedPtr<FJsonValue> &Value : *Children)
        {
          const TSharedPtr<FJsonObject> ChildObject = Value->AsObject();
          FNode ChildNode;
          if (ChildObject.IsValid() && Convert(*ChildObject, Child, false, ChildNode))
          {
            OutNode.Children.Add(MoveTemp(ChildNode));
          }
        }
      }
    }
    return true;
  }

  bool WritePng(const FImage &InImage, const FString &InPath, FString &OutError)
  {
    return UIWTRunImages::SavePng(InImage, InPath, OutError);
  }
}

// ---------------------------------------------------------------------------

FString UIWTFigmaNormalize::FileSafeId(const FString &InNodeId)
{
  return InNodeId.Replace(TEXT(":"), TEXT("-")).Replace(TEXT(";"), TEXT("_"));
}

FString UIWTFigmaNormalize::RenderFile(const FString &InNodeId, const FString &InFileKey)
{
  return InFileKey.IsEmpty() ? TEXT("render/") + FileSafeId(InNodeId) + TEXT(".png")
                             : TEXT("render/") + InFileKey / FileSafeId(InNodeId) + TEXT(".png");
}

FString UIWTFigmaNormalize::SourceImageFile(const FString &InImageRef)
{
  return TEXT("images/src/") + InImageRef;
}

FString UIWTFigmaNormalize::ReferenceRenderFile()
{
  return TEXT("reference_render.png");
}

bool UIWTFigmaNormalize::Normalize(const TSharedRef<FJsonObject> &InNodesResponse,
                                   const FOptions &InOptions, FResult &OutResult,
                                   FString &OutError)
{
  OutResult = FResult();
  const FJsonObject *Nodes = Obj(*InNodesResponse, TEXT("nodes"));
  const FJsonObject *Entry = Nodes ? Obj(*Nodes, *InOptions.NodeId) : nullptr;
  const FJsonObject *Root = Entry ? Obj(*Entry, TEXT("document")) : nullptr;
  if (!Root)
  {
    OutError = FString::Printf(TEXT("The file has no node %s (or the token can't see it)."),
                               *InOptions.NodeId);
    return false;
  }
  const FString RootType = Str(*Root, TEXT("type"));
  if (RootType == TEXT("DOCUMENT") || RootType == TEXT("CANVAS"))
  {
    OutError = TEXT("The link points at a page, not a frame. Select the frame in Figma and "
                    "copy its link (right-click → Copy link to selection).");
    return false;
  }

  FDocument &Doc = OutResult.Document;
  Doc.Version = 1;
  Doc.Source = TEXT("figma");
  TSharedRef<FJsonObject> SourceRef = MakeShared<FJsonObject>();
  SourceRef->SetStringField(TEXT("fileKey"), InOptions.FileKey);
  SourceRef->SetStringField(TEXT("nodeId"), InOptions.NodeId);
  if (!InOptions.FileName.IsEmpty())
  {
    SourceRef->SetStringField(TEXT("fileName"), InOptions.FileName);
  }
  if (!InOptions.Version.IsEmpty())
  {
    SourceRef->SetStringField(TEXT("version"), InOptions.Version);
  }
  if (!InOptions.LastModified.IsEmpty())
  {
    SourceRef->SetStringField(TEXT("lastModified"), InOptions.LastModified);
  }
  Doc.SourceRef = SourceRef;
  Doc.ReferenceSize = FVector2D(InOptions.ReferenceSize.X, InOptions.ReferenceSize.Y);

  FNormalizer Normalizer(Obj(*Entry, TEXT("components")), Obj(*Entry, TEXT("componentSets")),
                         InOptions, OutResult);
  if (!Normalizer.Convert(*Root, FContext(), true, Doc.Root))
  {
    OutError = FString::Printf(TEXT("Node %s can't be imported (it has no bounds)."),
                               *InOptions.NodeId);
    return false;
  }
  return true;
}

bool UIWTFigmaNormalize::NormalizeComponent(const TSharedRef<FJsonObject> &InNodesResponse,
                                            const FOptions &InOptions, FResult &InOutResult,
                                            FNode &OutRoot, FString &OutError)
{
  const FJsonObject *Nodes = Obj(*InNodesResponse, TEXT("nodes"));
  const FJsonObject *Entry = Nodes ? Obj(*Nodes, *InOptions.NodeId) : nullptr;
  const FJsonObject *Root = Entry ? Obj(*Entry, TEXT("document")) : nullptr;
  if (!Root)
  {
    OutError = FString::Printf(TEXT("file %s has no node %s (or the token can't see it)"),
                               *InOptions.FileKey, *InOptions.NodeId);
    return false;
  }
  FNormalizer Normalizer(Obj(*Entry, TEXT("components")), Obj(*Entry, TEXT("componentSets")),
                         InOptions, InOutResult);
  if (!Normalizer.Convert(*Root, FContext(), true, OutRoot))
  {
    OutError = FString::Printf(TEXT("node %s can't be imported (it has no bounds)"),
                               *InOptions.NodeId);
    return false;
  }
  return true;
}

void UIWTFigmaNormalize::CollectComponentKeys(const FNode &InRoot, TArray<FString> &OutKeys,
                                              TMap<FString, FString> *OutNames)
{
  if (InRoot.Kind == EKind::Instance && InRoot.Component.IsValid() &&
      !InRoot.Component->bHasOverrides)
  {
    if (!OutKeys.Contains(InRoot.Component->Key))
    {
      OutKeys.Add(InRoot.Component->Key);
      if (OutNames)
      {
        OutNames->Add(InRoot.Component->Key, InRoot.Component->Name);
      }
    }
    return;
  }
  for (const FNode &Child : InRoot.Children)
  {
    CollectComponentKeys(Child, OutKeys, OutNames);
  }
}

namespace
{
  void CollectImagePaths(const FNode &InNode, TSet<FString> &OutPaths)
  {
    if (InNode.Image.IsValid())
    {
      OutPaths.Add(InNode.Image->Path);
    }
    if (InNode.Fill.IsValid() && InNode.Fill->Image.IsValid())
    {
      OutPaths.Add(InNode.Fill->Image->Path);
    }
    for (const FNode &Child : InNode.Children)
    {
      CollectImagePaths(Child, OutPaths);
    }
  }
}

FString UIWTFigmaNormalize::HashComponent(const FNode &InRoot, const FString &InDirectory)
{
  TSet<FString> PathSet;
  CollectImagePaths(InRoot, PathSet);
  TArray<FString> Paths = PathSet.Array();
  Paths.Sort();
  FString Pixels;
  for (const FString &Path : Paths)
  {
    const FMD5Hash FileHash = FMD5Hash::HashFile(*(InDirectory / Path));
    Pixels += Path + TEXT("=") + (FileHash.IsValid() ? LexToString(FileHash) : TEXT("missing")) +
              TEXT(";");
  }
  return HashNode(InRoot, Pixels);
}

void UIWTFigmaNormalize::FinishImages(FResult &InOutResult, const FString &InDirectory,
                                      const FOptions &InOptions)
{
  TMap<FString, FIntPoint> Written;
  for (FImageJob &Job : InOutResult.Images)
  {
    FImageRef &Image = *Job.Image;
    const FVector2D Box(FMath::Max(Job.BoxSize.X, 0.01), FMath::Max(Job.BoxSize.Y, 0.01));
    auto SetSize = [&](const FIntPoint &InSize)
    {
      Image.Size = FVector2D(InSize.X, InSize.Y);
      double Scale = Image.Scale;
      if (!Job.RenderId.IsEmpty() || Image.Mode == EImageMode::Stretch)
      {
        Scale = InSize.X / Box.X;
      }
      else if (Image.Mode == EImageMode::Fit)
      {
        Scale = FMath::Max(InSize.X / Box.X, InSize.Y / Box.Y);
      }
      Image.Scale = FMath::RoundToDouble(Scale * 1000.0) / 1000.0;
    };
    if (const FIntPoint *Size = Written.Find(Image.Path))
    {
      SetSize(*Size);
      continue;
    }

    // A render is downloaded straight to its texture's path.
    const FString Source =
        InDirectory / (Job.RenderId.IsEmpty() ? SourceImageFile(Job.ImageRef) : Image.Path);
    FImage Pixels;
    FString Error;
    if (!UIWTRunImages::LoadImageFile(Source, Pixels, Error))
    {
      InOutResult.Document.Notes.Add(
          {Job.NodeId, TEXT("imageMissing"),
           Job.RenderId.IsEmpty()
               ? FString::Printf(TEXT("image fill %s couldn't be read"), *Job.ImageRef)
               : TEXT("the rendered image couldn't be read")});
      continue;
    }
    if (!Job.RenderId.IsEmpty())
    {
      // The render is the texture as it is.
      Written.Add(Image.Path, FIntPoint(Pixels.SizeX, Pixels.SizeY));
      SetSize(Written[Image.Path]);
      continue;
    }

    // The part of the image shown, in its pixels.
    FIntRect Crop(0, 0, Pixels.SizeX, Pixels.SizeY);
    if (Job.ScaleMode == TEXT("FILL"))
    {
      const double Aspect = Box.X / Box.Y;
      if (Pixels.SizeX > Pixels.SizeY * Aspect)
      {
        const int32 Width = FMath::Clamp(FMath::RoundToInt(Pixels.SizeY * Aspect), 1, Pixels.SizeX);
        Crop.Min.X = (Pixels.SizeX - Width) / 2;
        Crop.Max.X = Crop.Min.X + Width;
      }
      else
      {
        const int32 Height = FMath::Clamp(FMath::RoundToInt(Pixels.SizeX / Aspect), 1, Pixels.SizeY);
        Crop.Min.Y = (Pixels.SizeY - Height) / 2;
        Crop.Max.Y = Crop.Min.Y + Height;
      }
    }
    else if (Job.ScaleMode == TEXT("STRETCH") && Job.Transform.Num() == 6)
    {
      const double A = Job.Transform[0];
      const double B = Job.Transform[1];
      const double Tx = Job.Transform[2];
      const double C = Job.Transform[3];
      const double D = Job.Transform[4];
      const double Ty = Job.Transform[5];
      if (!FMath::IsNearlyZero(B, 1e-4) || !FMath::IsNearlyZero(C, 1e-4))
      {
        InOutResult.Document.Notes.Add(
            {Job.NodeId, TEXT("imageRotation"), TEXT("a rotated image crop is imported unrotated")});
      }
      const double X0 = FMath::Clamp(FMath::Min(Tx, Tx + A), 0.0, 1.0);
      const double X1 = FMath::Clamp(FMath::Max(Tx, Tx + A), 0.0, 1.0);
      const double Y0 = FMath::Clamp(FMath::Min(Ty, Ty + D), 0.0, 1.0);
      const double Y1 = FMath::Clamp(FMath::Max(Ty, Ty + D), 0.0, 1.0);
      Crop = FIntRect(FMath::FloorToInt(X0 * Pixels.SizeX), FMath::FloorToInt(Y0 * Pixels.SizeY),
                      FMath::Max(FMath::CeilToInt(X1 * Pixels.SizeX), FMath::FloorToInt(X0 * Pixels.SizeX) + 1),
                      FMath::Max(FMath::CeilToInt(Y1 * Pixels.SizeY), FMath::FloorToInt(Y0 * Pixels.SizeY) + 1));
      Crop.Max.X = FMath::Min(Crop.Max.X, Pixels.SizeX);
      Crop.Max.Y = FMath::Min(Crop.Max.Y, Pixels.SizeY);
    }

    // Oversized photos come down to MaxImageScale texture pixels per design
    // pixel; tiles keep their pixels.
    FIntPoint Output(Crop.Width(), Crop.Height());
    if (Image.Mode != EImageMode::Tile)
    {
      const double Scale = FMath::Max(Output.X / Box.X, Output.Y / Box.Y);
      if (Scale > InOptions.MaxImageScale)
      {
        const double Factor = InOptions.MaxImageScale / Scale;
        Output = FIntPoint(FMath::Max(1, FMath::RoundToInt(Output.X * Factor)),
                           FMath::Max(1, FMath::RoundToInt(Output.Y * Factor)));
      }
    }
    FImage Texture;
    bool bMade = true;
    if (Crop == FIntRect(0, 0, Pixels.SizeX, Pixels.SizeY) &&
        Output == FIntPoint(Pixels.SizeX, Pixels.SizeY))
    {
      Texture = MoveTemp(Pixels);
    }
    else
    {
      bMade = UIWTRunImages::CropForTexture(Pixels, Crop, UIWTRunImages::ECropMode::Copy,
                                            UIWTRunImages::ECropShape::Rect, 0, Output, Texture,
                                            Error);
    }
    if (!bMade || !WritePng(Texture, InDirectory / Image.Path, Error))
    {
      InOutResult.Document.Notes.Add({Job.NodeId, TEXT("imageMissing"), Error});
      continue;
    }
    Written.Add(Image.Path, FIntPoint(Texture.SizeX, Texture.SizeY));
    SetSize(Written[Image.Path]);
  }

  // reference.png at design pixels, for render compares and Claude runs.
  FImage Render;
  FString Error;
  if (UIWTRunImages::LoadImageFile(InDirectory / ReferenceRenderFile(), Render, Error))
  {
    const FIntPoint Size(FMath::Max(1, FMath::RoundToInt(Render.SizeX / InOptions.RenderScale)),
                         FMath::Max(1, FMath::RoundToInt(Render.SizeY / InOptions.RenderScale)));
    FImage Reference;
    if (UIWTRunImages::CropForTexture(Render, FIntRect(0, 0, Render.SizeX, Render.SizeY),
                                      UIWTRunImages::ECropMode::Copy, UIWTRunImages::ECropShape::Rect,
                                      0, Size, Reference, Error))
    {
      WritePng(Reference, InDirectory / TEXT("reference.png"), Error);
    }
  }
}
