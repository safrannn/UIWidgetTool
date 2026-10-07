#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Core/UIWTDesignFonts.h"
#include "Core/UIWTRunImages.h"
#include "Dom/JsonObject.h"
#include "Figma/UIWTFigmaClient.h"
#include "Figma/UIWTFigmaNormalize.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Tests for the Figma reader (import-figma.md steps 1 and 2) that need no
// network: link parsing, and the normalizer on a hand-written /nodes
// response in Plugins/UIWidgetToolPlugin/Tests/Figma.
namespace
{
  using namespace UIWTDesignTree;

  constexpr EAutomationTestFlags TestFlags =
      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

  const FNode *FindNode(const FNode &InNode, const FString &InId)
  {
    if (InNode.Id == InId)
    {
      return &InNode;
    }
    for (const FNode &Child : InNode.Children)
    {
      if (const FNode *Found = FindNode(Child, InId))
      {
        return Found;
      }
    }
    return nullptr;
  }

  bool HasNote(const FDocument &InDoc, const FString &InNode, const FString &InCategory)
  {
    return InDoc.Notes.ContainsByPredicate([&](const FNote &Note)
                                           { return Note.Node == InNode && Note.Category == InCategory; });
  }

  bool NearlyBox(const FRect &InBox, double InX, double InY, double InW, double InH)
  {
    return FMath::IsNearlyEqual(InBox.X, InX, 0.01) && FMath::IsNearlyEqual(InBox.Y, InY, 0.01) &&
           FMath::IsNearlyEqual(InBox.W, InW, 0.01) && FMath::IsNearlyEqual(InBox.H, InH, 0.01);
  }

  FString BoxText(const FRect &InBox)
  {
    return FString::Printf(TEXT("(%g, %g, %g, %g)"), InBox.X, InBox.Y, InBox.W, InBox.H);
  }

  class FUIWTFigmaTestBase : public FAutomationTestBase
  {
  public:
    FUIWTFigmaTestBase(const FString &InName, const bool bInComplexTask)
        : FAutomationTestBase(InName, bInComplexTask)
    {
    }

  protected:
    bool LoadFixture(UIWTFigmaNormalize::FOptions &OutOptions, UIWTFigmaNormalize::FResult &OutResult)
    {
      const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UIWidgetToolPlugin"));
      const FString Path = Plugin.IsValid() ? Plugin->GetBaseDir() / TEXT("Tests/Figma/frame.nodes.json")
                                            : FString();
      FString Text;
      TSharedPtr<FJsonObject> Response;
      if (!FFileHelper::LoadFileToString(Text, *Path) ||
          !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Response) ||
          !Response.IsValid())
      {
        AddError(FString::Printf(TEXT("can't read %s"), *Path));
        return false;
      }
      OutOptions.FileKey = TEXT("fixture");
      OutOptions.NodeId = TEXT("10:1");
      OutOptions.ReferenceSize = FIntPoint(400, 300);
      FString Error;
      if (!UIWTFigmaNormalize::Normalize(Response.ToSharedRef(), OutOptions, OutResult, Error))
      {
        AddError(TEXT("normalize failed: ") + Error);
        return false;
      }
      return true;
    }

    const FNode *Expect(const FDocument &InDoc, const FString &InId)
    {
      const FNode *Node = FindNode(InDoc.Root, InId);
      if (!Node)
      {
        AddError(FString::Printf(TEXT("no node %s"), *InId));
      }
      return Node;
    }

    void ExpectBox(const FNode *InNode, double InX, double InY, double InW, double InH)
    {
      if (InNode && !NearlyBox(InNode->Box, InX, InY, InW, InH))
      {
        AddError(FString::Printf(TEXT("%s box is %s, expected (%g, %g, %g, %g)"), *InNode->Id,
                                 *BoxText(InNode->Box), InX, InY, InW, InH));
      }
    }
  };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUIWTFigmaUrlTest, "UIWidgetTool.Figma.Url", TestFlags)

