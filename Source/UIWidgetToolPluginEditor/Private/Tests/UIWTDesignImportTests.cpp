#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Core/UIWTDesignImport.h"
#include "Core/UIWTDesignSettings.h"
#include "Core/UIWTGeneratedBlueprints.h"
#include "Core/UIWTRunImages.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "WidgetBlueprint.h"

// Tests for the shared design import (import-tree.md → Implementation order,
// Phase 2). Each test writes its design.json (and images) to the automation
// transient folder and imports into /Temp without saving. The render tests
// need a GPU device and are skipped under -nullrhi; run them with
//   UnrealEditor-Cmd.exe <project> -ExecCmds="Automation RunTests UIWidgetTool.DesignImport; Quit" -unattended
namespace
{
  using namespace UIWTDesignTree;

  constexpr EAutomationTestFlags TestFlags =
      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

  class FUIWTImportTestBase : public FAutomationTestBase
  {
  public:
    FUIWTImportTestBase(const FString &InName, const bool bInComplexTask)
        : FAutomationTestBase(InName, bInComplexTask)
    {
    }
    virtual bool SuppressLogErrors() override { return true; }
    virtual bool SuppressLogWarnings() override { return true; }

  protected:
    FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);

    FString DesignDir() const
    {
      return FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() /
                                               TEXT("UIWTDesignImport") / Id);
    }

    FString TargetFolder() const { return TEXT("/Temp/UIWTImportTest_") + Id; }

    // Writes design.json and returns its path.
    FString WriteDesign(const FString &InJson)
    {
      const FString Path = DesignDir() / TEXT("design.json");
      if (!FFileHelper::SaveStringToFile(InJson, *Path))
      {
        AddError(FString::Printf(TEXT("can't write %s"), *Path));
      }
      return Path;
    }

    void WritePng(const FString &InRelative, int32 InW, int32 InH, FColor InColor)
    {
      FImage Image(InW, InH, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
      for (FColor &Pixel : Image.AsBGRA8())
      {
        Pixel = InColor;
      }
      FString Error;
      if (!UIWTRunImages::SavePng(Image, DesignDir() / InRelative, Error))
      {
        AddError(Error);
      }
    }

    bool ImportDesign(const FString &InJson, UIWTDesignImport::FResult &OutResult,
                      const FString &InName = TEXT("WBP_Test"))
    {
      UIWTDesignImport::FRequest Request;
      Request.DesignFile = WriteDesign(InJson);
      Request.TargetFolder = TargetFolder();
      Request.BlueprintName = InName;
      Request.ComponentsFolder = TargetFolder() / TEXT("Components");
      Request.bSave = false;
      FString Error;
      const bool bImported = UIWTDesignImport::Import(Request, OutResult, Error);
      if (!bImported)
      {
        AddError(TEXT("import failed: ") + Error);
      }
      for (const FString &ApplyError : OutResult.ApplyErrors)
      {
        AddError(TEXT("Apply: ") + ApplyError);
      }
      return bImported;
    }

    void CleanUp()
    {
      IFileManager::Get().DeleteDirectory(*DesignDir(), false, true);
      const FString OnDisk = UIWTGenerated::GetFolderOnDisk(TargetFolder());
      if (!OnDisk.IsEmpty())
      {
        IFileManager::Get().DeleteDirectory(*OnDisk, false, true);
      }
    }

    // --- Rendering -----------------------------------------------------

    bool CanRender()
    {
      if (!FApp::CanEverRender())
      {
        AddInfo(TEXT("skipped: this editor can't render (-nullrhi)"));
        return false;
      }
      return true;
    }

    bool Render(UWidgetBlueprint *InBlueprint, FIntPoint InSize, FImage &OutImage)
    {
      FString Error;
      if (!UIWTRunImages::RenderWidget(InBlueprint, InSize, OutImage, Error))
      {
        AddError(TEXT("render failed: ") + Error);
        return false;
      }
      return true;
    }

    FColor Pixel(const FImage &InImage, int32 InX, int32 InY)
    {
      if (InX < 0 || InY < 0 || InX >= InImage.SizeX || InY >= InImage.SizeY)
      {
        return FColor::Transparent;
      }
      return InImage.AsBGRA8()[int64(InY) * InImage.SizeX + InX];
    }

    // The pixel is InExpected, within a tolerance for filtering and gamma.
    void ExpectPixel(const FString &InWhat, const FImage &InImage, int32 InX, int32 InY,
                     FColor InExpected, int32 InTolerance = 48)
    {
      const FColor Got = Pixel(InImage, InX, InY);
      const bool bNear = FMath::Abs(Got.R - InExpected.R) <= InTolerance &&
                         FMath::Abs(Got.G - InExpected.G) <= InTolerance &&
                         FMath::Abs(Got.B - InExpected.B) <= InTolerance;
      if (!bNear)
      {
        AddError(FString::Printf(TEXT("%s: pixel (%d, %d) at %dx%d is %s, expected %s"),
                                 *InWhat, InX, InY, InImage.SizeX, InImage.SizeY,
                                 *Got.ToString(), *InExpected.ToString()));
      }
    }
  };

  TSharedPtr<FJsonObject> ReadJsonFile(const FString &InPath)
  {
    FString Text;
    TSharedPtr<FJsonObject> Object;
    if (FFileHelper::LoadFileToString(Text, *InPath))
    {
      FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
    }
    return Object;
  }
}

