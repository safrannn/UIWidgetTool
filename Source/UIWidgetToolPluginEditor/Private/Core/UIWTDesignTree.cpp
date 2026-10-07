#include "UIWTDesignTree.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/SecureHash.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// design.json reading and writing (import-tree.md → Design tree schema).
// Reading fills typed structs once, so the converter never looks fields up
// in FJsonObject maps. Writing leaves every default out.
namespace
{
  using namespace UIWTDesignTree;

  // ---------------------------------------------------------------------
  // Enum names

  template <typename EnumType, int32 N>
  bool ParseEnum(const FString &InText, const TCHAR *const (&InNames)[N],
                 EnumType &OutValue)
  {
    for (int32 Index = 0; Index < N; ++Index)
    {
      if (InText.Equals(InNames[Index], ESearchCase::CaseSensitive))
      {
        OutValue = static_cast<EnumType>(Index);
        return true;
      }
    }
    return false;
  }

  template <typename EnumType, int32 N>
  const TCHAR *EnumName(EnumType InValue, const TCHAR *const (&InNames)[N])
  {
    const int32 Index = static_cast<int32>(InValue);
    return Index >= 0 && Index < N ? InNames[Index] : TEXT("");
  }

  const TCHAR *const KindNames[] = {TEXT("frame"), TEXT("group"), TEXT("instance"),
                                    TEXT("text"), TEXT("shape"), TEXT("image")};
  const TCHAR *const ConstraintHNames[] = {TEXT("left"), TEXT("right"),
                                           TEXT("leftRight"), TEXT("center"),
                                           TEXT("scale")};
  const TCHAR *const ConstraintVNames[] = {TEXT("top"), TEXT("bottom"),
                                           TEXT("topBottom"), TEXT("center"),
                                           TEXT("scale")};
  const TCHAR *const SizingNames[] = {TEXT("fixed"), TEXT("hug"), TEXT("fill")};
  const TCHAR *const LayoutModeNames[] = {TEXT("horizontal"), TEXT("vertical")};
  const TCHAR *const AlignMainNames[] = {TEXT("start"), TEXT("center"), TEXT("end"),
                                         TEXT("spaceBetween")};
  const TCHAR *const AlignCrossNames[] = {TEXT("start"), TEXT("center"), TEXT("end"),
                                          TEXT("stretch")};
  const TCHAR *const StrokeAlignNames[] = {TEXT("inside"), TEXT("center"),
                                           TEXT("outside")};
  const TCHAR *const TextAlignNames[] = {TEXT("left"), TEXT("center"), TEXT("right"),
                                         TEXT("justify")};
  const TCHAR *const TextVAlignNames[] = {TEXT("top"), TEXT("center"), TEXT("bottom")};
  const TCHAR *const TextSizingNames[] = {TEXT("auto"), TEXT("fixedWidth"),
                                          TEXT("fixed")};
  const TCHAR *const ImageModeNames[] = {TEXT("stretch"), TEXT("fit"), TEXT("tile")};
  const TCHAR *const ImageOriginNames[] = {TEXT("fill"), TEXT("rendered")};
  const TCHAR *const RootModeNames[] = {TEXT(""), TEXT("screen"), TEXT("widget")};

  // ---------------------------------------------------------------------
  // Reading

  // Collects errors with the id of the node being read.
  struct FReader
  {
    TArray<FString> &Errors;
    FString Where;

    void Error(const FString &InMessage)
    {
      Errors.Add(Where.IsEmpty() ? InMessage : Where + TEXT(": ") + InMessage);
    }

    bool Number(const TSharedPtr<FJsonObject> &InObject, const TCHAR *InField,
                double &OutValue, bool bRequired = false)
    {
      if (!InObject->HasField(InField))
      {
        if (bRequired)
        {
          Error(FString::Printf(TEXT("\"%s\" is required"), InField));
        }
        return false;
      }
      if (!InObject->TryGetNumberField(InField, OutValue))
      {
        Error(FString::Printf(TEXT("\"%s\" must be a number"), InField));
        return false;
      }
      return true;
    }

    template <typename EnumType, int32 N>
    void Enum(const TSharedPtr<FJsonObject> &InObject, const TCHAR *InField,
              const TCHAR *const (&InNames)[N], EnumType &OutValue)
    {
      FString Text;
      if (!InObject->TryGetStringField(InField, Text))
      {
        return;
      }
      if (!ParseEnum(Text, InNames, OutValue))
      {
        Error(FString::Printf(TEXT("unknown %s '%s'"), InField, *Text));
      }
    }

    // "#RRGGBB" or "#RRGGBBAA", sRGB.
    bool Color(const TSharedPtr<FJsonObject> &InObject, const TCHAR *InField,
               FColor &OutColor)
    {
      FString Text;
      if (!InObject->TryGetStringField(InField, Text))
      {
        return false;
      }
      auto Hex = [](TCHAR C) -> int32
      {
        if (C >= TEXT('0') && C <= TEXT('9'))
        {
          return C - TEXT('0');
        }
        if (C >= TEXT('a') && C <= TEXT('f'))
        {
          return C - TEXT('a') + 10;
        }
        if (C >= TEXT('A') && C <= TEXT('F'))
        {
          return C - TEXT('A') + 10;
        }
        return -1;
      };
      if (Text.StartsWith(TEXT("#")) && (Text.Len() == 7 || Text.Len() == 9))
      {
        uint8 Channels[4] = {0, 0, 0, 255};
        bool bValid = true;
        for (int32 Index = 0; Index < (Text.Len() - 1) / 2; ++Index)
        {
          const int32 High = Hex(Text[1 + Index * 2]);
          const int32 Low = Hex(Text[2 + Index * 2]);
          bValid &= High >= 0 && Low >= 0;
          Channels[Index] = static_cast<uint8>(High * 16 + Low);
        }
        if (bValid)
        {
          OutColor = FColor(Channels[0], Channels[1], Channels[2], Channels[3]);
          return true;
        }
      }
      Error(FString::Printf(TEXT("\"%s\" must be \"#RRGGBB\" or \"#RRGGBBAA\""),
                            InField));
      return false;
    }

