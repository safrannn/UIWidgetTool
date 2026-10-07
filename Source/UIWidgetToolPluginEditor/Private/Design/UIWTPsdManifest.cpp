#include "UIWTPsdManifest.h"

#include "Core/UIWTJson.h"
#include "Core/UIWTRunImages.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// The manifest → design tree mapping (import-ps.md step 2). Every bound in
// the manifest is absolute, in document pixels; the tree wants boxes
// relative to the parent, in 1x reference pixels (import-tree.md → Units).
namespace
{
  using namespace UIWTDesignTree;
  using namespace UIWTJson;
  using namespace UIWTPsdManifest;

  // Layer ids are numbers in the manifest; anything else is taken as text.
  FString IdOf(const FJsonObject &InNode)
  {
    const TSharedPtr<FJsonValue> Value = InNode.TryGetField(TEXT("id"));
    if (!Value.IsValid())
    {
      return FString();
    }
    if (Value->Type == EJson::Number)
    {
      return FString::Printf(TEXT("%lld"), static_cast<int64>(Value->AsNumber()));
    }
    return Value->AsString();
  }

  FColor ColorOf(const FString &InHex, double InAlphaScale = 1.0)
  {
    FColor Color = FColor::FromHex(InHex);
    Color.A = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Color.A * InAlphaScale), 0, 255));
    return Color;
  }

  // Photoshop's effect names → the tree's effect types.
  FString EffectTypeOf(const FString &InName)
  {
    if (InName == TEXT("dropShadow"))
    {
      return TEXT("dropShadow");
    }
    if (InName == TEXT("innerShadow"))
    {
      return TEXT("innerShadow");
    }
    if (InName == TEXT("outerGlow") || InName == TEXT("innerGlow"))
    {
      return TEXT("glow");
    }
    return InName.Contains(TEXT("blur"), ESearchCase::IgnoreCase) ? TEXT("blur") : TEXT("other");
  }

  // Name prefixes designers use, as hints for the optional Claude pass.
  void AddPrefixHint(const FString &InName, TArray<FString> &OutHints)
  {
    static const TCHAR *const Prefixes[] = {TEXT("btn"), TEXT("button"), TEXT("txt"), TEXT("text"),
                                            TEXT("list"), TEXT("img"), TEXT("icon"), TEXT("bg")};
    FString Prefix;
    if (!InName.Split(TEXT("_"), &Prefix, nullptr) || Prefix.IsEmpty())
    {
      return;
    }
    Prefix.ToLowerInline();
    for (const TCHAR *Known : Prefixes)
    {
      if (Prefix == Known)
      {
        OutHints.Add(TEXT("prefix:") + Prefix);
        return;
      }
    }
  }

  class FReader
  {
  public:
    FReader(const FString &InDirectory, const FOptions &InOptions, FResult &InOutResult)
        : Directory(InDirectory), Options(InOptions), Result(InOutResult)
    {
    }

    // Document pixels per 1x pixel, and design pixels per point.
    double Scale = 1.0;
    double PointToPixel = 1.0;
    // Node ids are <prefix>/<layer id>: the artboard's id, or "doc".
    FString Prefix;

    bool Convert(const FJsonObject &InNode, const FVector2D &InParentPos, FNode &OutNode);

  private:
    const FString &Directory;
    const FOptions &Options;
    FResult &Result;

    void Note(const FString &InNode, const TCHAR *InCategory, const FString &InDetail)
    {
      Result.Document.Notes.Add({InNode, InCategory, InDetail});
    }

    void Text(const FJsonObject &InText, FNode &OutNode);
    bool Shape(const FJsonObject &InNode, const FString &InBlendMode, FNode &OutNode);
    bool Image(const FJsonObject &InNode, FNode &OutNode);
  };

  bool FReader::Convert(const FJsonObject &InNode, const FVector2D &InParentPos, FNode &OutNode)
  {
    const FString Kind = Str(InNode, TEXT("kind"));
    const FString Id = Prefix + TEXT("/") + IdOf(InNode);
    const FString Name = Str(InNode, TEXT("name"));
    const bool bVisible = Bool(InNode, TEXT("visible"), true);
    if (!bVisible && !Options.bImportHidden)
    {
      return false;
    }
    if (Kind == TEXT("adjustment"))
    {
      Note(Id, TEXT("adjustmentDropped"),
           FString::Printf(TEXT("adjustment layer '%s' has no UMG equivalent: the composite "
                                "shows it, the import doesn't"),
                           *Name));
      return false;
    }
    const FJsonObject *Bounds = Obj(InNode, TEXT("bounds"));
    const double X = Bounds ? Num(*Bounds, TEXT("x")) : 0.0;
    const double Y = Bounds ? Num(*Bounds, TEXT("y")) : 0.0;
    const double W = Bounds ? Num(*Bounds, TEXT("w")) : 0.0;
    const double H = Bounds ? Num(*Bounds, TEXT("h")) : 0.0;
    if (W <= 0.0 || H <= 0.0)
    {
      // An empty layer or group: nothing to draw.
      return false;
    }

    OutNode.Id = Id;
    OutNode.Name = Name;
    OutNode.bVisible = bVisible;
    OutNode.Opacity = FMath::RoundToDouble(Num(InNode, TEXT("opacity"), 1.0) * 1000.0) / 1000.0;
    const FString Blend = Str(InNode, TEXT("blendMode"));
    OutNode.BlendMode = Blend.IsEmpty() || Blend == TEXT("normal") || Blend == TEXT("passThrough")
                            ? FString(TEXT("normal"))
                            : Blend;
    OutNode.Box = {Round2((X - InParentPos.X) / Scale), Round2((Y - InParentPos.Y) / Scale),
                   Round2(W / Scale), Round2(H / Scale)};
    AddPrefixHint(Name, OutNode.Hints);

    // Effects are baked into the PNGs of leaves that are images; groups and
    // live text keep them listed, and the converter reports them dropped.
    TArray<FString> EffectNames;
    if (const FJsonArray *Effects = Arr(InNode, TEXT("effects")))
    {
      for (const TSharedPtr<FJsonValue> &Effect : *Effects)
      {
        EffectNames.Add(Effect->AsString());
      }
    }
    auto AddEffects = [&](bool bInBaked)
    {
      for (const FString &Effect : EffectNames)
      {
        OutNode.Effects.Add({EffectTypeOf(Effect), bInBaked});
      }
    };

    if (Kind == TEXT("group"))
    {
      OutNode.Kind = EKind::Group;
      AddEffects(false);
      if (const FJsonArray *Children = Arr(InNode, TEXT("children")))
      {
        for (const TSharedPtr<FJsonValue> &Value : *Children)
        {
          const TSharedPtr<FJsonObject> Child = Value->AsObject();
          FNode ChildNode;
          if (Child.IsValid() && Convert(*Child, FVector2D(X, Y), ChildNode))
          {
            OutNode.Children.Add(MoveTemp(ChildNode));
          }
        }
      }
      if (OutNode.Children.IsEmpty())
      {
        return false;
      }
    }
    else if (Kind == TEXT("text") && Obj(InNode, TEXT("text")))
    {
      OutNode.Kind = EKind::Text;
      Text(*Obj(InNode, TEXT("text")), OutNode);
      AddEffects(false);
    }
    else if (Kind == TEXT("shape") && Shape(InNode, OutNode.BlendMode, OutNode))
    {
      OutNode.Kind = EKind::Shape;
      AddEffects(false);
    }
    else if (Image(InNode, OutNode))
    {
      OutNode.Kind = EKind::Image;
      AddEffects(true);
    }
    else
    {
      Note(Id, TEXT("layerSkipped"),
           FString::Printf(TEXT("%s layer '%s' has no image to import"), *Kind, *Name));
      return false;
    }
    ++Result.NodeCount;
    return true;
  }

  void FReader::Text(const FJsonObject &InText, FNode &OutNode)
  {
    TSharedRef<FTextBlock> Block = MakeShared<FTextBlock>();
    // Photoshop ends paragraphs with a carriage return.
    Block->Content = Str(InText, TEXT("content"))
                         .Replace(TEXT("\r\n"), TEXT("\n"))
                         .Replace(TEXT("\r"), TEXT("\n"));
    const FString Align = Str(InText, TEXT("align"));
    Block->Align = Align == TEXT("center")    ? ETextAlign::Center
                   : Align == TEXT("right")   ? ETextAlign::Right
                   : Align == TEXT("justify") ? ETextAlign::Justify
                                              : ETextAlign::Left;
    // Box (paragraph) text wraps at its box; point text grows with its line.
    Block->Sizing = Obj(InText, TEXT("box")) ? ETextSizing::FixedWidth : ETextSizing::Auto;
    if (const FJsonArray *Runs = Arr(InText, TEXT("runs")))
    {
      for (const TSharedPtr<FJsonValue> &Value : *Runs)
      {
        const TSharedPtr<FJsonObject> Run = Value->AsObject();
        if (!Run.IsValid())
        {
          continue;
        }
        FTextRun TextRun;
        const int32 Length = Block->Content.Len();
        TextRun.Start = FMath::Clamp(static_cast<int32>(Num(*Run, TEXT("from"))), 0, Length);
        TextRun.Len =
            FMath::Clamp(static_cast<int32>(Num(*Run, TEXT("to"), Length)), TextRun.Start, Length) -
            TextRun.Start;
        if (const FJsonObject *Font = Obj(*Run, TEXT("font")))
        {
          TextRun.Font.Family = Str(*Font, TEXT("family"));
          TextRun.Font.Style = Str(*Font, TEXT("style"));
          TextRun.Font.PostScript = Str(*Font, TEXT("postscript"));
        }
        TextRun.Size = Round2(Num(*Run, TEXT("size"), 12.0) * PointToPixel);
        const FString Color = Str(*Run, TEXT("color"));
        TextRun.Color = Color.IsEmpty() ? FColor::Black : ColorOf(Color);
        TextRun.Tracking = Num(*Run, TEXT("tracking"));
        double Leading = 0.0;
        if (Run->TryGetNumberField(TEXT("leading"), Leading) && Leading > 0.0)
        {
          TextRun.LineHeightPx = Round2(Leading * PointToPixel);
        }
        Block->Runs.Add(MoveTemp(TextRun));
      }
    }
    if (Block->Runs.IsEmpty())
    {
      FTextRun Default;
      Default.Len = Block->Content.Len();
      Default.Size = Round2(12.0 * PointToPixel);
      Block->Runs.Add(Default);
    }
    OutNode.Text = Block;
  }

  // A solid rectangle or rounded rectangle with a normal blend, drawn as a
  // Border. Anything else falls back to the layer's PNG.
  bool FReader::Shape(const FJsonObject &InNode, const FString &InBlendMode, FNode &OutNode)
  {
    const FJsonObject *ShapeJson = Obj(InNode, TEXT("shape"));
    if (!ShapeJson || InBlendMode != TEXT("normal"))
    {
      return false;
    }
    const FString Fill = Str(*ShapeJson, TEXT("fill"));
    const FJsonObject *Stroke = Obj(*ShapeJson, TEXT("stroke"));
    if (Fill.IsEmpty() && !Stroke)
    {
      return false;
    }
    if (!Fill.IsEmpty())
    {
      TSharedRef<FFill> Block = MakeShared<FFill>();
      // Fill opacity only affects the fill; layer opacity is the node's.
      Block->Color = ColorOf(Fill, Num(InNode, TEXT("fillOpacity"), 1.0));
      OutNode.Fill = Block;
    }
    if (Stroke)
    {
      TSharedRef<FStroke> Block = MakeShared<FStroke>();
      Block->Color = ColorOf(Str(*Stroke, TEXT("color")));
      const double Width = Round2(Num(*Stroke, TEXT("width"), 1.0) / Scale);
      Block->Weights = {Width, Width, Width, Width};
      const FString Align = Str(*Stroke, TEXT("align"));
      Block->Align = Align == TEXT("center")    ? EStrokeAlign::Center
                     : Align == TEXT("outside") ? EStrokeAlign::Outside
                                                : EStrokeAlign::Inside;
      OutNode.Stroke = Block;
    }
    if (const FJsonArray *Radii = Arr(*ShapeJson, TEXT("radii")))
    {
      if (Radii->Num() == 4)
      {
        const FVector4 Value(Round2((*Radii)[0]->AsNumber() / Scale),
                             Round2((*Radii)[1]->AsNumber() / Scale),
                             Round2((*Radii)[2]->AsNumber() / Scale),
                             Round2((*Radii)[3]->AsNumber() / Scale));
        if (Value.X > 0 || Value.Y > 0 || Value.Z > 0 || Value.W > 0)
        {
          OutNode.Radii = Value;
        }
      }
    }
    return true;
  }

  // The layer's PNG, as Photoshop rendered it (effects included).
  bool FReader::Image(const FJsonObject &InNode, FNode &OutNode)
  {
    const FString Path = Str(InNode, TEXT("image"));
    // Only files inside the export folder.
    if (Path.IsEmpty() || !FPaths::IsRelative(Path) || Path.Contains(TEXT("..")))
    {
      return false;
    }
    TSharedRef<FImageRef> Ref = MakeShared<FImageRef>();
    Ref->Path = Path;
    Ref->Origin = EImageOrigin::Rendered;
    FIntPoint Size;
    if (UIWTRunImages::ReadPngSize(Directory / Path, Size))
    {
      Ref->Size = FVector2D(Size.X, Size.Y);
      Ref->Scale = OutNode.Box.W > 0.0
                       ? FMath::RoundToDouble(Size.X / OutNode.Box.W * 1000.0) / 1000.0
                       : Scale;
    }
    else
    {
      Ref->Scale = Scale;
      Note(OutNode.Id, TEXT("imageMissing"),
           FString::Printf(TEXT("%s is missing or isn't a PNG"), *Path));
    }
    OutNode.Image = Ref;
    return true;
  }
}

