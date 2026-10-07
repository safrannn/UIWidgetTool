#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Core/UIWTDesignImport.h"
#include "Core/UIWTDesignSettings.h"
#include "Core/UIWTGeneratedBlueprints.h"
#include "Core/UIWTRunImages.h"
#include "Design/UIWTDesignToolset.h"
#include "Design/UIWTPsdManifest.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ToolsetRegistry/ToolCallAsyncResultString.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "WidgetBlueprint.h"

// Tests for the Photoshop manifest reader (import-tree.md → Implementation
// order, Phase 6). Each test writes an export folder (manifest.json and
// layer PNGs, as the UXP exporter would) to the automation transient folder;
// the import test imports into /Temp without saving.
namespace
{
  using namespace UIWTDesignTree;

  constexpr EAutomationTestFlags TestFlags =
      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

  // An artboard at (100, 50) in a 2x document at 144 ppi: every size in it
  // is halved, and 1 pt is 1 design pixel.
  const TCHAR *ManifestJson = TEXT(R"json({
    "version": 1,
    "exporter": "UIWidgetTool PhotoshopBridge test",
    "source": { "file": "Shop.psd", "documentId": 12, "exportedAt": "{EXPORTED}", "artboardId": 7 },
    "document": { "w": 1000, "h": 700, "resolution": 144, "scale": 2 },
    "root": {
      "id": 7, "name": "Shop", "kind": "artboard", "bounds": { "x": 100, "y": 50, "w": 800, "h": 600 },
      "children": [
        { "id": 2, "name": "Background", "kind": "pixel", "bounds": { "x": 100, "y": 50, "w": 800, "h": 600 },
          "opacity": 1, "blendMode": "normal", "visible": true, "image": "layers/2.png" },
        { "id": 3, "name": "btn_Buy", "kind": "group", "bounds": { "x": 140, "y": 130, "w": 240, "h": 80 },
          "opacity": 0.5, "blendMode": "passThrough", "effects": [ "dropShadow" ],
          "children": [
            { "id": 4, "name": "Plate", "kind": "shape", "bounds": { "x": 140, "y": 130, "w": 240, "h": 80 },
              "fillOpacity": 0.5, "image": "layers/4.png",
              "shape": { "fill": "#3366FF", "stroke": { "color": "#000000", "width": 4, "align": "outside" },
                         "radii": [ 24, 24, 24, 24 ] } },
            { "id": 5, "name": "Label", "kind": "text", "bounds": { "x": 160, "y": 140, "w": 120, "h": 40 },
              "text": { "content": "{LABEL}", "align": "center",
                "runs": [ { "from": 0, "to": 7, "font": { "postscript": "Inter-Bold", "family": "Inter", "style": "Bold" },
                            "size": 16, "color": "#FFFFFFCC", "tracking": 20, "leading": 24 } ] } }
          ] },
        { "id": 6, "name": "Body", "kind": "text", "bounds": { "x": 140, "y": 250, "w": 400, "h": 100 },
          "text": { "content": "Body", "box": { "w": 400, "h": 100 },
                    "runs": [ { "from": 0, "to": 4, "size": 12, "color": "#000000" } ] } },
        { "id": 8, "name": "Glow", "kind": "pixel", "bounds": { "x": 500, "y": 100, "w": 100, "h": 100 },
          "blendMode": "screen", "effects": [ "outerGlow" ], "image": "layers/8.png" },
        { "id": 9, "name": "Tint", "kind": "adjustment", "bounds": { "x": 100, "y": 50, "w": 800, "h": 600 } },
        { "id": 10, "name": "Hidden", "kind": "pixel", "visible": false,
          "bounds": { "x": 100, "y": 50, "w": 20, "h": 20 }, "image": "layers/10.png" },
        { "id": 11, "name": "Masked", "kind": "clipStack", "bounds": { "x": 700, "y": 450, "w": 40, "h": 40 },
          "image": "layers/11.png" },
        { "id": 12, "name": "Escape", "kind": "pixel", "bounds": { "x": 100, "y": 50, "w": 10, "h": 10 },
          "image": "../outside.png" },
        { "id": 13, "name": "Missing", "kind": "pixel", "bounds": { "x": 100, "y": 50, "w": 10, "h": 10 },
          "image": "layers/13.png" },
        { "id": 14, "name": "Empty", "kind": "group", "bounds": { "x": 0, "y": 0, "w": 0, "h": 0 }, "children": [] }
      ]
    }
  })json");

  class FUIWTPsdTestBase : public FAutomationTestBase
  {
  public:
    FUIWTPsdTestBase(const FString &InName, const bool bInComplexTask)
        : FAutomationTestBase(InName, bInComplexTask)
    {
    }
    virtual bool SuppressLogErrors() override { return true; }
    virtual bool SuppressLogWarnings() override { return true; }

  protected:
    FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);

    FString ExportDir() const
    {
      return FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() /
                                               TEXT("UIWTPsd") / Id / TEXT("Shop.uiwt-psd"));
    }

    FString ManifestFile() const { return ExportDir() / TEXT("manifest.json"); }

    FString TargetFolder() const { return TEXT("/Temp/UIWTPsdTest_") + Id; }

    UIWTPsdManifest::FOptions Options(bool bInImportHidden = true) const
    {
      UIWTPsdManifest::FOptions Result;
      Result.ReferenceSize = FIntPoint(400, 300);
      Result.bImportHidden = bInImportHidden;
      return Result;
    }

    // The manifest with its label text and export time filled in.
    FString Manifest(const FString &InLabel, const FString &InExportedAt) const
    {
      return FString(ManifestJson)
          .Replace(TEXT("{LABEL}"), *InLabel)
          .Replace(TEXT("{EXPORTED}"), *InExportedAt);
    }

    // Writes the export folder: the manifest, the layer PNGs and the
    // composite. Layer 13 is left out on purpose.
    void WriteExport(const FString &InLabel, const FString &InExportedAt)
    {
      if (!FFileHelper::SaveStringToFile(Manifest(InLabel, InExportedAt), *ManifestFile()))
      {
        AddError(TEXT("can't write ") + ManifestFile());
      }
      WritePng(TEXT("layers/2.png"), 800, 600, FColor(16, 24, 32));
      WritePng(TEXT("layers/4.png"), 248, 88, FColor(51, 102, 255));
      WritePng(TEXT("layers/8.png"), 100, 100, FColor::White);
      WritePng(TEXT("layers/10.png"), 20, 20, FColor::Red);
      WritePng(TEXT("layers/11.png"), 40, 40, FColor::Green);
      WritePng(TEXT("composite.png"), 800, 600, FColor(16, 24, 32));
    }

    void WritePng(const FString &InRelative, int32 InW, int32 InH, FColor InColor)
    {
      FImage Image(InW, InH, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
      for (FColor &Pixel : Image.AsBGRA8())
      {
        Pixel = InColor;
      }
      FString Error;
      if (!UIWTRunImages::SavePng(Image, ExportDir() / InRelative, Error))
      {
        AddError(Error);
      }
    }

    void CleanUp()
    {
      IFileManager::Get().DeleteDirectory(*FPaths::GetPath(ExportDir()), false, true);
      IFileManager::Get().DeleteDirectory(*UIWTPsdManifest::GetCacheDirectory(ManifestFile()),
                                          false, true);
      const FString OnDisk = UIWTGenerated::GetFolderOnDisk(TargetFolder());
      if (!OnDisk.IsEmpty())
      {
        IFileManager::Get().DeleteDirectory(*OnDisk, false, true);
      }
    }

    static const FNode *Child(const FNode &InParent, const FString &InName)
    {
      return InParent.Children.FindByPredicate([&InName](const FNode &InNode)
                                               { return InNode.Name == InName; });
    }

    static bool HasNote(const FDocument &InDoc, const FString &InNode, const FString &InCategory)
    {
      return InDoc.Notes.ContainsByPredicate(
          [&](const FNote &InNote)
          { return InNote.Node == InNode && InNote.Category == InCategory; });
    }

    void TestBox(const FString &InWhat, const FNode *InNode, double InX, double InY, double InW,
                 double InH)
    {
      if (!InNode)
      {
        AddError(InWhat + TEXT(": no node"));
        return;
      }
      const FRect &Box = InNode->Box;
      if (Box.X != InX || Box.Y != InY || Box.W != InW || Box.H != InH)
      {
        AddError(FString::Printf(TEXT("%s: box (%g, %g, %g, %g), expected (%g, %g, %g, %g)"),
                                 *InWhat, Box.X, Box.Y, Box.W, Box.H, InX, InY, InW, InH));
      }
    }
  };
}