    FEdges Edges(const TSharedPtr<FJsonObject> &InObject)
    {
      FEdges Result;
      Number(InObject, TEXT("l"), Result.L);
      Number(InObject, TEXT("t"), Result.T);
      Number(InObject, TEXT("r"), Result.R);
      Number(InObject, TEXT("b"), Result.B);
      return Result;
    }

    TSharedPtr<const FImageRef> Image(const TSharedPtr<FJsonObject> &InObject)
    {
      TSharedRef<FImageRef> Result = MakeShared<FImageRef>();
      if (!InObject->TryGetStringField(TEXT("path"), Result->Path) ||
          Result->Path.IsEmpty())
      {
        Error(TEXT("an image needs a \"path\""));
      }
      Number(InObject, TEXT("scale"), Result->Scale);
      if (Result->Scale <= 0.0)
      {
        Error(TEXT("\"scale\" must be positive"));
        Result->Scale = 1.0;
      }
      Enum(InObject, TEXT("origin"), ImageOriginNames, Result->Origin);
      Enum(InObject, TEXT("mode"), ImageModeNames, Result->Mode);
      const TSharedPtr<FJsonObject> *Size = nullptr;
      if (InObject->TryGetObjectField(TEXT("size"), Size))
      {
        double W = 0.0;
        double H = 0.0;
        if (Number(*Size, TEXT("w"), W, true) & Number(*Size, TEXT("h"), H, true))
        {
          Result->Size = FVector2D(W, H);
        }
      }
      return Result;
    }