bool FUIWTFigmaUrlTest::RunTest(const FString &Parameters)
{
  struct FCase
  {
    const TCHAR *Url;
    const TCHAR *Key;
    const TCHAR *Node;
  };
  const FCase Good[] = {
      {TEXT("https://www.figma.com/design/AbC123/My-File?node-id=12-34&t=xyz-0"), TEXT("AbC123"),
       TEXT("12:34")},
      {TEXT("https://figma.com/file/AbC123/My-File?type=design&node-id=12%3A34"), TEXT("AbC123"),
       TEXT("12:34")},
      {TEXT("figma.com/design/AbC123/branch/BrK456/My-File?node-id=1-2"), TEXT("BrK456"),
       TEXT("1:2")},
      {TEXT("https://www.figma.com/proto/AbC123/My-File?node-id=5-6#frame"), TEXT("AbC123"),
       TEXT("5:6")},
  };
  for (const FCase &Case : Good)
  {
    UIWTFigmaClient::FFrameUrl Url;
    FString Error;
    if (TestTrue(FString::Printf(TEXT("parses %s"), Case.Url),
                 UIWTFigmaClient::ParseUrl(Case.Url, Url, Error)))
    {
      TestEqual(TEXT("file key"), Url.FileKey, FString(Case.Key));
      TestEqual(TEXT("node id"), Url.NodeId, FString(Case.Node));
    }
  }
  const TCHAR *Bad[] = {
      TEXT("https://www.figma.com/design/AbC123/My-File"),
      TEXT("https://example.com/design/AbC123/x?node-id=1-2"),
      TEXT("https://www.figma.com/community/file/123?node-id=1-2"),
      TEXT("https://www.figma.com/design/Ab..C/x?node-id=1-2"),
  };
  for (const TCHAR *Url : Bad)
  {
    UIWTFigmaClient::FFrameUrl Parsed;
    FString Error;
    TestFalse(FString::Printf(TEXT("refuses %s"), Url), UIWTFigmaClient::ParseUrl(Url, Parsed, Error));
  }
  TestEqual(TEXT("file-safe id"), UIWTFigmaNormalize::FileSafeId(TEXT("I12:34;5:6")),
            FString(TEXT("I12-34_5-6")));
  return true;
}

// Every mapping the fixture covers: boxes (groups re-based, rotation, render
// bounds), fills, strokes, text runs, auto layout, sizing, instances,
// rasterizing and the skipped hidden layer.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTFigmaNormalizeTest, FUIWTFigmaTestBase,
                                        "UIWidgetTool.Figma.Normalize", TestFlags)

