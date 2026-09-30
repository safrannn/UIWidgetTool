#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Blueprint/WidgetTree.h"
#include "Claude/UIWTClaudeService.h"
#include "Claude/UIWTToolset.h"
#include "Components/PanelWidget.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Core/UIWTDesignImport.h"
#include "Core/UIWTGeneratedBlueprints.h"
#include "Core/UIWTRunImages.h"
#include "Design/UIWTImageReader.h"
#include "Design/UIWTPsdManifest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

// Tests for the image reader (import-image.md, section 6): the tools of an
// image-reading run, called directly in a run without a Claude process
// (FUIWTClaudeService::DevBeginImageRead), importing into /Temp unsaved.
namespace
{
  using namespace UIWTDesignTree;

  constexpr EAutomationTestFlags TestFlags =
      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

  // A 400 x 300 mockup at 2x: a blue plate and a red icon on a dark screen.
  const TCHAR *TreeJson = TEXT(R"json({ "root": {
    "id": "root", "name": "Mock", "kind": "frame", "box": { "x": 0, "y": 0, "w": 400, "h": 300 },
    "fill": { "color": "#101820" },
    "children": [
      { "id": "plate", "name": "Btn_Play", "kind": "frame", "box": { "x": 40, "y": 40, "w": 120, "h": 60 },
        "fill": { "color": "#3366FF" }, "radii": [ 16, 16, 16, 16 ], "hints": [ "role:button" ],
        "children": [
          { "id": "label", "name": "Label", "kind": "text", "box": { "x": 40, "y": 40, "w": 120, "h": 60 },
            "text": { "content": "{LABEL}", "runs": [ { "font": { "family": "Inter", "style": "Bold" }, "size": 32, "color": "#FFFFFF" } ],
                      "align": "center", "valign": "center", "sizing": "fixed" } } ] },
      { "id": "icon", "name": "Icon", "kind": "image", "box": { "x": 200, "y": 40, "w": 48, "h": 48 },
        "image": { "path": "images/icon.png" } } ] } })json");

  class FUIWTImageReaderTestBase : public FAutomationTestBase
  {
  public:
    FUIWTImageReaderTestBase(const FString &InName, const bool bInComplexTask)
        : FAutomationTestBase(InName, bInComplexTask)
    {
    }
    virtual bool SuppressLogErrors() override { return true; }
    virtual bool SuppressLogWarnings() override { return true; }

  protected:
    FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);

    FString WorkDir() const
    {
      return FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() /
                                               TEXT("UIWTImageReader") / Id);
    }

    FString TargetFolder() const { return TEXT("/Temp/UIWTImageTest_") + Id; }

    // The mockup's pixels at 2x.
    FString WriteMockup()
    {
      FImage Image(400, 300, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
      TArrayView64<FColor> Pixels = Image.AsBGRA8();
      for (int32 Y = 0; Y < 300; ++Y)
      {
        for (int32 X = 0; X < 400; ++X)
        {
          const bool bPlate = X >= 40 && X < 160 && Y >= 40 && Y < 100;
          const bool bIcon = FVector2D(X - 224, Y - 64).Size() < 24.0;
          Pixels[int64(Y) * 400 + X] = bPlate  ? FColor(0x33, 0x66, 0xFF)
                                       : bIcon ? FColor(0xEB, 0x57, 0x57)
                                               : FColor(0x10, 0x18, 0x20);
        }
      }
      const FString File = WorkDir() / TEXT("mock.png");
      FString Error;
      if (!UIWTRunImages::SavePng(Image, File, Error))
      {
        AddError(Error);
      }
      return File;
    }

    void CleanUp(const FGuid &InEntryId, const FString &InCacheDir)
    {
      if (FUIWTClaudeService *Service = FUIWTClaudeService::TryGet())
      {
        Service->ForgetChatState(InEntryId);
      }
      UUIWidgetPreviewObjectManagerSettings *Settings = UUIWidgetPreviewObjectManagerSettings::Get();
      Settings->RemoveWidgetPreviewObject(InEntryId);
      Settings->SaveWidgetPreviewObjects();
      IFileManager::Get().DeleteDirectory(*InCacheDir, false, true);
      IFileManager::Get().DeleteDirectory(*WorkDir(), false, true);
      const FString OnDisk = UIWTGenerated::GetFolderOnDisk(TargetFolder());
      if (!OnDisk.IsEmpty())
      {
        IFileManager::Get().DeleteDirectory(*OnDisk, false, true);
      }
    }
  };
}