    bool Node(const TSharedPtr<FJsonObject> &InObject, FNode &OutNode,
              int32 InDepth)
    {
      if (InDepth > 256)
      {
        Error(TEXT("the tree is nested more than 256 levels deep"));
        return false;
      }
      if (!InObject->TryGetStringField(TEXT("id"), OutNode.Id) || OutNode.Id.IsEmpty())
      {
        Error(TEXT("a node needs an \"id\""));
      }
      const FString SavedWhere = Where;
      Where = OutNode.Id;
      InObject->TryGetStringField(TEXT("name"), OutNode.Name);

      FString KindText;
      if (!InObject->TryGetStringField(TEXT("kind"), KindText) ||
          !ParseEnum(KindText, KindNames, OutNode.Kind))
      {
        Error(FString::Printf(TEXT("\"kind\" must be one of frame, group, "
                                   "instance, text, shape, image (got '%s')"),
                              *KindText));
      }

      const TSharedPtr<FJsonObject> *Box = nullptr;
      if (InObject->TryGetObjectField(TEXT("box"), Box))
      {
        Number(*Box, TEXT("x"), OutNode.Box.X, true);
        Number(*Box, TEXT("y"), OutNode.Box.Y, true);
        Number(*Box, TEXT("w"), OutNode.Box.W, true);
        Number(*Box, TEXT("h"), OutNode.Box.H, true);
      }
      else
      {
        Error(TEXT("\"box\" is required"));
      }

      Number(InObject, TEXT("rotation"), OutNode.Rotation);
      Number(InObject, TEXT("opacity"), OutNode.Opacity);
      InObject->TryGetBoolField(TEXT("visible"), OutNode.bVisible);
      InObject->TryGetStringField(TEXT("blendMode"), OutNode.BlendMode);
      InObject->TryGetBoolField(TEXT("clip"), OutNode.bClip);
      InObject->TryGetBoolField(TEXT("absolute"), OutNode.bAbsolute);

      const TSharedPtr<FJsonObject> *Constraints = nullptr;
      if (InObject->TryGetObjectField(TEXT("constraints"), Constraints))
      {
        OutNode.bHasConstraints = true;
        Enum(*Constraints, TEXT("h"), ConstraintHNames, OutNode.ConstraintH);
        Enum(*Constraints, TEXT("v"), ConstraintVNames, OutNode.ConstraintV);
      }

      const TSharedPtr<FJsonObject> *Sizing = nullptr;
      if (InObject->TryGetObjectField(TEXT("sizing"), Sizing))
      {
        Enum(*Sizing, TEXT("h"), SizingNames, OutNode.SizingH);
        Enum(*Sizing, TEXT("v"), SizingNames, OutNode.SizingV);
      }

      const TSharedPtr<FJsonObject> *Limits = nullptr;
      if (InObject->TryGetObjectField(TEXT("sizeLimits"), Limits))
      {
        FSizeLimits &SizeLimits = OutNode.SizeLimits.Emplace();
        double Value = 0.0;
        if (Number(*Limits, TEXT("minW"), Value))
        {
          SizeLimits.MinW = Value;
        }
        if (Number(*Limits, TEXT("maxW"), Value))
        {
          SizeLimits.MaxW = Value;
        }
        if (Number(*Limits, TEXT("minH"), Value))
        {
          SizeLimits.MinH = Value;
        }
        if (Number(*Limits, TEXT("maxH"), Value))
        {
          SizeLimits.MaxH = Value;
        }
      }

      const TSharedPtr<FJsonObject> *LayoutObject = nullptr;
      if (InObject->TryGetObjectField(TEXT("layout"), LayoutObject))
      {
        TSharedRef<FLayout> Layout = MakeShared<FLayout>();
        FString Mode;
        if (!(*LayoutObject)->TryGetStringField(TEXT("mode"), Mode) ||
            !ParseEnum(Mode, LayoutModeNames, Layout->Mode))
        {
          Error(FString::Printf(TEXT("layout \"mode\" must be horizontal or "
                                     "vertical (got '%s')"),
                                *Mode));
        }
        (*LayoutObject)->TryGetBoolField(TEXT("wrap"), Layout->bWrap);
        Number(*LayoutObject, TEXT("spacing"), Layout->Spacing);
        Number(*LayoutObject, TEXT("crossSpacing"), Layout->CrossSpacing);
        const TSharedPtr<FJsonObject> *Padding = nullptr;
        if ((*LayoutObject)->TryGetObjectField(TEXT("padding"), Padding))
        {
          Layout->Padding = Edges(*Padding);
        }
        const TSharedPtr<FJsonObject> *Align = nullptr;
        if ((*LayoutObject)->TryGetObjectField(TEXT("align"), Align))
        {
          Enum(*Align, TEXT("main"), AlignMainNames, Layout->AlignMain);
          Enum(*Align, TEXT("cross"), AlignCrossNames, Layout->AlignCross);
        }
        if (OutNode.Kind != EKind::Frame && OutNode.Kind != EKind::Instance)
        {
          Error(TEXT("only frames can have a \"layout\""));
        }
        OutNode.Layout = Layout;
      }

      const TSharedPtr<FJsonObject> *FillObject = nullptr;
      if (InObject->TryGetObjectField(TEXT("fill"), FillObject))
      {
        TSharedRef<FFill> Fill = MakeShared<FFill>();
        FColor FillColor;
        if (Color(*FillObject, TEXT("color"), FillColor))
        {
          Fill->Color = FillColor;
        }
        const TSharedPtr<FJsonObject> *ImageObject = nullptr;
        if ((*FillObject)->TryGetObjectField(TEXT("image"), ImageObject))
        {
          Fill->Image = Image(*ImageObject);
        }
        if (Fill->Color.IsSet() && Fill->Image.IsValid())
        {
          Error(TEXT("a fill is either a color or an image"));
        }
        OutNode.Fill = Fill;
      }

      const TSharedPtr<FJsonObject> *StrokeObject = nullptr;
      if (InObject->TryGetObjectField(TEXT("stroke"), StrokeObject))
      {
        TSharedRef<FStroke> Stroke = MakeShared<FStroke>();
        Color(*StrokeObject, TEXT("color"), Stroke->Color);
        const TSharedPtr<FJsonObject> *Weights = nullptr;
        if ((*StrokeObject)->TryGetObjectField(TEXT("weights"), Weights))
        {
          Stroke->Weights = Edges(*Weights);
        }
        Enum(*StrokeObject, TEXT("align"), StrokeAlignNames, Stroke->Align);
        OutNode.Stroke = Stroke;
      }

      const TArray<TSharedPtr<FJsonValue>> *Radii = nullptr;
      if (InObject->TryGetArrayField(TEXT("radii"), Radii))
      {
        double Values[4] = {0.0, 0.0, 0.0, 0.0};
        if (Radii->Num() != 4)
        {
          Error(TEXT("\"radii\" must have 4 numbers: tl, tr, br, bl"));
        }
        for (int32 Index = 0; Index < FMath::Min(4, Radii->Num()); ++Index)
        {
          (*Radii)[Index]->TryGetNumber(Values[Index]);
        }
        OutNode.Radii = FVector4(Values[0], Values[1], Values[2], Values[3]);
      }

      const TArray<TSharedPtr<FJsonValue>> *Effects = nullptr;
      if (InObject->TryGetArrayField(TEXT("effects"), Effects))
      {
        for (const TSharedPtr<FJsonValue> &Value : *Effects)
        {
          const TSharedPtr<FJsonObject> *EffectObject = nullptr;
          if (Value->TryGetObject(EffectObject))
          {
            FEffect &Effect = OutNode.Effects.AddDefaulted_GetRef();
            (*EffectObject)->TryGetStringField(TEXT("type"), Effect.Type);
            (*EffectObject)->TryGetBoolField(TEXT("baked"), Effect.bBaked);
            FColor EffectColor;
            if (Color(*EffectObject, TEXT("color"), EffectColor))
            {
              Effect.Color = EffectColor;
            }
            const TSharedPtr<FJsonObject> *Offset = nullptr;
            if ((*EffectObject)->TryGetObjectField(TEXT("offset"), Offset))
            {
              (*Offset)->TryGetNumberField(TEXT("x"), Effect.Offset.X);
              (*Offset)->TryGetNumberField(TEXT("y"), Effect.Offset.Y);
            }
            (*EffectObject)->TryGetNumberField(TEXT("radius"), Effect.Radius);
            (*EffectObject)->TryGetNumberField(TEXT("spread"), Effect.Spread);
          }
        }
      }

      const TSharedPtr<FJsonObject> *TextObject = nullptr;
      if (InObject->TryGetObjectField(TEXT("text"), TextObject))
      {
        TSharedRef<FTextBlock> Text = MakeShared<FTextBlock>();
        (*TextObject)->TryGetStringField(TEXT("content"), Text->Content);
        Enum(*TextObject, TEXT("align"), TextAlignNames, Text->Align);
        Enum(*TextObject, TEXT("valign"), TextVAlignNames, Text->VAlign);
        Enum(*TextObject, TEXT("sizing"), TextSizingNames, Text->Sizing);
        const TArray<TSharedPtr<FJsonValue>> *Runs = nullptr;
        if ((*TextObject)->TryGetArrayField(TEXT("runs"), Runs))
        {
          for (const TSharedPtr<FJsonValue> &RunValue : *Runs)
          {
            const TSharedPtr<FJsonObject> *RunObject = nullptr;
            if (!RunValue->TryGetObject(RunObject))
            {
              continue;
            }
            FTextRun &Run = Text->Runs.AddDefaulted_GetRef();
            double Value = 0.0;
            if ((*RunObject)->TryGetNumberField(TEXT("start"), Value))
            {
              Run.Start = FMath::RoundToInt32(Value);
            }
            Run.Len = Text->Content.Len() - Run.Start;
            if ((*RunObject)->TryGetNumberField(TEXT("len"), Value))
            {
              Run.Len = FMath::RoundToInt32(Value);
            }
            const TSharedPtr<FJsonObject> *Font = nullptr;
            if ((*RunObject)->TryGetObjectField(TEXT("font"), Font))
            {
              (*Font)->TryGetStringField(TEXT("family"), Run.Font.Family);
              (*Font)->TryGetStringField(TEXT("style"), Run.Font.Style);
              (*Font)->TryGetStringField(TEXT("postscript"), Run.Font.PostScript);
            }
            Number(*RunObject, TEXT("size"), Run.Size, true);
            Color(*RunObject, TEXT("color"), Run.Color);
            Number(*RunObject, TEXT("tracking"), Run.Tracking);
            FString Percent;
            if ((*RunObject)->TryGetNumberField(TEXT("lineHeight"), Value))
            {
              Run.LineHeightPx = Value;
            }
            else if ((*RunObject)->TryGetStringField(TEXT("lineHeight"), Percent))
            {
              if (Percent.EndsWith(TEXT("%")) &&
                  FCString::IsNumeric(*Percent.LeftChop(1)))
              {
                Run.LineHeightPercent = FCString::Atod(*Percent.LeftChop(1));
              }
              else
              {
                Error(FString::Printf(TEXT("\"lineHeight\" must be a number or "
                                           "\"N%%\" (got '%s')"),
                                      *Percent));
              }
            }
          }
        }
        if (Text->Runs.IsEmpty())
        {
          Error(TEXT("text needs at least one run"));
        }
        OutNode.Text = Text;
      }
      if (OutNode.Kind == EKind::Text && !OutNode.Text.IsValid())
      {
        Error(TEXT("a text node needs \"text\""));
      }

      const TSharedPtr<FJsonObject> *ImageObject = nullptr;
      if (InObject->TryGetObjectField(TEXT("image"), ImageObject))
      {
        OutNode.Image = Image(*ImageObject);
      }
      if (OutNode.Kind == EKind::Image && !OutNode.Image.IsValid())
      {
        Error(TEXT("an image node needs \"image\""));
      }

      const TSharedPtr<FJsonObject> *ComponentObject = nullptr;
      if (InObject->TryGetObjectField(TEXT("component"), ComponentObject))
      {
        TSharedRef<FComponentRef> Component = MakeShared<FComponentRef>();
        (*ComponentObject)->TryGetStringField(TEXT("key"), Component->Key);
        (*ComponentObject)->TryGetStringField(TEXT("name"), Component->Name);
        (*ComponentObject)->TryGetBoolField(TEXT("hasOverrides"), Component->bHasOverrides);
        if (Component->Key.IsEmpty())
        {
          Error(TEXT("\"component\" needs a \"key\""));
        }
        OutNode.Component = Component;
      }
      if (OutNode.Kind == EKind::Instance && !OutNode.Component.IsValid())
      {
        Error(TEXT("an instance node needs \"component\""));
      }

      const TArray<TSharedPtr<FJsonValue>> *Hints = nullptr;
      if (InObject->TryGetArrayField(TEXT("hints"), Hints))
      {
        for (const TSharedPtr<FJsonValue> &Value : *Hints)
        {
          FString Hint;
          if (Value->TryGetString(Hint))
          {
            OutNode.Hints.Add(Hint);
          }
        }
      }

      const TArray<TSharedPtr<FJsonValue>> *Children = nullptr;
      if (InObject->TryGetArrayField(TEXT("children"), Children))
      {
        if (!Children->IsEmpty() && OutNode.Kind != EKind::Frame &&
            OutNode.Kind != EKind::Group && OutNode.Kind != EKind::Instance)
        {
          Error(TEXT("only frames, groups and instances have children"));
        }
        OutNode.Children.Reserve(Children->Num());
        for (const TSharedPtr<FJsonValue> &Value : *Children)
        {
          const TSharedPtr<FJsonObject> *ChildObject = nullptr;
          if (!Value->TryGetObject(ChildObject))
          {
            Error(TEXT("children must be objects"));
            continue;
          }
          Node(*ChildObject, OutNode.Children.AddDefaulted_GetRef(), InDepth + 1);
        }
      }
      Where = SavedWhere;
      return true;
    }
  };