bool FUIWTFigmaNormalizeTest::RunTest(const FString &Parameters)
{
  UIWTFigmaNormalize::FOptions Options;
  UIWTFigmaNormalize::FResult Result;
  if (!LoadFixture(Options, Result))
  {
    return false;
  }
  const FDocument &Doc = Result.Document;
  TestEqual(TEXT("source"), Doc.Source, FString(TEXT("figma")));
  TestTrue(TEXT("reference size"), Doc.ReferenceSize == FVector2D(400, 300));
  ExpectBox(&Doc.Root, 0, 0, 400, 300);
  TestTrue(TEXT("root clips"), Doc.Root.bClip);

  const FNode *Card = Expect(Doc, TEXT("10:2"));
  ExpectBox(Card, 20, 20, 100, 60);
  if (Card)
  {
    TestTrue(TEXT("card is a shape"), Card->Kind == EKind::Shape);
    TestTrue(TEXT("card fill: paint opacity in the alpha"),
             Card->Fill.IsValid() && Card->Fill->Color == FColor(255, 0, 0, 128));
    TestTrue(TEXT("card stroke"), Card->Stroke.IsValid() &&
                                      Card->Stroke->Align == EStrokeAlign::Outside &&
                                      Card->Stroke->Weights.L == 2.0);
    TestTrue(TEXT("card radii"), Card->Radii.IsSet() && Card->Radii->X == 8.0);
    TestTrue(TEXT("card constraint"), Card->bHasConstraints && Card->ConstraintH == EConstraint::Max);
  }

  const FNode *Group = Expect(Doc, TEXT("10:3"));
  ExpectBox(Group, 200, 50, 100, 100);
  ExpectBox(Expect(Doc, TEXT("10:4")), 50, 50, 50, 50);

  const FNode *Title = Expect(Doc, TEXT("10:5"));
  if (Title && TestTrue(TEXT("title text"), Title->Text.IsValid()))
  {
    const FTextBlock &Text = *Title->Text;
    TestEqual(TEXT("content"), Text.Content, FString(TEXT("Hello World")));
    if (TestEqual(TEXT("two runs"), Text.Runs.Num(), 2))
    {
      TestEqual(TEXT("run 0 style"), Text.Runs[0].Font.Style, FString(TEXT("Bold")));
      TestEqual(TEXT("run 0 length"), Text.Runs[0].Len, 6);
      TestEqual(TEXT("run 1 style"), Text.Runs[1].Font.Style, FString(TEXT("Regular")));
      TestEqual(TEXT("run 1 postscript"), Text.Runs[1].Font.PostScript, FString(TEXT("Inter-Regular")));
      TestEqual(TEXT("tracking px → 1/1000 em"), Text.Runs[0].Tracking, 50.0);
      TestTrue(TEXT("line height px"), Text.Runs[0].LineHeightPx.IsSet() &&
                                           *Text.Runs[0].LineHeightPx == 24.0);
      TestTrue(TEXT("colour"), Text.Runs[0].Color == FColor::White);
    }
    TestTrue(TEXT("align"), Text.Align == ETextAlign::Center);
    TestTrue(TEXT("HEIGHT → fixedWidth"), Text.Sizing == ETextSizing::FixedWidth);
    TestTrue(TEXT("text stroke"), Title->Stroke.IsValid() &&
                                      Title->Stroke->Align == EStrokeAlign::Outside &&
                                      Title->Stroke->Weights.T == 3.0 &&
                                      Title->Stroke->Color == FColor::Black);
    if (TestEqual(TEXT("text shadow"), Title->Effects.Num(), 1))
    {
      const FEffect &Shadow = Title->Effects[0];
      TestEqual(TEXT("shadow type"), Shadow.Type, FString(TEXT("dropShadow")));
      TestFalse(TEXT("text shadow not baked"), Shadow.bBaked);
      TestTrue(TEXT("shadow colour, alpha kept"), Shadow.Color.IsSet() && *Shadow.Color == FColor(0, 0, 0, 64));
      TestTrue(TEXT("shadow offset"), Shadow.Offset == FVector2D(1.0, 4.0));
      TestEqual(TEXT("shadow blur"), Shadow.Radius, 4.0);
    }
  }

  const FNode *Icon = Expect(Doc, TEXT("10:6"));
  ExpectBox(Icon, 349, 19, 26, 26);
  if (Icon)
  {
    TestTrue(TEXT("vector rendered"), Icon->Kind == EKind::Image && Icon->Image.IsValid() &&
                                          Icon->Image->Origin == EImageOrigin::Rendered &&
                                          Icon->Image->Path == TEXT("render/10-6.png"));
  }

  const FNode *Row = Expect(Doc, TEXT("10:7"));
  ExpectBox(Row, 20, 200, 200, 40);
  if (Row && TestTrue(TEXT("row layout"), Row->Layout.IsValid()))
  {
    TestTrue(TEXT("horizontal"), Row->Layout->Mode == ELayoutMode::Horizontal && !Row->Layout->bWrap);
    TestEqual(TEXT("spacing"), Row->Layout->Spacing, 8.0);
    TestEqual(TEXT("padding"), Row->Layout->Padding.L, 4.0);
    TestTrue(TEXT("align"), Row->Layout->AlignMain == EAlignMain::Center &&
                                Row->Layout->AlignCross == EAlignCross::Center);
    TestTrue(TEXT("hug width"), Row->SizingH == ESizing::Hug && Row->SizingV == ESizing::Fixed);
    TestFalse(TEXT("an empty fills array is no fill"), Row->Fill.IsValid());
  }
  const FNode *Photo = Expect(Doc, TEXT("10:8"));
  if (Photo)
  {
    TestTrue(TEXT("image fill leaf"), Photo->Kind == EKind::Image && Photo->Image.IsValid() &&
                                          Photo->Image->Origin == EImageOrigin::Fill &&
                                          Photo->Image->Path == TEXT("images/abc123_fill_1000.png"));
  }
  const FNode *Button = Expect(Doc, TEXT("10:9"));
  if (Button && TestTrue(TEXT("instance"), Button->Kind == EKind::Instance && Button->Component.IsValid()))
  {
    TestEqual(TEXT("component key"), Button->Component->Key, FString(TEXT("btnkey")));
    TestEqual(TEXT("variant name"), Button->Component->Name, FString(TEXT("Button State=Default")));
    TestFalse(TEXT("a width change isn't an override"), Button->Component->bHasOverrides);
    TestTrue(TEXT("fill width"), Button->SizingH == ESizing::Fill);
    TestTrue(TEXT("variant hint"), Button->Hints.Contains(TEXT("variant:State=Default")));
    TestNotNull(TEXT("instance child keeps its compound id"), FindNode(*Button, TEXT("I10:9;20:2")));
  }
  const FNode *Arrow = Expect(Doc, TEXT("10:10"));
  ExpectBox(Arrow, 130, 8, 16, 16);
  TestTrue(TEXT("render bounds clipped in a layout"), HasNote(Doc, TEXT("10:10"), TEXT("renderClipped")));

  const FNode *Rotated = Expect(Doc, TEXT("10:11"));
  ExpectBox(Rotated, 270, 230, 80, 20);
  if (Rotated)
  {
    TestTrue(FString::Printf(TEXT("rotation %g, expected -90 (Figma turns counterclockwise)"),
                             Rotated->Rotation),
             FMath::IsNearlyEqual(Rotated->Rotation, -90.0, 0.01));
  }
  const FNode *Dot = Expect(Doc, TEXT("10:12"));
  if (Dot)
  {
    TestTrue(TEXT("circle as a rounded shape"), Dot->Kind == EKind::Shape && Dot->Radii.IsSet() &&
                                                    Dot->Radii->X == 5.0);
  }
  TestNull(TEXT("hidden layer that needs rendering is left out"), FindNode(Doc.Root, TEXT("10:13")));
  TestTrue(TEXT("and noted"), HasNote(Doc, TEXT("10:13"), TEXT("hiddenSkipped")));

  // Jobs: the two rendered vectors (one with the layer's box) and the fill.
  TestEqual(TEXT("image jobs"), Result.Images.Num(), 3);
  for (const UIWTFigmaNormalize::FImageJob &Job : Result.Images)
  {
    if (Job.RenderId == TEXT("10:10"))
    {
      TestTrue(TEXT("flow vector uses absolute bounds"), Job.bAbsoluteBounds);
    }
    if (Job.RenderId == TEXT("10:6"))
    {
      TestFalse(TEXT("canvas vector uses render bounds"), Job.bAbsoluteBounds);
    }
  }

  // The normalized tree reads back through the schema.
  FDocument Again;
  TArray<FString> Errors;
  TestTrue(TEXT("round trip"), ReadDocument(WriteDocumentString(Doc), Again, Errors));
  for (const FString &Error : Errors)
  {
    AddError(Error);
  }
  return true;
}