// The source's cache, absolute boxes to relative design pixels, and
// rejected trees.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTImageReaderTreeTest, FUIWTImageReaderTestBase,
                                        "UIWidgetTool.ImageReader.Tree", TestFlags)

bool FUIWTImageReaderTreeTest::RunTest(const FString &Parameters)
{
  UIWTImageReader::FSource Source;
  FString Error;
  if (!TestTrue(TEXT("prepare"), UIWTImageReader::PrepareSource(
                                     WriteMockup(), UIWTImageReader::EScale::Two, Source, Error)))
  {
    AddError(Error);
    return false;
  }
  TestEqual(TEXT("scale"), Source.Scale, 2.0);
  TestTrue(TEXT("design size"), Source.DesignSize == FVector2D(200.0, 150.0));
  TestTrue(TEXT("reference cached"),
           FPaths::FileExists(Source.CacheDir / TEXT("reference.png")));
  TestEqual(TEXT("crc"), Source.Crc.Len(), 8);
  // The icon's crop, as CutImageNode would write it.
  FImage Icon(48, 48, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
  UIWTRunImages::SavePng(Icon, Source.CacheDir / TEXT("images/icon.png"), Error);

  FDocument Doc;
  TArray<FString> Errors;
  const FString Json = FString(TreeJson).Replace(TEXT("{LABEL}"), TEXT("Play"));
  if (TestTrue(TEXT("document"),
               UIWTImageReader::ToDocument(Json, Source.CacheDir, Source.Scale, Source.SourceFile,
                                           Source.Crc, FIntPoint(1920, 1080), Doc, Errors)))
  {
    TestEqual(TEXT("source"), Doc.Source, FString(TEXT("image")));
    TestTrue(TEXT("root halved"), Doc.Root.Box.W == 200.0 && Doc.Root.Box.H == 150.0);
    const FNode &Plate = Doc.Root.Children[0];
    TestTrue(TEXT("plate relative and halved"), Plate.Box.X == 20.0 && Plate.Box.Y == 20.0 &&
                                                    Plate.Box.W == 60.0 && Plate.Box.H == 30.0);
    TestTrue(TEXT("radii halved"), Plate.Radii.IsSet() && Plate.Radii->X == 8.0);
    TestTrue(TEXT("hint kept"), Plate.Hints.Contains(TEXT("role:button")));
    const FNode &Label = Plate.Children[0];
    TestTrue(TEXT("label relative to the plate"), Label.Box.X == 0.0 && Label.Box.Y == 0.0 &&
                                                      Label.Box.W == 60.0);
    TestEqual(TEXT("font size halved"), Label.Text->Runs[0].Size, 16.0);
    const FNode &Image = Doc.Root.Children[1];
    TestTrue(TEXT("image scale and size"),
             Image.Image.IsValid() && Image.Image->Scale == 2.0 && Image.Image->Size.IsSet() &&
                 Image.Image->Size->X == 48.0);
  }
  for (const FString &Problem : Errors)
  {
    AddError(Problem);
  }

  TestFalse(TEXT("a node without a box is refused"),
            UIWTImageReader::ToDocument(TEXT(R"json({"root": {"id": "r", "kind": "frame"}})json"),
                                        Source.CacheDir, 1.0, Source.SourceFile, Source.Crc,
                                        FIntPoint(1920, 1080), Doc, Errors));
  TestTrue(TEXT("says why"), !Errors.IsEmpty() && Errors[0].Contains(TEXT("box")));
  TestFalse(TEXT("an image not cut is refused"),
            UIWTImageReader::ToDocument(Json.Replace(TEXT("images/icon.png"), TEXT("../x.png")),
                                        Source.CacheDir, 2.0, Source.SourceFile, Source.Crc,
                                        FIntPoint(1920, 1080), Doc, Errors));
  IFileManager::Get().DeleteDirectory(*Source.CacheDir, false, true);
  IFileManager::Get().DeleteDirectory(*WorkDir(), false, true);
  return true;
}

// The run's tools: CutImageNode crops and reuses identical art;
// WriteDesignTree imports into the entry, then merges keeping ids and a
// widget added in UE; the tools refuse other runs.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTImageReaderToolsTest, FUIWTImageReaderTestBase,
                                        "UIWidgetTool.ImageReader.Tools", TestFlags)