  // Ids must be unique across the document: they are the sidecar's keys.
  void CheckUniqueIds(const FNode &InNode, TSet<FString> &InOutIds,
                      TArray<FString> &OutErrors)
  {
    bool bAlreadyUsed = false;
    InOutIds.Add(InNode.Id, &bAlreadyUsed);
    if (bAlreadyUsed && !InNode.Id.IsEmpty())
    {
      OutErrors.Add(FString::Printf(TEXT("%s: the id is used by more than one node"),
                                    *InNode.Id));
    }
    for (const FNode &Child : InNode.Children)
    {
      CheckUniqueIds(Child, InOutIds, OutErrors);
    }
  }

  // ---------------------------------------------------------------------
  // Writing

  TSharedRef<FJsonValue> Num(double InValue)
  {
    return MakeShared<FJsonValueNumber>(InValue);
  }

  FString ColorText(const FColor &InColor)
  {
    return InColor.A == 255
               ? FString::Printf(TEXT("#%02X%02X%02X"), InColor.R, InColor.G, InColor.B)
               : FString::Printf(TEXT("#%02X%02X%02X%02X"), InColor.R, InColor.G,
                                 InColor.B, InColor.A);
  }

  TSharedRef<FJsonObject> WriteEdges(const FEdges &InEdges)
  {
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetField(TEXT("l"), Num(InEdges.L));
    Object->SetField(TEXT("t"), Num(InEdges.T));
    Object->SetField(TEXT("r"), Num(InEdges.R));
    Object->SetField(TEXT("b"), Num(InEdges.B));
    return Object;
  }