// FinishImages: a FILL image is cropped to its box's shape, renders keep
// their pixels, sizes and scales are filled in, a missing download is
// noted, and reference.png comes down to design pixels.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTFigmaImagesTest, FUIWTFigmaTestBase,
                                        "UIWidgetTool.Figma.Images", TestFlags)

bool FUIWTFigmaImagesTest::RunTest(const FString &Parameters)
{
  UIWTFigmaNormalize::FOptions Options;
  UIWTFigmaNormalize::FResult Result;
  if (!LoadFixture(Options, Result))
  {
    return false;
  }
  const FString Directory = FPaths::ConvertRelativePathToFull(
      FPaths::AutomationTransientDir() / TEXT("UIWTFigma") /
      FGuid::NewGuid().ToString(EGuidFormats::Digits));
  auto WriteImage = [&](const FString &InRelative, int32 InW, int32 InH)
  {
    FImage Image(InW, InH, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
    for (FColor &Pixel : Image.AsBGRA8())
    {
      Pixel = FColor::Orange;
    }
    FString Error;
    if (!UIWTRunImages::SavePng(Image, Directory / InRelative, Error))
    {
      AddError(Error);
    }
  };
  WriteImage(UIWTFigmaNormalize::RenderFile(TEXT("10:6")), 52, 52);
  WriteImage(UIWTFigmaNormalize::SourceImageFile(TEXT("abc123")), 100, 50);
  WriteImage(UIWTFigmaNormalize::ReferenceRenderFile(), 800, 600);
  // 10:10's render is missing on purpose.

  UIWTFigmaNormalize::FinishImages(Result, Directory, Options);
  UIWTFigmaNormalize::WriteReference(Directory, Options);
  const FDocument &Doc = Result.Document;
  const FNode *Photo = FindNode(Doc.Root, TEXT("10:8"));
  if (Photo && Photo->Image.IsValid())
  {
    TestTrue(TEXT("photo cropped square"),
             Photo->Image->Size.IsSet() && *Photo->Image->Size == FVector2D(50, 50));
    TestTrue(FString::Printf(TEXT("photo scale %g"), Photo->Image->Scale),
             FMath::IsNearlyEqual(Photo->Image->Scale, 50.0 / 32.0, 0.001));
    TestTrue(TEXT("photo written"), IFileManager::Get().FileExists(*(Directory / Photo->Image->Path)));
  }
  const FNode *Icon = FindNode(Doc.Root, TEXT("10:6"));
  if (Icon && Icon->Image.IsValid())
  {
    TestTrue(TEXT("render size"), Icon->Image->Size.IsSet() && *Icon->Image->Size == FVector2D(52, 52));
    TestTrue(FString::Printf(TEXT("render scale %g"), Icon->Image->Scale),
             FMath::IsNearlyEqual(Icon->Image->Scale, 2.0, 0.001));
  }
  TestTrue(TEXT("missing render noted"), HasNote(Doc, TEXT("10:10"), TEXT("imageMissing")));
  FImage Reference;
  FString Error;
  TestTrue(TEXT("reference.png at design pixels"),
           UIWTRunImages::LoadImageFile(Directory / TEXT("reference.png"), Reference, Error) &&
               Reference.SizeX == 400 && Reference.SizeY == 300);
  IFileManager::Get().DeleteDirectory(*Directory, false, true);
  return true;
}

// The fonts step before a Figma import's conversion (UIWTDesignFonts): which
// fonts a design uses, and how a Figma style becomes Google Fonts' weight and
// slant. Downloading needs the network and isn't tested here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUIWTFigmaFontsTest, "UIWidgetTool.Figma.Fonts", TestFlags)

bool FUIWTFigmaFontsTest::RunTest(const FString &Parameters)
{
  struct FStyleCase
  {
    const TCHAR *Style;
    int32 Weight;
    bool bItalic;
  };
  const FStyleCase Good[] = {
      {TEXT("Regular"), 400, false},        {TEXT(""), 400, false},
      {TEXT("Italic"), 400, true},          {TEXT("Semi Bold"), 600, false},
      {TEXT("SemiBold Italic"), 600, true}, {TEXT("Extra Light"), 200, false},
      {TEXT("Black"), 900, false},          {TEXT("Bold Italic"), 700, true},
      {TEXT("300"), 300, false},
  };
  for (const FStyleCase &Case : Good)
  {
    int32 Weight = 0;
    bool bItalic = false;
    if (TestTrue(FString::Printf(TEXT("parses '%s'"), Case.Style),
                 UIWTDesignFonts::ParseStyle(Case.Style, Weight, bItalic)))
    {
      TestEqual(FString::Printf(TEXT("'%s' weight"), Case.Style), Weight, Case.Weight);
      TestEqual(FString::Printf(TEXT("'%s' italic"), Case.Style), bItalic, Case.bItalic);
    }
  }
  for (const TCHAR *Style : {TEXT("Condensed Bold"), TEXT("Display")})
  {
    int32 Weight = 0;
    bool bItalic = false;
    TestFalse(FString::Printf(TEXT("refuses '%s'"), Style),
              UIWTDesignFonts::ParseStyle(Style, Weight, bItalic));
  }

  auto TextNode = [](const TCHAR *InId, const TArray<FFontRef> &InFonts)
  {
    FNode Node;
    Node.Id = InId;
    Node.Kind = EKind::Text;
    const TSharedRef<FTextBlock> Text = MakeShared<FTextBlock>();
    for (const FFontRef &Font : InFonts)
    {
      Text->Runs.AddDefaulted_GetRef().Font = Font;
    }
    Node.Text = Text;
    return Node;
  };
  auto Font = [](const TCHAR *InFamily, const TCHAR *InStyle)
  {
    FFontRef Ref;
    Ref.Family = InFamily;
    Ref.Style = InStyle;
    return Ref;
  };
  FDocument Doc;
  Doc.Root.Id = TEXT("1:1");
  Doc.Root.Children.Add(
      TextNode(TEXT("1:2"), {Font(TEXT("Inter"), TEXT("Semi Bold")), Font(TEXT("Inter"), TEXT("Regular"))}));
  // Spelled differently, still the same font.
  Doc.Root.Children.Add(TextNode(TEXT("1:3"), {Font(TEXT("inter"), TEXT("SemiBold"))}));
  FComponentDef Component;
  // A run without a family is left out.
  Component.Root =
      TextNode(TEXT("2:1"), {Font(TEXT("Open Sans"), TEXT("Bold")), Font(TEXT(""), TEXT("Bold"))});
  Doc.Components.Add(TEXT("key"), Component);
  const TArray<FFontRef> Fonts = UIWTDesignFonts::CollectFonts(Doc);
  if (TestEqual(TEXT("distinct fonts, screen and components"), Fonts.Num(), 3))
  {
    TestEqual(TEXT("first"), Fonts[0].Family + TEXT(" ") + Fonts[0].Style,
              FString(TEXT("Inter Semi Bold")));
    TestEqual(TEXT("second"), Fonts[1].Style, FString(TEXT("Regular")));
    TestEqual(TEXT("component's"), Fonts[2].Family, FString(TEXT("Open Sans")));
  }
  TestNull(TEXT("no library font for an unknown family"),
           UIWTDesignFonts::FindLibraryFont(TEXT("No Such Family 12345")));

  // An installed font's names, read from one of the engine's own files.
  const TArray<UIWTDesignFonts::FInstalledFace> Read = UIWTDesignFonts::ReadFontFile(
      FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-BoldItalic.ttf"));
  if (TestEqual(TEXT("one face in a .ttf"), Read.Num(), 1))
  {
    TestEqual(TEXT("family"), Read[0].Family, FString(TEXT("Roboto")));
    TestEqual(TEXT("style"), Read[0].Style, FString(TEXT("Bold Italic")));
    TestEqual(TEXT("PostScript name"), Read[0].PostScript, FString(TEXT("Roboto-BoldItalic")));
    TestFalse(TEXT("static"), Read[0].bVariable);
    TestFalse(TEXT("may be embedded"), Read[0].IsRestricted());
  }
  TestEqual(TEXT("not a font file"),
            UIWTDesignFonts::ReadFontFile(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto.tps"))
                .Num(),
            0);

  // Matching installed faces: PostScript name first, then names; variable
  // and restricted faces are skipped with a reason.
  auto Face = [](const TCHAR *InFamily, const TCHAR *InStyle, const TCHAR *InLegacyFamily,
                 const TCHAR *InLegacyStyle, const TCHAR *InPostScript)
  {
    UIWTDesignFonts::FInstalledFace Installed;
    Installed.File = FString(TEXT("C:/Fonts/")) + InPostScript + TEXT(".ttf");
    Installed.Family = InFamily;
    Installed.Style = InStyle;
    Installed.LegacyFamily = InLegacyFamily;
    Installed.LegacyStyle = InLegacyStyle;
    Installed.PostScript = InPostScript;
    return Installed;
  };
  TArray<UIWTDesignFonts::FInstalledFace> Faces = {
      Face(TEXT("Inter"), TEXT("Semi Bold"), TEXT("Inter SemiBold"), TEXT("Regular"),
           TEXT("Inter-SemiBold")),
      // Only legacy names, as some older fonts have.
      Face(TEXT("Segoe UI Semibold"), TEXT("Regular"), TEXT("Segoe UI Semibold"),
           TEXT("Regular"), TEXT("SegoeUI-Semibold")),
      Face(TEXT("Lato"), TEXT("Regular"), TEXT("Lato"), TEXT("Regular"), TEXT("Lato-Regular")),
      Face(TEXT("Brand"), TEXT("Bold"), TEXT("Brand"), TEXT("Bold"), TEXT("Brand-Bold")),
  };
  Faces[2].bVariable = true;
  Faces[3].FsType = 0x0002;
  auto Find = [&Faces](const TCHAR *InFamily, const TCHAR *InStyle, const TCHAR *InPostScript,
                       FString &OutWhyNot) -> FString
  {
    FFontRef Ref;
    Ref.Family = InFamily;
    Ref.Style = InStyle;
    Ref.PostScript = InPostScript;
    const UIWTDesignFonts::FInstalledFace *Found =
        UIWTDesignFonts::FindInstalledFace(Faces, Ref, OutWhyNot);
    return Found ? Found->PostScript : FString();
  };
  FString WhyNot;
  TestEqual(TEXT("by PostScript name"), Find(TEXT("Whatever"), TEXT(""), TEXT("inter-semibold"), WhyNot),
            FString(TEXT("Inter-SemiBold")));
  TestEqual(TEXT("by typographic names"), Find(TEXT("Inter"), TEXT("SemiBold"), TEXT(""), WhyNot),
            FString(TEXT("Inter-SemiBold")));
  TestEqual(TEXT("by legacy names"), Find(TEXT("Segoe UI"), TEXT("Semibold"), TEXT(""), WhyNot),
            FString(TEXT("SegoeUI-Semibold")));
  TestEqual(TEXT("other style"), Find(TEXT("Inter"), TEXT("Bold"), TEXT(""), WhyNot), FString());
  WhyNot.Reset();
  TestEqual(TEXT("variable skipped"), Find(TEXT("Lato"), TEXT("Regular"), TEXT(""), WhyNot), FString());
  TestTrue(TEXT("variable reason"), WhyNot.Contains(TEXT("variable")));
  WhyNot.Reset();
  TestEqual(TEXT("other weight of a variable font"), Find(TEXT("Lato"), TEXT("Bold"), TEXT(""), WhyNot),
            FString());
  TestTrue(TEXT("other weight's reason"), WhyNot.Contains(TEXT("variable")));
  WhyNot.Reset();
  TestEqual(TEXT("restricted skipped"), Find(TEXT("Brand"), TEXT("Bold"), TEXT(""), WhyNot), FString());
  TestTrue(TEXT("restricted reason"), WhyNot.Contains(TEXT("embedding")));
  return true;
}

#endif