// The manifest → design tree mapping: relative 1x boxes, points to pixels,
// text, shapes, effects, blend modes, hidden layers and what is left out.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTPsdManifestTest, FUIWTPsdTestBase,
                                        "UIWidgetTool.Psd.Manifest", TestFlags)

bool FUIWTPsdManifestTest::RunTest(const FString &Parameters)
{
  WriteExport(TEXT("Buy\\rnow"), TEXT("2026-09-29T10:00:00.000Z"));
  const FString Json = Manifest(TEXT("Buy\\rnow"), TEXT("2026-09-29T10:00:00.000Z"));
  UIWTPsdManifest::FResult Read;
  FString Error;
  if (!UIWTPsdManifest::Read(Json, ExportDir(), Options(), Read, Error))
  {
    AddError(TEXT("read failed: ") + Error);
    CleanUp();
    return false;
  }
  const FDocument &Doc = Read.Document;
  TestEqual(TEXT("source"), Doc.Source, FString(TEXT("psd")));
  TestTrue(TEXT("reference size"), Doc.ReferenceSize == FVector2D(400, 300));
  TestEqual(TEXT("export time"), Read.ExportedAt, FString(TEXT("2026-09-29T10:00:00.000Z")));
  if (TestTrue(TEXT("sourceRef"), Doc.SourceRef.IsValid()))
  {
    TestEqual(TEXT("sourceRef nodeId: the artboard"), Doc.SourceRef->GetStringField(TEXT("nodeId")),
              FString(TEXT("7")));
    TestEqual(TEXT("sourceRef document"), Doc.SourceRef->GetStringField(TEXT("document")),
              FString(TEXT("Shop.psd")));
  }

  // The artboard: a clipping frame, halved.
  const FNode &Root = Doc.Root;
  TestEqual(TEXT("root id"), Root.Id, FString(TEXT("7")));
  TestEqual(TEXT("root name"), Root.Name, FString(TEXT("Shop")));
  TestTrue(TEXT("root clips"), Root.bClip);
  TestBox(TEXT("root"), &Root, 0, 0, 400, 300);
  TArray<FString> Names;
  for (const FNode &Node : Root.Children)
  {
    Names.Add(Node.Name);
  }
  TestEqual(TEXT("children, bottom first; no adjustment, escaping path or empty group"),
            FString::Join(Names, TEXT(",")),
            FString(TEXT("Background,btn_Buy,Body,Glow,Hidden,Masked,Missing")));
  TestEqual(TEXT("node count"), Read.NodeCount, 10);

  // A pixel layer: its PNG at 2x.
  const FNode *Background = Child(Root, TEXT("Background"));
  TestBox(TEXT("Background"), Background, 0, 0, 400, 300);
  if (Background && TestTrue(TEXT("Background image"), Background->Image.IsValid()))
  {
    TestEqual(TEXT("Background id"), Background->Id, FString(TEXT("7/2")));
    TestTrue(TEXT("Background kind"), Background->Kind == EKind::Image);
    TestEqual(TEXT("Background path"), Background->Image->Path, FString(TEXT("layers/2.png")));
    TestEqual(TEXT("Background scale"), Background->Image->Scale, 2.0);
    TestTrue(TEXT("Background rendered"), Background->Image->Origin == EImageOrigin::Rendered);
    TestTrue(TEXT("Background size"), Background->Image->Size.IsSet() &&
                                          *Background->Image->Size == FVector2D(800, 600));
  }

  // A group with effects: kept, its effect listed as not baked.
  const FNode *Group = Child(Root, TEXT("btn_Buy"));
  TestBox(TEXT("btn_Buy"), Group, 20, 40, 120, 40);
  if (Group)
  {
    TestTrue(TEXT("group kind"), Group->Kind == EKind::Group);
    TestEqual(TEXT("group opacity"), Group->Opacity, 0.5);
    TestEqual(TEXT("pass through is normal"), Group->BlendMode, FString(TEXT("normal")));
    TestTrue(TEXT("group effect not baked"),
             Group->Effects.Num() == 1 && Group->Effects[0].Type == TEXT("dropShadow") &&
                 !Group->Effects[0].bBaked);
    TestTrue(TEXT("prefix hint"), Group->Hints.Contains(TEXT("prefix:btn")));

    // A rounded rectangle: a shape, fill opacity in the alpha.
    const FNode *Plate = Child(*Group, TEXT("Plate"));
    TestBox(TEXT("Plate"), Plate, 0, 0, 120, 40);
    if (Plate)
    {
      TestTrue(TEXT("Plate kind"), Plate->Kind == EKind::Shape);
      TestFalse(TEXT("Plate has no image"), Plate->Image.IsValid());
      TestTrue(TEXT("Plate fill"), Plate->Fill.IsValid() && Plate->Fill->Color.IsSet() &&
                                       *Plate->Fill->Color == FColor(0x33, 0x66, 0xFF, 128));
      TestTrue(TEXT("Plate stroke"), Plate->Stroke.IsValid() && Plate->Stroke->Weights.L == 2.0 &&
                                         Plate->Stroke->Align == EStrokeAlign::Outside);
      TestTrue(TEXT("Plate radii"),
               Plate->Radii.IsSet() && *Plate->Radii == FVector4(12.0, 12.0, 12.0, 12.0));
    }

    // Point text: pt × 144 / 72 / 2 = px.
    const FNode *Label = Child(*Group, TEXT("Label"));
    TestBox(TEXT("Label"), Label, 10, 5, 60, 20);
    if (Label && TestTrue(TEXT("Label text"), Label->Text.IsValid() && Label->Text->Runs.Num() == 1))
    {
      const FTextBlock &Text = *Label->Text;
      const FTextRun &Run = Text.Runs[0];
      TestEqual(TEXT("carriage returns become line feeds"), Text.Content, FString(TEXT("Buy\nnow")));
      TestTrue(TEXT("centered"), Text.Align == ETextAlign::Center);
      TestTrue(TEXT("point text is auto"), Text.Sizing == ETextSizing::Auto);
      TestEqual(TEXT("size"), Run.Size, 16.0);
      TestTrue(TEXT("leading"), Run.LineHeightPx.IsSet() && *Run.LineHeightPx == 24.0);
      TestEqual(TEXT("tracking"), Run.Tracking, 20.0);
      TestTrue(TEXT("color"), Run.Color == FColor(255, 255, 255, 204));
      TestEqual(TEXT("font"), Run.Font.PostScript, FString(TEXT("Inter-Bold")));
      TestEqual(TEXT("run length"), Run.Len, 7);
    }
  }

  // Box text wraps at its box.
  const FNode *Body = Child(Root, TEXT("Body"));
  if (Body && TestTrue(TEXT("Body text"), Body->Text.IsValid() && Body->Text->Runs.Num() == 1))
  {
    TestTrue(TEXT("box text is fixed width"), Body->Text->Sizing == ETextSizing::FixedWidth);
    TestEqual(TEXT("Body size"), Body->Text->Runs[0].Size, 12.0);
    TestFalse(TEXT("auto leading"), Body->Text->Runs[0].LineHeightPx.IsSet());
  }

  // A pixel layer with an effect and a blend mode: the effect is baked.
  const FNode *Glow = Child(Root, TEXT("Glow"));
  TestBox(TEXT("Glow"), Glow, 200, 25, 50, 50);
  if (Glow)
  {
    TestEqual(TEXT("blend mode kept"), Glow->BlendMode, FString(TEXT("screen")));
    TestTrue(TEXT("glow baked"), Glow->Effects.Num() == 1 && Glow->Effects[0].Type == TEXT("glow") &&
                                     Glow->Effects[0].bBaked);
  }

  const FNode *Hidden = Child(Root, TEXT("Hidden"));
  TestTrue(TEXT("hidden layer imported hidden"), Hidden && !Hidden->bVisible);
  const FNode *Masked = Child(Root, TEXT("Masked"));
  TestTrue(TEXT("clip stack is an image"), Masked && Masked->Kind == EKind::Image);
  const FNode *Missing = Child(Root, TEXT("Missing"));
  TestTrue(TEXT("missing PNG: document scale"),
           Missing && Missing->Image.IsValid() && Missing->Image->Scale == 2.0 &&
               !Missing->Image->Size.IsSet());
  TestTrue(TEXT("imageMissing"), HasNote(Doc, TEXT("7/13"), TEXT("imageMissing")));
  TestTrue(TEXT("adjustmentDropped"), HasNote(Doc, TEXT("7/9"), TEXT("adjustmentDropped")));
  TestTrue(TEXT("escaping image path skipped"), HasNote(Doc, TEXT("7/12"), TEXT("layerSkipped")));

  // The converter: a screen at the reference size, notes in the report.
  FConvertOptions ConvertOptions;
  ConvertOptions.BlueprintName = TEXT("WBP_Shop");
  ConvertOptions.TargetFolder = TEXT("/Game/UIWTPsdTest");
  ConvertOptions.ComponentsFolder = TEXT("/Game/UIWTPsdTest/Components");
  const FConvertResult Converted = Convert(Doc, ConvertOptions);
  TestTrue(TEXT("spec"), Converted.Spec.IsValid());
  TestTrue(TEXT("screen root"), Converted.RootMode == ERootMode::Screen);
  TestTrue(TEXT("notes reported"),
           Converted.Report.ContainsByPredicate([](const FReportEntry &Entry)
                                                { return Entry.Category == TEXT("adjustmentDropped"); }));

  // The round trip through design.json keeps what the reader wrote.
  FDocument Again;
  TArray<FString> Errors;
  TestTrue(TEXT("design.json round trip"),
           ReadDocument(WriteDocumentString(Doc), Again, Errors) &&
               HashNode(Again.Root) == HashNode(Doc.Root));

  // Hidden layers left out.
  UIWTPsdManifest::FResult WithoutHidden;
  if (UIWTPsdManifest::Read(Json, ExportDir(), Options(false), WithoutHidden, Error))
  {
    TestNull(TEXT("hidden layer left out"), Child(WithoutHidden.Document.Root, TEXT("Hidden")));
  }

  // Refusals.
  UIWTPsdManifest::FResult Refused;
  TestFalse(TEXT("newer version refused"),
            UIWTPsdManifest::Read(Json.Replace(TEXT("\"version\": 1"), TEXT("\"version\": 2")),
                                  ExportDir(), Options(), Refused, Error));
  TestFalse(TEXT("not JSON refused"),
            UIWTPsdManifest::Read(TEXT("{"), ExportDir(), Options(), Refused, Error));

  CleanUp();
  return true;
}