  TSharedRef<FJsonObject> WriteImage(const FImageRef &InImage)
  {
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("path"), InImage.Path);
    if (InImage.Scale != 1.0)
    {
      Object->SetField(TEXT("scale"), Num(InImage.Scale));
    }
    if (InImage.Origin != EImageOrigin::Fill)
    {
      Object->SetStringField(TEXT("origin"), EnumName(InImage.Origin, ImageOriginNames));
    }
    if (InImage.Mode != EImageMode::Stretch)
    {
      Object->SetStringField(TEXT("mode"), EnumName(InImage.Mode, ImageModeNames));
    }
    if (InImage.Size.IsSet())
    {
      TSharedRef<FJsonObject> Size = MakeShared<FJsonObject>();
      Size->SetField(TEXT("w"), Num(InImage.Size->X));
      Size->SetField(TEXT("h"), Num(InImage.Size->Y));
      Object->SetObjectField(TEXT("size"), Size);
    }
    return Object;
  }

  TSharedRef<FJsonObject> WriteNode(const FNode &InNode)
  {
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("id"), InNode.Id);
    Object->SetStringField(TEXT("name"), InNode.Name);
    Object->SetStringField(TEXT("kind"), EnumName(InNode.Kind, KindNames));
    TSharedRef<FJsonObject> Box = MakeShared<FJsonObject>();
    Box->SetField(TEXT("x"), Num(InNode.Box.X));
    Box->SetField(TEXT("y"), Num(InNode.Box.Y));
    Box->SetField(TEXT("w"), Num(InNode.Box.W));
    Box->SetField(TEXT("h"), Num(InNode.Box.H));
    Object->SetObjectField(TEXT("box"), Box);
    if (InNode.Rotation != 0.0)
    {
      Object->SetField(TEXT("rotation"), Num(InNode.Rotation));
    }
    if (InNode.Opacity != 1.0)
    {
      Object->SetField(TEXT("opacity"), Num(InNode.Opacity));
    }
    if (!InNode.bVisible)
    {
      Object->SetBoolField(TEXT("visible"), false);
    }
    if (InNode.BlendMode != TEXT("normal"))
    {
      Object->SetStringField(TEXT("blendMode"), InNode.BlendMode);
    }
    if (InNode.bClip)
    {
      Object->SetBoolField(TEXT("clip"), true);
    }
    if (InNode.bHasConstraints)
    {
      TSharedRef<FJsonObject> Constraints = MakeShared<FJsonObject>();
      Constraints->SetStringField(TEXT("h"), EnumName(InNode.ConstraintH, ConstraintHNames));
      Constraints->SetStringField(TEXT("v"), EnumName(InNode.ConstraintV, ConstraintVNames));
      Object->SetObjectField(TEXT("constraints"), Constraints);
    }
    if (InNode.SizeLimits.IsSet())
    {
      TSharedRef<FJsonObject> Limits = MakeShared<FJsonObject>();
      const FSizeLimits &SizeLimits = *InNode.SizeLimits;
      if (SizeLimits.MinW.IsSet())
      {
        Limits->SetField(TEXT("minW"), Num(*SizeLimits.MinW));
      }
      if (SizeLimits.MaxW.IsSet())
      {
        Limits->SetField(TEXT("maxW"), Num(*SizeLimits.MaxW));
      }
      if (SizeLimits.MinH.IsSet())
      {
        Limits->SetField(TEXT("minH"), Num(*SizeLimits.MinH));
      }
      if (SizeLimits.MaxH.IsSet())
      {
        Limits->SetField(TEXT("maxH"), Num(*SizeLimits.MaxH));
      }
      Object->SetObjectField(TEXT("sizeLimits"), Limits);
    }
    if (InNode.Layout.IsValid())
    {
      const FLayout &Layout = *InNode.Layout;
      TSharedRef<FJsonObject> LayoutObject = MakeShared<FJsonObject>();
      LayoutObject->SetStringField(TEXT("mode"), EnumName(Layout.Mode, LayoutModeNames));
      if (Layout.bWrap)
      {
        LayoutObject->SetBoolField(TEXT("wrap"), true);
      }
      if (Layout.Spacing != 0.0)
      {
        LayoutObject->SetField(TEXT("spacing"), Num(Layout.Spacing));
      }
      if (Layout.CrossSpacing != 0.0)
      {
        LayoutObject->SetField(TEXT("crossSpacing"), Num(Layout.CrossSpacing));
      }
      if (!Layout.Padding.IsZero())
      {
        LayoutObject->SetObjectField(TEXT("padding"), WriteEdges(Layout.Padding));
      }
      if (Layout.AlignMain != EAlignMain::Start || Layout.AlignCross != EAlignCross::Start)
      {
        TSharedRef<FJsonObject> Align = MakeShared<FJsonObject>();
        if (Layout.AlignMain != EAlignMain::Start)
        {
          Align->SetStringField(TEXT("main"), EnumName(Layout.AlignMain, AlignMainNames));
        }
        if (Layout.AlignCross != EAlignCross::Start)
        {
          Align->SetStringField(TEXT("cross"), EnumName(Layout.AlignCross, AlignCrossNames));
        }
        LayoutObject->SetObjectField(TEXT("align"), Align);
      }
      Object->SetObjectField(TEXT("layout"), LayoutObject);
    }
    if (InNode.SizingH != ESizing::Fixed || InNode.SizingV != ESizing::Fixed)
    {
      TSharedRef<FJsonObject> Sizing = MakeShared<FJsonObject>();
      Sizing->SetStringField(TEXT("h"), EnumName(InNode.SizingH, SizingNames));
      Sizing->SetStringField(TEXT("v"), EnumName(InNode.SizingV, SizingNames));
      Object->SetObjectField(TEXT("sizing"), Sizing);
    }
    if (InNode.bAbsolute)
    {
      Object->SetBoolField(TEXT("absolute"), true);
    }
    if (InNode.Fill.IsValid())
    {
      TSharedRef<FJsonObject> Fill = MakeShared<FJsonObject>();
      if (InNode.Fill->Color.IsSet())
      {
        Fill->SetStringField(TEXT("color"), ColorText(*InNode.Fill->Color));
      }
      if (InNode.Fill->Image.IsValid())
      {
        Fill->SetObjectField(TEXT("image"), WriteImage(*InNode.Fill->Image));
      }
      Object->SetObjectField(TEXT("fill"), Fill);
    }
    if (InNode.Stroke.IsValid())
    {
      TSharedRef<FJsonObject> Stroke = MakeShared<FJsonObject>();
      Stroke->SetStringField(TEXT("color"), ColorText(InNode.Stroke->Color));
      Stroke->SetObjectField(TEXT("weights"), WriteEdges(InNode.Stroke->Weights));
      Stroke->SetStringField(TEXT("align"), EnumName(InNode.Stroke->Align, StrokeAlignNames));
      Object->SetObjectField(TEXT("stroke"), Stroke);
    }
    if (InNode.Radii.IsSet())
    {
      const FVector4 &Radii = *InNode.Radii;
      Object->SetArrayField(TEXT("radii"),
                            {Num(Radii.X), Num(Radii.Y), Num(Radii.Z), Num(Radii.W)});
    }
    if (!InNode.Effects.IsEmpty())
    {
      TArray<TSharedPtr<FJsonValue>> Effects;
      for (const FEffect &Effect : InNode.Effects)
      {
        TSharedRef<FJsonObject> EffectObject = MakeShared<FJsonObject>();
        EffectObject->SetStringField(TEXT("type"), Effect.Type);
        EffectObject->SetBoolField(TEXT("baked"), Effect.bBaked);
        if (Effect.Color.IsSet())
        {
          EffectObject->SetStringField(TEXT("color"), ColorText(*Effect.Color));
          TSharedRef<FJsonObject> Offset = MakeShared<FJsonObject>();
          Offset->SetField(TEXT("x"), Num(Effect.Offset.X));
          Offset->SetField(TEXT("y"), Num(Effect.Offset.Y));
          EffectObject->SetObjectField(TEXT("offset"), Offset);
          EffectObject->SetField(TEXT("radius"), Num(Effect.Radius));
          EffectObject->SetField(TEXT("spread"), Num(Effect.Spread));
        }
        Effects.Add(MakeShared<FJsonValueObject>(EffectObject));
      }
      Object->SetArrayField(TEXT("effects"), Effects);
    }
    if (InNode.Text.IsValid())
    {
      const FTextBlock &Text = *InNode.Text;
      TSharedRef<FJsonObject> TextObject = MakeShared<FJsonObject>();
      TextObject->SetStringField(TEXT("content"), Text.Content);
      TArray<TSharedPtr<FJsonValue>> Runs;
      for (const FTextRun &Run : Text.Runs)
      {
        TSharedRef<FJsonObject> RunObject = MakeShared<FJsonObject>();
        RunObject->SetField(TEXT("start"), Num(Run.Start));
        RunObject->SetField(TEXT("len"), Num(Run.Len));
        TSharedRef<FJsonObject> Font = MakeShared<FJsonObject>();
        if (!Run.Font.Family.IsEmpty())
        {
          Font->SetStringField(TEXT("family"), Run.Font.Family);
        }
        if (!Run.Font.Style.IsEmpty())
        {
          Font->SetStringField(TEXT("style"), Run.Font.Style);
        }
        if (!Run.Font.PostScript.IsEmpty())
        {
          Font->SetStringField(TEXT("postscript"), Run.Font.PostScript);
        }
        RunObject->SetObjectField(TEXT("font"), Font);
        RunObject->SetField(TEXT("size"), Num(Run.Size));
        RunObject->SetStringField(TEXT("color"), ColorText(Run.Color));
        if (Run.Tracking != 0.0)
        {
          RunObject->SetField(TEXT("tracking"), Num(Run.Tracking));
        }
        if (Run.LineHeightPx.IsSet())
        {
          RunObject->SetField(TEXT("lineHeight"), Num(*Run.LineHeightPx));
        }
        else if (Run.LineHeightPercent.IsSet())
        {
          RunObject->SetStringField(TEXT("lineHeight"),
                                    FString::SanitizeFloat(*Run.LineHeightPercent, 0) +
                                        TEXT("%"));
        }
        Runs.Add(MakeShared<FJsonValueObject>(RunObject));
      }
      TextObject->SetArrayField(TEXT("runs"), Runs);
      if (Text.Align != ETextAlign::Left)
      {
        TextObject->SetStringField(TEXT("align"), EnumName(Text.Align, TextAlignNames));
      }
      if (Text.VAlign != ETextVAlign::Top)
      {
        TextObject->SetStringField(TEXT("valign"), EnumName(Text.VAlign, TextVAlignNames));
      }
      if (Text.Sizing != ETextSizing::Auto)
      {
        TextObject->SetStringField(TEXT("sizing"), EnumName(Text.Sizing, TextSizingNames));
      }
      Object->SetObjectField(TEXT("text"), TextObject);
    }
    if (InNode.Image.IsValid())
    {
      Object->SetObjectField(TEXT("image"), WriteImage(*InNode.Image));
    }
    if (InNode.Component.IsValid())
    {
      TSharedRef<FJsonObject> Component = MakeShared<FJsonObject>();
      Component->SetStringField(TEXT("key"), InNode.Component->Key);
      Component->SetStringField(TEXT("name"), InNode.Component->Name);
      if (InNode.Component->bHasOverrides)
      {
        Component->SetBoolField(TEXT("hasOverrides"), true);
      }
      Object->SetObjectField(TEXT("component"), Component);
    }
    if (!InNode.Hints.IsEmpty())
    {
      TArray<TSharedPtr<FJsonValue>> Hints;
      for (const FString &Hint : InNode.Hints)
      {
        Hints.Add(MakeShared<FJsonValueString>(Hint));
      }
      Object->SetArrayField(TEXT("hints"), Hints);
    }
    if (!InNode.Children.IsEmpty())
    {
      TArray<TSharedPtr<FJsonValue>> Children;
      Children.Reserve(InNode.Children.Num());
      for (const FNode &Child : InNode.Children)
      {
        Children.Add(MakeShared<FJsonValueObject>(WriteNode(Child)));
      }
      Object->SetArrayField(TEXT("children"), Children);
    }
    return Object;
  }
}