// ---------------------------------------------------------------------------

bool UIWTPsdManifest::Read(const FString &InManifestJson, const FString &InDirectory,
                           const FOptions &InOptions, FResult &OutResult, FString &OutError)
{
  OutResult = FResult();
  TSharedPtr<FJsonObject> Manifest;
  if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(InManifestJson), Manifest) ||
      !Manifest.IsValid())
  {
    OutError = TEXT("manifest.json isn't valid JSON.");
    return false;
  }
  const int32 Version = static_cast<int32>(Num(*Manifest, TEXT("version"), 1.0));
  if (Version != 1)
  {
    OutError = FString::Printf(TEXT("manifest.json is version %d; this reader knows version 1. "
                                    "Update the plugin or the Photoshop exporter."),
                               Version);
    return false;
  }
  const FJsonObject *Document = Obj(*Manifest, TEXT("document"));
  const FJsonObject *Root = Obj(*Manifest, TEXT("root"));
  if (!Document || !Root)
  {
    OutError = TEXT("manifest.json has no document or root.");
    return false;
  }
  const FJsonObject *Source = Obj(*Manifest, TEXT("source"));

  FReader Reader(InDirectory, InOptions, OutResult);
  Reader.Scale = FMath::Max(Num(*Document, TEXT("scale"), 1.0), 0.01);
  const double Resolution = FMath::Max(Num(*Document, TEXT("resolution"), 72.0), 1.0);
  Reader.PointToPixel = Resolution / 72.0 / Reader.Scale;
  const bool bArtboard = Str(*Root, TEXT("kind")) == TEXT("artboard");
  Reader.Prefix = bArtboard ? IdOf(*Root) : FString(TEXT("doc"));

  // The root: the document, or an artboard (which clips its content).
  const FJsonObject *RootBounds = Obj(*Root, TEXT("bounds"));
  const FVector2D RootPos(RootBounds ? Num(*RootBounds, TEXT("x")) : 0.0,
                          RootBounds ? Num(*RootBounds, TEXT("y")) : 0.0);
  const double RootW = RootBounds ? Num(*RootBounds, TEXT("w")) : Num(*Document, TEXT("w"));
  const double RootH = RootBounds ? Num(*RootBounds, TEXT("h")) : Num(*Document, TEXT("h"));
  FDocument &Doc = OutResult.Document;
  Doc.Version = 1;
  Doc.Source = TEXT("psd");
  Doc.ReferenceSize = FVector2D(InOptions.ReferenceSize.X, InOptions.ReferenceSize.Y);
  TSharedRef<FJsonObject> SourceRef = MakeShared<FJsonObject>();
  SourceRef->SetStringField(TEXT("nodeId"), Reader.Prefix);
  if (Source)
  {
    SourceRef->SetStringField(TEXT("document"), Str(*Source, TEXT("file")));
    OutResult.ExportedAt = Str(*Source, TEXT("exportedAt"));
    SourceRef->SetStringField(TEXT("exportedAt"), OutResult.ExportedAt);
  }
  Doc.SourceRef = SourceRef;
  Doc.Root.Id = Reader.Prefix;
  Doc.Root.Name = Str(*Root, TEXT("name"));
  if (Doc.Root.Name.IsEmpty() && Source)
  {
    Doc.Root.Name = FPaths::GetBaseFilename(Str(*Source, TEXT("file")));
  }
  Doc.Root.Kind = EKind::Frame;
  Doc.Root.bClip = bArtboard;
  Doc.Root.Box = {0.0, 0.0, Round2(RootW / Reader.Scale), Round2(RootH / Reader.Scale)};
  if (Doc.Root.Box.W <= 0.0 || Doc.Root.Box.H <= 0.0)
  {
    OutError = TEXT("manifest.json's document has no size.");
    return false;
  }
  if (const FJsonArray *Children = Arr(*Root, TEXT("children")))
  {
    for (const TSharedPtr<FJsonValue> &Value : *Children)
    {
      const TSharedPtr<FJsonObject> Child = Value->AsObject();
      FNode ChildNode;
      if (Child.IsValid() && Reader.Convert(*Child, RootPos, ChildNode))
      {
        Doc.Root.Children.Add(MoveTemp(ChildNode));
      }
    }
  }
  ++OutResult.NodeCount;
  return true;
}