// A first import: textures, the blueprint, inline instances, the design
// size, the reference image and the sidecar; a second import to the same
// place is refused.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignImportTest, FUIWTImportTestBase,
                                        "UIWidgetTool.DesignImport.Import", TestFlags)

bool FUIWTDesignImportTest::RunTest(const FString &Parameters)
{
  WritePng(TEXT("images/icon.png"), 16, 16, FColor::Red);
  WritePng(TEXT("reference.png"), 32, 18, FColor::Black);
  const FString Json = TEXT(R"json({
    "version": 1, "source": "figma",
    "sourceRef": { "fileKey": "test", "nodeId": "1:1" },
    "referenceSize": { "w": 640, "h": 360 },
    "root": {
      "id": "1:1", "name": "Import Test", "kind": "frame",
      "box": { "x": 0, "y": 0, "w": 640, "h": 360 },
      "fill": { "color": "#101820" },
      "children": [
        { "id": "1:2", "name": "Icon", "kind": "image", "box": { "x": 10, "y": 10, "w": 32, "h": 32 },
          "image": { "path": "images/icon.png", "scale": 0.5, "origin": "fill", "size": { "w": 16, "h": 16 } } },
        { "id": "1:3", "name": "Title", "kind": "text", "box": { "x": 60, "y": 10, "w": 200, "h": 30 },
          "text": { "content": "Hello", "runs": [ { "font": { "family": "Unmapped" }, "size": 24, "color": "#FFFFFF" } ] } },
        { "id": "1:4", "name": "Button", "kind": "instance", "box": { "x": 10, "y": 60, "w": 120, "h": 40 },
          "component": { "key": "btn", "name": "Button", "hasOverrides": false },
          "layout": { "mode": "horizontal", "padding": { "l": 8, "t": 8, "r": 8, "b": 8 } },
          "fill": { "color": "#3366FF" },
          "children": [
            { "id": "I1:4;2:1", "name": "Label", "kind": "text", "box": { "x": 8, "y": 8, "w": 104, "h": 24 },
              "text": { "content": "OK", "runs": [ { "font": { "family": "Unmapped" }, "size": 16, "color": "#FFFFFF" } ] } }
          ] }
      ]
    }
  })json");

  UIWTDesignImport::FResult Result;
  if (!ImportDesign(Json, Result))
  {
    CleanUp();
    return false;
  }
  UWidgetBlueprint *Blueprint = Result.Blueprint;
  TestNotNull(TEXT("blueprint"), Blueprint);
  TestEqual(TEXT("root mode"), static_cast<int32>(Result.RootMode),
            static_cast<int32>(ERootMode::Screen));
  TestTrue(TEXT("design size"), Result.DesignSize == FIntPoint(640, 360));
  TestEqual(TEXT("one texture"), Result.TexturesWritten, 1);
  TestFalse(TEXT("not saved"), Result.bSaved);
  if (Blueprint && Blueprint->GeneratedClass)
  {
    const UUserWidget *Defaults = Blueprint->GeneratedClass->GetDefaultObject<UUserWidget>();
    TestEqual(TEXT("design size mode"), static_cast<int32>(Defaults->DesignSizeMode),
              static_cast<int32>(EDesignPreviewSizeMode::Custom));
    TestTrue(TEXT("design time size"), Defaults->DesignTimeSize == FVector2D(640, 360));
    TestNotNull(TEXT("Title widget"), Blueprint->WidgetTree->FindWidget(TEXT("Title")));
    // The instance has no overrides: a child WBP, built from the instance
    // since the tree has no main component for it.
    TestNull(TEXT("the label isn't in the screen"), Blueprint->WidgetTree->FindWidget(TEXT("Label")));
    const UWidgetBlueprint *Child = LoadObject<UWidgetBlueprint>(
        nullptr, *(TargetFolder() / TEXT("Components/WBP_Button/WBP_Button.WBP_Button")));
    TestTrue(TEXT("child WBP with the label"),
             Child && Child->WidgetTree->FindWidget(TEXT("Label")) != nullptr);
    TestTrue(TEXT("componentFallback reported"),
             Result.Report.ContainsByPredicate([](const FReportEntry &Entry)
                                               { return Entry.Category == TEXT("componentFallback"); }));
  }

  // The sidecar.
  const TSharedPtr<FJsonObject> Sidecar = ReadJsonFile(Result.SidecarFile);
  if (TestTrue(TEXT("sidecar written"), Sidecar.IsValid()))
  {
    const TSharedPtr<FJsonObject> *Owned = nullptr;
    const TSharedPtr<FJsonObject> *Widgets = nullptr;
    const TSharedPtr<FJsonObject> *Textures = nullptr;
    TestTrue(TEXT("owned"), Sidecar->TryGetObjectField(TEXT("owned"), Owned) &&
                                (*Owned)->HasField(TEXT("1:3")));
    TestTrue(TEXT("widgets"), Sidecar->TryGetObjectField(TEXT("widgets"), Widgets) &&
                                  (*Widgets)->HasField(TEXT("Title")));
    FString TexturePath;
    if (TestTrue(TEXT("textures"),
                 Sidecar->TryGetObjectField(TEXT("textures"), Textures) &&
                     (*Textures)->TryGetStringField(TEXT("images/icon.png"), TexturePath)))
    {
      TestNotNull(TEXT("texture asset"), LoadObject<UTexture2D>(nullptr, *TexturePath));
    }
    if (Widgets)
    {
      const TSharedPtr<FJsonObject> *Title = nullptr;
      FString Parent;
      TestTrue(TEXT("Title has class and parent"),
               (*Widgets)->TryGetObjectField(TEXT("Title"), Title) &&
                   (*Title)->GetStringField(TEXT("class")) == TEXT("TextBlock") &&
                   (*Title)->TryGetStringField(TEXT("parent"), Parent) && !Parent.IsEmpty());
    }
  }
  TestTrue(TEXT("reference copied"),
           !Result.ReferenceFile.IsEmpty() &&
               IFileManager::Get().FileExists(*Result.ReferenceFile));

  // No merge yet: the same target is refused.
  UIWTDesignImport::FRequest Again;
  Again.DesignFile = DesignDir() / TEXT("design.json");
  Again.TargetFolder = TargetFolder();
  Again.BlueprintName = TEXT("WBP_Test");
  Again.bSave = false;
  UIWTDesignImport::FResult AgainResult;
  FString Error;
  TestFalse(TEXT("existing blueprint refused"),
            UIWTDesignImport::Import(Again, AgainResult, Error));
  TestTrue(TEXT("refusal says why"), Error.Contains(TEXT("already exists")));

  CleanUp();
  return true;
}