bool UIWTDesignTree::ReadDocument(const FString &InJson, FDocument &OutDocument,
                                  TArray<FString> &OutErrors)
{
  TSharedPtr<FJsonObject> Object;
  if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(InJson), Object) ||
      !Object.IsValid())
  {
    OutErrors.Add(TEXT("design.json is not a JSON object"));
    return false;
  }
  return ReadDocument(Object.ToSharedRef(), OutDocument, OutErrors);
}

bool UIWTDesignTree::ReadDocument(const TSharedRef<FJsonObject> &InJson,
                                  FDocument &OutDocument, TArray<FString> &OutErrors)
{
  OutDocument = FDocument();
  FReader Reader{OutErrors, FString()};

  double Version = 0.0;
  if (!InJson->TryGetNumberField(TEXT("version"), Version) || Version != 1.0)
  {
    OutErrors.Add(TEXT("\"version\" must be 1"));
    return false;
  }
  OutDocument.Version = 1;
  InJson->TryGetStringField(TEXT("source"), OutDocument.Source);
  const TSharedPtr<FJsonObject> *SourceRef = nullptr;
  if (InJson->TryGetObjectField(TEXT("sourceRef"), SourceRef))
  {
    OutDocument.SourceRef = *SourceRef;
  }
  const TSharedPtr<FJsonObject> *ReferenceSize = nullptr;
  if (InJson->TryGetObjectField(TEXT("referenceSize"), ReferenceSize))
  {
    Reader.Number(*ReferenceSize, TEXT("w"), OutDocument.ReferenceSize.X, true);
    Reader.Number(*ReferenceSize, TEXT("h"), OutDocument.ReferenceSize.Y, true);
  }
  else
  {
    OutErrors.Add(TEXT("\"referenceSize\" is required"));
  }
  Reader.Enum(InJson, TEXT("rootMode"), RootModeNames, OutDocument.RootMode);

  const TArray<TSharedPtr<FJsonValue>> *Notes = nullptr;
  if (InJson->TryGetArrayField(TEXT("notes"), Notes))
  {
    for (const TSharedPtr<FJsonValue> &Value : *Notes)
    {
      const TSharedPtr<FJsonObject> *NoteObject = nullptr;
      if (Value->TryGetObject(NoteObject))
      {
        FNote &Note = OutDocument.Notes.AddDefaulted_GetRef();
        (*NoteObject)->TryGetStringField(TEXT("node"), Note.Node);
        (*NoteObject)->TryGetStringField(TEXT("category"), Note.Category);
        (*NoteObject)->TryGetStringField(TEXT("detail"), Note.Detail);
      }
    }
  }

  const TSharedPtr<FJsonObject> *Components = nullptr;
  if (InJson->TryGetObjectField(TEXT("components"), Components))
  {
    for (const auto &Pair : (*Components)->Values)
    {
      const FString Key(Pair.Key);
      const TSharedPtr<FJsonObject> *DefObject = nullptr;
      if (!Pair.Value->TryGetObject(DefObject))
      {
        OutErrors.Add(FString::Printf(TEXT("components.%s must be an object"), *Key));
        continue;
      }
      FComponentDef &Def = OutDocument.Components.Add(Key);
      (*DefObject)->TryGetStringField(TEXT("name"), Def.Name);
      (*DefObject)->TryGetStringField(TEXT("hash"), Def.Hash);
      const TSharedPtr<FJsonObject> *DefSourceRef = nullptr;
      if ((*DefObject)->TryGetObjectField(TEXT("sourceRef"), DefSourceRef))
      {
        (*DefSourceRef)->TryGetStringField(TEXT("fileKey"), Def.FileKey);
        (*DefSourceRef)->TryGetStringField(TEXT("nodeId"), Def.NodeId);
        (*DefSourceRef)->TryGetStringField(TEXT("version"), Def.Version);
      }
      const TSharedPtr<FJsonObject> *Root = nullptr;
      if ((*DefObject)->TryGetObjectField(TEXT("root"), Root))
      {
        Reader.Where = FString::Printf(TEXT("components.%s"), *Key);
        Reader.Node(*Root, Def.Root, 0);
        Reader.Where.Reset();
      }
      else
      {
        OutErrors.Add(FString::Printf(TEXT("components.%s needs a \"root\""), *Key));
      }
    }
  }

  const TSharedPtr<FJsonObject> *Root = nullptr;
  if (!InJson->TryGetObjectField(TEXT("root"), Root))
  {
    OutErrors.Add(TEXT("\"root\" is required"));
    return false;
  }
  Reader.Node(*Root, OutDocument.Root, 0);

  TSet<FString> Ids;
  CheckUniqueIds(OutDocument.Root, Ids, OutErrors);
  return OutErrors.IsEmpty();
}