// Prepare copies the export into the project; the import builds the
// blueprint with the composite as its reference, and a later export
// re-imports through the merge.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTPsdImportTest, FUIWTPsdTestBase,
                                        "UIWidgetTool.Psd.Import", TestFlags)

bool FUIWTPsdImportTest::RunTest(const FString &Parameters)
{
  WriteExport(TEXT("Buy"), TEXT("2026-09-29T10:00:00.000Z"));
  FString DesignFile;
  UIWTPsdManifest::FResult Read;
  FString Error;
  if (!UIWTPsdManifest::Prepare(ManifestFile(), Options(), DesignFile, Read, Error))
  {
    AddError(TEXT("prepare failed: ") + Error);
    CleanUp();
    return false;
  }
  const FString Cache = UIWTPsdManifest::GetCacheDirectory(ManifestFile());
  TestEqual(TEXT("design.json in the cache"), DesignFile, Cache / TEXT("design.json"));
  TestTrue(TEXT("cache inside Saved"),
           Cache.StartsWith(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())));
  IFileManager &Files = IFileManager::Get();
  TestTrue(TEXT("layers copied"), Files.FileExists(*(Cache / TEXT("layers/2.png"))));
  TestTrue(TEXT("composite copied"), Files.FileExists(*(Cache / TEXT("reference.png"))));
  TestTrue(TEXT("manifest copied"), Files.FileExists(*(Cache / TEXT("manifest.json"))));
  FString ManifestRef;
  TestTrue(TEXT("sourceRef manifest"),
           Read.Document.SourceRef->TryGetStringField(TEXT("manifest"), ManifestRef) &&
               FPaths::IsSamePath(ManifestRef, ManifestFile()));

  UIWTDesignImport::FRequest Request;
  Request.DesignFile = DesignFile;
  Request.ReferenceImage = Cache / TEXT("reference.png");
  Request.TargetFolder = TargetFolder();
  Request.BlueprintName = TEXT("WBP_Shop");
  Request.ComponentsFolder = TargetFolder() / TEXT("Components");
  Request.bSave = false;
  UIWTDesignImport::FResult Imported;
  if (!UIWTDesignImport::Import(Request, Imported, Error))
  {
    AddError(TEXT("import failed: ") + Error);
    CleanUp();
    return false;
  }
  for (const FString &ApplyError : Imported.ApplyErrors)
  {
    AddError(TEXT("Apply: ") + ApplyError);
  }
  UWidgetBlueprint *Blueprint = Imported.Blueprint;
  TestNotNull(TEXT("blueprint"), Blueprint);
  TestTrue(TEXT("screen root"), Imported.RootMode == ERootMode::Screen);
  TestTrue(TEXT("design size"), Imported.DesignSize == FIntPoint(400, 300));
  // Background, Glow, Hidden and Masked; the Plate is a Border, the missing
  // PNG is reported.
  TestEqual(TEXT("textures"), Imported.TexturesWritten, 4);
  TestTrue(TEXT("reference copied"), !Imported.ReferenceFile.IsEmpty() &&
                                         Files.FileExists(*Imported.ReferenceFile));
  UTextBlock *Label =
      Blueprint ? Cast<UTextBlock>(Blueprint->WidgetTree->FindWidget(TEXT("Label"))) : nullptr;
  if (TestNotNull(TEXT("Label"), Label))
  {
    TestEqual(TEXT("Label text"), Label->GetText().ToString(), FString(TEXT("Buy")));
  }
  UIWTDesignImport::FSidecar Sidecar;
  if (TestTrue(TEXT("sidecar"),
               Blueprint && UIWTDesignImport::ReadSidecar(Blueprint->GetOutermost()->GetName(),
                                                          Sidecar, Error)))
  {
    TestEqual(TEXT("sidecar source"), Sidecar.Source, FString(TEXT("psd")));
    FString ExportedAt;
    TestTrue(TEXT("sidecar keeps the export time"),
             Sidecar.SourceRef.IsValid() &&
                 Sidecar.SourceRef->TryGetStringField(TEXT("exportedAt"), ExportedAt) &&
                 ExportedAt == TEXT("2026-09-29T10:00:00.000Z"));
  }

  // A later export with a new label re-imports through the merge.
  if (Blueprint && Label)
  {
    WriteExport(TEXT("Buy it"), TEXT("2026-09-29T11:00:00.000Z"));
    if (UIWTPsdManifest::Prepare(ManifestFile(), Options(), DesignFile, Read, Error))
    {
      UIWTDesignImport::FReimportRequest Reimport;
      Reimport.Blueprint = Blueprint;
      Reimport.DesignFile = DesignFile;
      Reimport.ReferenceImage = Cache / TEXT("reference.png");
      UIWTDesignImport::FReimportPlan Plan;
      UIWTDesignImport::FReimportResult Applied;
      if (!UIWTDesignImport::PlanReimport(Reimport, Plan, Error))
      {
        AddError(TEXT("plan failed: ") + Error);
      }
      else if (!UIWTDesignImport::ApplyReimport(Plan, Applied, Error))
      {
        AddError(TEXT("apply failed: ") + Error);
      }
      else
      {
        TestTrue(TEXT("one node changed: the label"),
                 Plan.Changes.Num() == 1 && Plan.Changes[0].Node == TEXT("7/5"));
        Label = Cast<UTextBlock>(Blueprint->WidgetTree->FindWidget(TEXT("Label")));
        TestTrue(TEXT("new label"), Label && Label->GetText().ToString() == TEXT("Buy it"));
        UIWTDesignImport::AcceptReimport(Blueprint, false, Error);
      }
    }
    else
    {
      AddError(TEXT("second prepare failed: ") + Error);
    }
  }

  CleanUp();
  return true;
}