// The font map: a family maps with the typeface named like the style, an
// unknown style falls back to Regular, and line heights are measured. A
// family the map doesn't list uses the font library's Font.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignFontsTest, FUIWTImportTestBase,
                                        "UIWidgetTool.DesignImport.Fonts", TestFlags)

bool FUIWTDesignFontsTest::RunTest(const FString &Parameters)
{
  UUIWTDesignSettings *Settings = GetMutableDefault<UUIWTDesignSettings>();
  const TArray<FUIWTFontMapping> Saved = Settings->Fonts;
  FUIWTFontMapping Mapping;
  Mapping.Family = TEXT("Inter");
  Mapping.Font = TSoftObjectPtr<UFont>(FSoftObjectPath(TEXT("/Engine/EngineFonts/Roboto.Roboto")));
  Settings->Fonts = {Mapping};

  FConvertOptions Options;
  UIWTDesignImport::SetFontResolver(Options);
  const TOptional<FResolvedFont> Bold = Options.ResolveFont({TEXT("inter"), TEXT("Bold"), {}});
  const TOptional<FResolvedFont> Odd =
      Options.ResolveFont({TEXT("Inter"), TEXT("Ultra Wide"), {}});
  const TOptional<FResolvedFont> Unmapped =
      Options.ResolveFont({TEXT("Comic"), TEXT("Regular"), {}});
  // Not in the font map: the family's Font from the font library.
  const FDirectoryPath SavedLibrary = Settings->FontLibraryFolder;
  Settings->FontLibraryFolder.Path = TEXT("/Engine/EngineFonts");
  FConvertOptions LibraryOptions;
  UIWTDesignImport::SetFontResolver(LibraryOptions);
  const TOptional<FResolvedFont> Library =
      LibraryOptions.ResolveFont({TEXT("Roboto"), TEXT("Bold Italic"), {}});
  Settings->FontLibraryFolder = SavedLibrary;
  Settings->Fonts = Saved;

  if (TestTrue(TEXT("family matches ignoring case"), Bold.IsSet()))
  {
    TestEqual(TEXT("font object"), Bold->FontObject,
              FString(TEXT("/Engine/EngineFonts/Roboto.Roboto")));
    TestEqual(TEXT("typeface from style"), Bold->Typeface.ToString(), FString(TEXT("Bold")));
    const double Natural = Options.NaturalLineHeight(Bold, 16.0);
    AddInfo(FString::Printf(TEXT("Roboto Bold at 16 px: natural line height %.1f px"), Natural));
    TestTrue(TEXT("line height measured"), Natural > 16.0 && Natural < 32.0);
  }
  if (TestTrue(TEXT("unknown style still maps"), Odd.IsSet()))
  {
    TestEqual(TEXT("falls back to Regular"), Odd->Typeface.ToString(), FString(TEXT("Regular")));
  }
  TestFalse(TEXT("unmapped family"), Unmapped.IsSet());
  if (TestTrue(TEXT("library font found"), Library.IsSet()))
  {
    TestEqual(TEXT("library font object"), Library->FontObject,
              FString(TEXT("/Engine/EngineFonts/Roboto.Roboto")));
    TestEqual(TEXT("library typeface from style"), Library->Typeface.ToString(),
              FString(TEXT("Bold Italic")));
  }
  TestTrue(TEXT("default font measured"), Options.NaturalLineHeight({}, 20.0) > 20.0);
  return true;
}