TSharedRef<FJsonObject> UIWTDesignTree::WriteDocument(const FDocument &InDocument)
{
  TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetField(TEXT("version"), Num(InDocument.Version));
  if (!InDocument.Source.IsEmpty())
  {
    Object->SetStringField(TEXT("source"), InDocument.Source);
  }
  if (InDocument.SourceRef.IsValid())
  {
    Object->SetObjectField(TEXT("sourceRef"), InDocument.SourceRef);
  }
  TSharedRef<FJsonObject> ReferenceSize = MakeShared<FJsonObject>();
  ReferenceSize->SetField(TEXT("w"), Num(InDocument.ReferenceSize.X));
  ReferenceSize->SetField(TEXT("h"), Num(InDocument.ReferenceSize.Y));
  Object->SetObjectField(TEXT("referenceSize"), ReferenceSize);
  if (InDocument.RootMode != ERootMode::Auto)
  {
    Object->SetStringField(TEXT("rootMode"), EnumName(InDocument.RootMode, RootModeNames));
  }
  if (!InDocument.Notes.IsEmpty())
  {
    TArray<TSharedPtr<FJsonValue>> Notes;
    for (const FNote &Note : InDocument.Notes)
    {
      TSharedRef<FJsonObject> NoteObject = MakeShared<FJsonObject>();
      NoteObject->SetStringField(TEXT("node"), Note.Node);
      NoteObject->SetStringField(TEXT("category"), Note.Category);
      NoteObject->SetStringField(TEXT("detail"), Note.Detail);
      Notes.Add(MakeShared<FJsonValueObject>(NoteObject));
    }
    Object->SetArrayField(TEXT("notes"), Notes);
  }
  if (!InDocument.Components.IsEmpty())
  {
    TSharedRef<FJsonObject> Components = MakeShared<FJsonObject>();
    TArray<FString> Keys;
    InDocument.Components.GetKeys(Keys);
    Keys.Sort();
    for (const FString &Key : Keys)
    {
      const FComponentDef &Def = InDocument.Components[Key];
      TSharedRef<FJsonObject> DefObject = MakeShared<FJsonObject>();
      DefObject->SetStringField(TEXT("name"), Def.Name);
      TSharedRef<FJsonObject> DefSourceRef = MakeShared<FJsonObject>();
      DefSourceRef->SetStringField(TEXT("fileKey"), Def.FileKey);
      DefSourceRef->SetStringField(TEXT("nodeId"), Def.NodeId);
      DefSourceRef->SetStringField(TEXT("version"), Def.Version);
      DefObject->SetObjectField(TEXT("sourceRef"), DefSourceRef);
      DefObject->SetStringField(TEXT("hash"), Def.Hash);
      DefObject->SetObjectField(TEXT("root"), WriteNode(Def.Root));
      Components->SetObjectField(Key, DefObject);
    }
    Object->SetObjectField(TEXT("components"), Components);
  }
  Object->SetObjectField(TEXT("root"), WriteNode(InDocument.Root));
  return Object;
}

FString UIWTDesignTree::WriteDocumentString(const FDocument &InDocument)
{
  FString Json;
  TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
      TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);
  FJsonSerializer::Serialize(WriteDocument(InDocument), Writer);
  return Json;
}

FString UIWTDesignTree::HashNode(const FNode &InNode, const FString &InExtra)
{
  FString Json;
  TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
      TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
  FJsonSerializer::Serialize(WriteNode(InNode), Writer);
  Json += InExtra;
  const FTCHARToUTF8 Utf8(*Json);
  FSHAHash Hash;
  FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash.Hash);
  return Hash.ToString().ToLower();
}

double UIWTDesignTree::Round2(double InValue)
{
  return FMath::RoundToDouble(InValue * 100.0) / 100.0;
}

FString UIWTDesignTree::FileSafeName(const FString &InName, const TCHAR *InFallback)
{
  FString Name;
  for (const TCHAR Char : InName)
  {
    Name.AppendChar(FChar::IsAlnum(Char) || Char == TEXT('-') ? Char : TEXT('_'));
  }
  return Name.IsEmpty() ? FString(InFallback) : Name;
}