bool FUIWTImageReaderToolsTest::RunTest(const FString &Parameters)
{
  FUIWTClaudeService *Service = FUIWTClaudeService::TryGet();
  UIWTImageReader::FSource Source;
  FString Error;
  if (!TestNotNull(TEXT("service"), Service) ||
      !TestTrue(TEXT("prepare"), UIWTImageReader::PrepareSource(
                                     WriteMockup(), UIWTImageReader::EScale::Two, Source, Error)))
  {
    return false;
  }
  UUIWidgetPreviewObjectManagerSettings *Settings = UUIWidgetPreviewObjectManagerSettings::Get();
  const FGuid EntryId = Settings->AddWidgetPreviewObject(TSoftClassPtr<UUserWidget>(), FString());

  FUIWTImageReadStart Start;
  Start.CacheDir = Source.CacheDir;
  Start.SourceFile = Source.SourceFile;
  Start.Crc = Source.Crc;
  Start.TargetFolder = TargetFolder();
  Start.BlueprintName = TEXT("WBP_ImageTest");
  Start.Scale = Source.Scale;
  Start.Nodes = 4;
  Start.Rounds = 3;
  Start.bSave = false;
  FText RunError;
  if (!TestTrue(TEXT("dev run"), Service->DevBeginImageRead(EntryId, Start, RunError)))
  {
    AddError(RunError.ToString());
    CleanUp(EntryId, Source.CacheDir);
    return false;
  }

  // Art, and the same art again under another id.
  const FString IconPath = UUIWTToolset::CutImageNode(TEXT("icon"), 200, 40, 48, 48, TEXT("Copy"),
                                                      TEXT("Circle"));
  TestEqual(TEXT("cut"), IconPath, FString(TEXT("images/icon.png")));
  FIntPoint Size;
  TestTrue(TEXT("crop written"),
           UIWTPsdManifest::ReadPngSize(Source.CacheDir / IconPath, Size) && Size == FIntPoint(48, 48));
  TestEqual(TEXT("identical art reused"),
            UUIWTToolset::CutImageNode(TEXT("icon2"), 200, 40, 48, 48, TEXT("Copy"), TEXT("Circle")),
            IconPath);
  TestFalse(TEXT("different art gets its own file"),
            UUIWTToolset::CutImageNode(TEXT("corner"), 0, 0, 16, 16, TEXT("Copy"), TEXT("Rect")) ==
                IconPath);

  // Before any tree there's nothing to render.
  TestTrue(TEXT("no blueprint yet"),
           UUIWTToolset::RenderWidgetBlueprint(nullptr, 0, 0).IsEmpty());

  // The first tree creates the blueprint and gives the entry its widget.
  TestTrue(TEXT("a broken tree is rejected"),
           UUIWTToolset::WriteDesignTree(TEXT("{\"root\": {\"id\": \"r\"}}")).IsEmpty());
  const FString First =
      UUIWTToolset::WriteDesignTree(FString(TreeJson).Replace(TEXT("{LABEL}"), TEXT("Play")));
  TestTrue(TEXT("imported"), First.StartsWith(TEXT("Imported")));
  const FUIWTActiveRun *Run = Service->GetActiveRun();
  UWidgetBlueprint *Blueprint =
      Run ? Cast<UWidgetBlueprint>(Run->BlueprintPath.TryLoad()) : nullptr;
  if (!TestNotNull(TEXT("the run's blueprint"), Blueprint))
  {
    Service->DevEndRun(false);
    CleanUp(EntryId, Source.CacheDir);
    return false;
  }
  const FWidgetPreviewObject *Entry = Settings->FindWidgetPreviewObject(EntryId);
  TestTrue(TEXT("the entry has the blueprint"),
           Entry && Entry->WidgetClass.ToSoftObjectPath() ==
                        FSoftObjectPath(Blueprint->GeneratedClass));
  UWidget *ButtonBefore = nullptr;
  Blueprint->WidgetTree->ForEachWidget(
      [&ButtonBefore](UWidget *InWidget)
      {
        if (InWidget->GetClass()->GetName() == TEXT("Button"))
        {
          ButtonBefore = InWidget;
        }
      });
  TestNotNull(TEXT("role:button made a Button"), ButtonBefore);

  // A widget added in UE to the root's canvas (the root widget itself is
  // the root frame's bg Border), then the tree again with a new label.
  UPanelWidget *Canvas = nullptr;
  Blueprint->WidgetTree->ForEachWidget(
      [&Canvas](UWidget *InWidget)
      {
        if (!Canvas && InWidget->GetClass()->GetName() == TEXT("CanvasPanel"))
        {
          Canvas = Cast<UPanelWidget>(InWidget);
        }
      });
  if (TestNotNull(TEXT("root canvas"), Canvas))
  {
    Canvas->AddChild(Blueprint->WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass(),
                                                                     TEXT("Extra")));
  }
  const FString Second =
      UUIWTToolset::WriteDesignTree(FString(TreeJson).Replace(TEXT("{LABEL}"), TEXT("Go")));
  AddInfo(Second);
  TestTrue(TEXT("updated"), Second.StartsWith(TEXT("Updated")));
  TestTrue(TEXT("one node changed"), Second.Contains(TEXT("0 nodes added, 0 removed, 1 changed")));
  const UTextBlock *Label = Cast<UTextBlock>(Blueprint->WidgetTree->FindWidget(TEXT("Label")));
  TestTrue(TEXT("new label"), Label && Label->GetText().ToString() == TEXT("Go"));
  TestNotNull(TEXT("the UE widget stays"), Blueprint->WidgetTree->FindWidget(TEXT("Extra")));
  TestTrue(TEXT("the Button is kept"),
           ButtonBefore && Blueprint->WidgetTree->FindWidget(ButtonBefore->GetFName()) == ButtonBefore);

  Service->DevEndRun(true);
  TestTrue(TEXT("refused outside an image run"),
           UUIWTToolset::CutImageNode(TEXT("late"), 0, 0, 8, 8, TEXT("Copy"), TEXT("Rect")).IsEmpty());
  CleanUp(EntryId, Source.CacheDir);
  return true;
}