// The ImportPsdManifest tool runs the fonts step before converting. With
// this computer's fonts and Google Fonts off (so the test needs neither),
// the label's Roboto Bold comes from the font library, here the engine's
// fonts folder, and the tool's result lists it. The tool completes later.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTPsdImportFontsTest, FUIWTPsdTestBase,
                                        "UIWidgetTool.Psd.ImportFonts", TestFlags)

bool FUIWTPsdImportFontsTest::RunTest(const FString &Parameters)
{
  const FString ExportedAt = TEXT("2026-10-05T10:00:00.000Z");
  WriteExport(TEXT("Buy"), ExportedAt);
  const FString Roboto =
      Manifest(TEXT("Buy"), ExportedAt)
          .Replace(TEXT(R"("postscript": "Inter-Bold", "family": "Inter")"),
                   TEXT(R"("postscript": "Roboto-Bold", "family": "Roboto")"));
  if (!FFileHelper::SaveStringToFile(Roboto, *ManifestFile()))
  {
    AddError(TEXT("can't write ") + ManifestFile());
  }

  UUIWTDesignSettings *Settings = GetMutableDefault<UUIWTDesignSettings>();
  auto Restore = [Settings, Library = Settings->FontLibraryFolder,
                  bInstalled = Settings->bUseInstalledFonts,
                  bGoogle = Settings->bDownloadGoogleFonts]()
  {
    Settings->FontLibraryFolder = Library;
    Settings->bUseInstalledFonts = bInstalled;
    Settings->bDownloadGoogleFonts = bGoogle;
  };
  Settings->FontLibraryFolder.Path = TEXT("/Engine/EngineFonts");
  Settings->bUseInstalledFonts = false;
  Settings->bDownloadGoogleFonts = false;

  const TStrongObjectPtr<UToolCallAsyncResultString> Result(
      UUIWTDesignToolset::ImportPsdManifest(ManifestFile(), TargetFolder(), TEXT("WBP_PsdFonts")));
  ADD_LATENT_AUTOMATION_COMMAND(FUntilCommand(
      [this, Result, Restore]() -> bool
      {
        if (!Result->bIsComplete)
        {
          return false;
        }
        Restore();
        if (TestTrue(TEXT("tool succeeded: ") + Result->Error, Result->Error.IsEmpty()))
        {
          TSharedPtr<FJsonObject> Value;
          FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Result->Value), Value);
          const TSharedPtr<FJsonObject> *Fonts = nullptr;
          const TArray<TSharedPtr<FJsonValue>> *InLibrary = nullptr;
          TestTrue(TEXT("result lists the label's font in the library"),
                   Value.IsValid() && Value->TryGetObjectField(TEXT("fonts"), Fonts) &&
                       (*Fonts)->TryGetArrayField(TEXT("inLibrary"), InLibrary) &&
                       InLibrary->ContainsByPredicate(
                           [](const TSharedPtr<FJsonValue> &InFont)
                           { return InFont->AsString() == TEXT("Roboto Bold"); }));
          const FString Path = TargetFolder() / TEXT("WBP_PsdFonts/WBP_PsdFonts.WBP_PsdFonts");
          const UWidgetBlueprint *Blueprint = FindObject<UWidgetBlueprint>(nullptr, *Path);
          const UTextBlock *Label =
              Blueprint ? Cast<UTextBlock>(Blueprint->WidgetTree->FindWidget(TEXT("Label")))
                        : nullptr;
          if (TestNotNull(TEXT("label"), Label))
          {
            const FSlateFontInfo Font = Label->GetFont();
            TestEqual(TEXT("label font"),
                      Font.FontObject ? Font.FontObject->GetPathName() : FString(),
                      FString(TEXT("/Engine/EngineFonts/Roboto.Roboto")));
            TestEqual(TEXT("label typeface"), Font.TypefaceFontName.ToString(),
                      FString(TEXT("Bold")));
          }
        }
        CleanUp();
        return true;
      },
      [this, Restore]() -> bool
      {
        Restore();
        AddError(TEXT("ImportPsdManifest didn't complete"));
        CleanUp();
        return true;
      },
      30.f));
  return true;
}

#endif