// Canvas constraints: children land where the constraints put them at two
// sizes, a group with opacity keeps its children's constraints against the
// frame, and a hug button follows its content.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignRenderConstraintsTest, FUIWTImportTestBase,
                                        "UIWidgetTool.DesignImport.Render.Constraints",
                                        TestFlags)

bool FUIWTDesignRenderConstraintsTest::RunTest(const FString &Parameters)
{
  if (!CanRender())
  {
    return true;
  }
  const FString Json = TEXT(R"json({
    "version": 1, "source": "figma", "referenceSize": { "w": 400, "h": 300 },
    "root": {
      "id": "c:root", "name": "Constraints", "kind": "frame",
      "box": { "x": 0, "y": 0, "w": 400, "h": 300 }, "fill": { "color": "#000000" },
      "children": [
        { "id": "c:r", "name": "Right", "kind": "shape", "box": { "x": 370, "y": 10, "w": 20, "h": 20 },
          "constraints": { "h": "right", "v": "top" }, "fill": { "color": "#FF0000" } },
        { "id": "c:g", "name": "Center", "kind": "shape", "box": { "x": 190, "y": 140, "w": 20, "h": 20 },
          "constraints": { "h": "center", "v": "center" }, "fill": { "color": "#00FF00" } },
        { "id": "c:b", "name": "Scale", "kind": "shape", "box": { "x": 100, "y": 200, "w": 100, "h": 20 },
          "constraints": { "h": "scale", "v": "top" }, "fill": { "color": "#0000FF" } },
        { "id": "c:grp", "name": "Faded", "kind": "group", "box": { "x": 300, "y": 240, "w": 80, "h": 40 },
          "opacity": 0.5,
          "children": [
            { "id": "c:y", "name": "Yellow", "kind": "shape", "box": { "x": 40, "y": 10, "w": 20, "h": 20 },
              "constraints": { "h": "right", "v": "top" }, "fill": { "color": "#FFFF00" } }
          ] },
        { "id": "c:btn", "name": "Button", "kind": "frame", "box": { "x": 20, "y": 240, "w": 60, "h": 40 },
          "sizing": { "h": "hug", "v": "hug" }, "fill": { "color": "#FFFFFF" },
          "layout": { "mode": "horizontal", "padding": { "l": 10, "t": 10, "r": 10, "b": 10 } },
          "children": [
            { "id": "c:icon", "name": "Icon", "kind": "shape", "box": { "x": 10, "y": 10, "w": 40, "h": 20 },
              "fill": { "color": "#FF00FF" } }
          ] }
      ]
    }
  })json");
  UIWTDesignImport::FResult Result;
  if (!ImportDesign(Json, Result))
  {
    CleanUp();
    return false;
  }
  FImage Small;
  FImage Large;
  if (Render(Result.Blueprint, FIntPoint(400, 300), Small) &&
      Render(Result.Blueprint, FIntPoint(600, 400), Large))
  {
    ExpectPixel(TEXT("right, 400"), Small, 380, 20, FColor::Red);
    ExpectPixel(TEXT("right, 600"), Large, 580, 20, FColor::Red);
    ExpectPixel(TEXT("center, 400"), Small, 200, 150, FColor::Green);
    ExpectPixel(TEXT("center, 600x400"), Large, 300, 200, FColor::Green);
    ExpectPixel(TEXT("scale, 400, inside"), Small, 190, 210, FColor::Blue);
    ExpectPixel(TEXT("scale, 400, past the end"), Small, 210, 210, FColor::Black);
    ExpectPixel(TEXT("scale, 600, stretched"), Large, 290, 210, FColor::Blue);
    // In the frame's coordinates the yellow square is at 340..360; the
    // group's opacity halves it, whatever the blending space.
    const FColor YellowSmall = Pixel(Small, 350, 260);
    const FColor YellowLarge = Pixel(Large, 550, 260);
    TestTrue(FString::Printf(TEXT("group child, 400 (%s)"), *YellowSmall.ToString()),
             YellowSmall.R > 60 && YellowSmall.R < 230 && YellowSmall.B < 48);
    TestTrue(FString::Printf(TEXT("group child follows the frame, 600 (%s)"),
                             *YellowLarge.ToString()),
             YellowLarge.R > 60 && YellowLarge.R < 230 && YellowLarge.B < 48);
    ExpectPixel(TEXT("hug button padding"), Small, 23, 243, FColor::White);
    ExpectPixel(TEXT("hug button content"), Small, 50, 260, FColor::Magenta);
    ExpectPixel(TEXT("hug button right padding"), Small, 77, 260, FColor::White);
    ExpectPixel(TEXT("hug button ends"), Small, 84, 260, FColor::Black);
  }
  CleanUp();
  return true;
}