// A run that ends before any tree leaves no blueprint and no cache.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTImageReaderFailTest, FUIWTImageReaderTestBase,
                                        "UIWidgetTool.ImageReader.Failure", TestFlags)

bool FUIWTImageReaderFailTest::RunTest(const FString &Parameters)
{
  FUIWTClaudeService *Service = FUIWTClaudeService::TryGet();
  UIWTImageReader::FSource Source;
  FString Error;
  if (!TestNotNull(TEXT("service"), Service) ||
      !TestTrue(TEXT("prepare"), UIWTImageReader::PrepareSource(
                                     WriteMockup(), UIWTImageReader::EScale::One, Source, Error)))
  {
    return false;
  }
  UUIWidgetPreviewObjectManagerSettings *Settings = UUIWidgetPreviewObjectManagerSettings::Get();
  const FGuid EntryId = Settings->AddWidgetPreviewObject(TSoftClassPtr<UUserWidget>(), FString());
  FUIWTImageReadStart Start;
  Start.CacheDir = Source.CacheDir;
  Start.SourceFile = Source.SourceFile;
  Start.Crc = Source.Crc;
  Start.TargetFolder = TargetFolder();
  Start.bSave = false;
  FText RunError;
  if (TestTrue(TEXT("dev run"), Service->DevBeginImageRead(EntryId, Start, RunError)))
  {
    Service->DevEndRun(false);
    TestFalse(TEXT("cache deleted"), FPaths::DirectoryExists(Source.CacheDir));
    const FWidgetPreviewObject *Entry = Settings->FindWidgetPreviewObject(EntryId);
    TestTrue(TEXT("the entry has no blueprint"), Entry && Entry->WidgetClass.IsNull());
  }
  CleanUp(EntryId, Source.CacheDir);
  return true;
}

#endif