FString UIWTPsdManifest::GetCacheDirectory(const FString &InManifestFile)
{
  FString Folder = FPaths::GetPath(FPaths::ConvertRelativePathToFull(InManifestFile));
  FPaths::NormalizeDirectoryName(Folder);
  const uint32 Hash = FCrc::StrCrc32(*Folder.ToLower());
  return FPaths::ConvertRelativePathToFull(
      FPaths::ProjectSavedDir() / TEXT("UIWidgetTool/Psd") /
      FString::Printf(TEXT("%s_%08x"),
                      *FileSafeName(FPaths::GetCleanFilename(Folder), TEXT("export")), Hash));
}

bool UIWTPsdManifest::Prepare(const FString &InManifestFile, const FOptions &InOptions,
                              FString &OutDesignFile, FResult &OutResult, FString &OutError)
{
  const FString Manifest = FPaths::ConvertRelativePathToFull(InManifestFile);
  const FString Source = FPaths::GetPath(Manifest);
  const FString Cache = GetCacheDirectory(Manifest);
  FString Json;
  if (!FFileHelper::LoadFileToString(Json, *Manifest))
  {
    OutError = FString::Printf(TEXT("Could not read %s."), *Manifest);
    return false;
  }

  // A copy of the export in the project, so the import never writes into
  // the designer's folder and a later export can't change it midway.
  IFileManager &Files = IFileManager::Get();
  Files.DeleteDirectory(*(Cache / TEXT("layers")), false, true);
  Files.MakeDirectory(*(Cache / TEXT("layers")), true);
  TArray<FString> Layers;
  Files.FindFiles(Layers, *(Source / TEXT("layers/*.png")), true, false);
  for (const FString &Layer : Layers)
  {
    if (Files.Copy(*(Cache / TEXT("layers") / Layer), *(Source / TEXT("layers") / Layer), true,
                   true) != COPY_OK)
    {
      OutError = FString::Printf(TEXT("Could not copy layers/%s."), *Layer);
      return false;
    }
  }
  Files.Delete(*(Cache / TEXT("reference.png")), false, true, true);
  if (Files.FileExists(*(Source / TEXT("composite.png"))))
  {
    Files.Copy(*(Cache / TEXT("reference.png")), *(Source / TEXT("composite.png")), true, true);
  }
  FFileHelper::SaveStringToFile(Json, *(Cache / TEXT("manifest.json")),
                                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

  if (!Read(Json, Cache, InOptions, OutResult, OutError))
  {
    return false;
  }
  // Where a re-import reads the export again.
  OutResult.Document.SourceRef->SetStringField(TEXT("manifest"), Manifest);
  OutDesignFile = Cache / TEXT("design.json");
  if (!FFileHelper::SaveStringToFile(WriteDocumentString(OutResult.Document), *OutDesignFile,
                                     FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
  {
    OutError = FString::Printf(TEXT("Could not write %s."), *OutDesignFile);
    return false;
  }
  return true;
}