// Strokes grow outward without moving siblings, in a canvas and in an
// H/VBox, and a stroked frame that clips cuts its content at its own box.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignRenderStrokesTest, FUIWTImportTestBase,
                                        "UIWidgetTool.DesignImport.Render.Strokes", TestFlags)

bool FUIWTDesignRenderStrokesTest::RunTest(const FString &Parameters)
{
  if (!CanRender())
  {
    return true;
  }
  const FString Json = TEXT(R"json({
    "version": 1, "source": "figma", "referenceSize": { "w": 400, "h": 300 },
    "root": {
      "id": "s:root", "name": "Strokes", "kind": "frame",
      "box": { "x": 0, "y": 0, "w": 400, "h": 300 }, "fill": { "color": "#000000" },
      "children": [
        { "id": "s:out", "name": "Outside", "kind": "shape", "box": { "x": 50, "y": 50, "w": 40, "h": 40 },
          "fill": { "color": "#0000FF" },
          "stroke": { "color": "#FFFFFF", "weights": { "l": 4, "t": 4, "r": 4, "b": 4 }, "align": "outside" } },
        { "id": "s:center", "name": "Center", "kind": "shape", "box": { "x": 150, "y": 50, "w": 40, "h": 40 },
          "fill": { "color": "#0000FF" },
          "stroke": { "color": "#FFFFFF", "weights": { "l": 4, "t": 4, "r": 4, "b": 4 }, "align": "center" } },
        { "id": "s:row", "name": "Row", "kind": "frame", "box": { "x": 40, "y": 150, "w": 200, "h": 60 },
          "layout": { "mode": "horizontal", "spacing": 20 },
          "children": [
            { "id": "s:a", "name": "A", "kind": "shape", "box": { "x": 0, "y": 0, "w": 40, "h": 40 },
              "fill": { "color": "#FF0000" },
              "stroke": { "color": "#00FF00", "weights": { "l": 4, "t": 4, "r": 4, "b": 4 }, "align": "outside" } },
            { "id": "s:b", "name": "B", "kind": "shape", "box": { "x": 60, "y": 0, "w": 40, "h": 40 },
              "fill": { "color": "#FF0000" } }
          ] },
        { "id": "s:clip", "name": "Clipper", "kind": "frame", "box": { "x": 260, "y": 50, "w": 100, "h": 100 },
          "clip": true, "fill": { "color": "#000000" },
          "stroke": { "color": "#FFFFFF", "weights": { "l": 4, "t": 4, "r": 4, "b": 4 }, "align": "outside" },
          "children": [
            { "id": "s:inner", "name": "Inner", "kind": "shape", "box": { "x": 50, "y": 50, "w": 100, "h": 100 },
              "fill": { "color": "#FF0000" } }
          ] }
      ]
    }
  })json");
  UIWTDesignImport::FResult Result;
  if (!ImportDesign(Json, Result))
  {
    CleanUp();
    return false;
  }
  FImage Image;
  if (Render(Result.Blueprint, FIntPoint(400, 300), Image))
  {
    ExpectPixel(TEXT("outside stroke"), Image, 47, 70, FColor::White);
    ExpectPixel(TEXT("outside stroke fill"), Image, 70, 70, FColor::Blue);
    ExpectPixel(TEXT("outside stroke ends"), Image, 43, 70, FColor::Black);
    ExpectPixel(TEXT("center stroke, outer half"), Image, 149, 70, FColor::White);
    ExpectPixel(TEXT("center stroke, inner half"), Image, 151, 70, FColor::White);
    ExpectPixel(TEXT("center stroke fill"), Image, 170, 70, FColor::Blue);
    ExpectPixel(TEXT("center stroke ends"), Image, 146, 70, FColor::Black);
    ExpectPixel(TEXT("stroke in a row"), Image, 38, 170, FColor::Green);
    ExpectPixel(TEXT("stroked item's fill"), Image, 60, 170, FColor::Red);
    ExpectPixel(TEXT("the gap stays"), Image, 92, 170, FColor::Black);
    ExpectPixel(TEXT("sibling didn't move"), Image, 120, 170, FColor::Red);
    ExpectPixel(TEXT("sibling starts at its place"), Image, 97, 170, FColor::Black);
    ExpectPixel(TEXT("clipped content inside"), Image, 340, 120, FColor::Red);
    ExpectPixel(TEXT("clipping frame's stroke"), Image, 362, 120, FColor::White);
    ExpectPixel(TEXT("content cut at the frame's box"), Image, 370, 120, FColor::Black);
  }
  CleanUp();
  return true;
}

// A fixed-width text in a canvas grows in height instead of overflowing
// sideways.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignRenderTextWrapTest, FUIWTImportTestBase,
                                        "UIWidgetTool.DesignImport.Render.TextWrap", TestFlags)

bool FUIWTDesignRenderTextWrapTest::RunTest(const FString &Parameters)
{
  if (!CanRender())
  {
    return true;
  }
  const FString Json = TEXT(R"json({
    "version": 1, "source": "figma", "referenceSize": { "w": 300, "h": 200 },
    "root": {
      "id": "t:root", "name": "Wrap", "kind": "frame",
      "box": { "x": 0, "y": 0, "w": 300, "h": 200 }, "fill": { "color": "#000000" },
      "children": [
        { "id": "t:text", "name": "Body", "kind": "text", "box": { "x": 20, "y": 20, "w": 100, "h": 20 },
          "text": { "content": "The quick brown fox jumps over the lazy dog again and again",
                    "runs": [ { "font": { "family": "Unmapped" }, "size": 16, "color": "#FFFFFF" } ],
                    "sizing": "fixedWidth" } }
      ]
    }
  })json");
  UIWTDesignImport::FResult Result;
  if (!ImportDesign(Json, Result))
  {
    CleanUp();
    return false;
  }
  FImage Image;
  if (Render(Result.Blueprint, FIntPoint(300, 200), Image))
  {
    auto Bright = [&](int32 InX0, int32 InY0, int32 InX1, int32 InY1)
    {
      int32 Count = 0;
      for (int32 Y = InY0; Y < InY1; ++Y)
      {
        for (int32 X = InX0; X < InX1; ++X)
        {
          const FColor Color = Pixel(Image, X, Y);
          Count += Color.R > 128 && Color.G > 128 && Color.B > 128 ? 1 : 0;
        }
      }
      return Count;
    };
    TestTrue(TEXT("text below the first line"), Bright(20, 45, 120, 120) > 20);
    TestEqual(TEXT("nothing right of the box"), Bright(126, 10, 300, 200), 0);
  }
  CleanUp();
  return true;
}

#endif
