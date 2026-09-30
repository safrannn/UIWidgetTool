#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Animation/WidgetAnimation.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/PanelWidget.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Core/UIWTDesignImport.h"
#include "Core/UIWTGeneratedBlueprints.h"
#include "Core/UIWTRunImages.h"
#include "Core/UIWTWidgetSpec.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Tests/UIWTSpecTestWidget.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

// Tests for the re-import merge (import-tree.md → Implementation order,
// Phase 4; the tests are listed under Build and test). Designs are written
// to the automation transient folder and imported into /Temp without
// saving, except the discard test, which needs the blueprint on disk and
// imports into a /Game folder it deletes afterwards.
namespace
{
  using namespace UIWTDesignTree;

  constexpr EAutomationTestFlags TestFlags =
      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

  class FUIWTReimportTestBase : public FAutomationTestBase
  {
  public:
    FUIWTReimportTestBase(const FString &InName, const bool bInComplexTask)
        : FAutomationTestBase(InName, bInComplexTask)
    {
    }
    virtual bool SuppressLogErrors() override { return true; }
    virtual bool SuppressLogWarnings() override { return true; }

  protected:
    FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FString TargetFolder = TEXT("/Temp/UIWTReimportTest_") + Id;

    FString DesignDir() const
    {
      return FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() /
                                               TEXT("UIWTReimport") / Id);
    }

    FString DesignFile() const { return DesignDir() / TEXT("design.json"); }

    void WriteDesign(const FString &InJson)
    {
      if (!FFileHelper::SaveStringToFile(InJson, *DesignFile()))
      {
        AddError(TEXT("can't write the design"));
      }
    }

    void WritePng(const FString &InRelative, FColor InColor)
    {
      FImage Image(8, 8, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
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

    UWidgetBlueprint *Import(const FString &InJson, UClass *InParent = nullptr, bool bInSave = false)
    {
      WriteDesign(InJson);
      UIWTDesignImport::FRequest Request;
      Request.DesignFile = DesignFile();
      Request.TargetFolder = TargetFolder;
      Request.BlueprintName = TEXT("WBP_Test");
      Request.ParentClass = InParent;
      Request.ComponentsFolder = TargetFolder / TEXT("Components");
      Request.bSave = bInSave;
      UIWTDesignImport::FResult Result;
      FString Error;
      if (!UIWTDesignImport::Import(Request, Result, Error))
      {
        AddError(TEXT("import failed: ") + Error);
        return nullptr;
      }
      for (const FString &ApplyError : Result.ApplyErrors)
      {
        AddError(TEXT("import Apply: ") + ApplyError);
      }
      return Result.Blueprint;
    }

    // Plans a re-import of the design InJson into InBlueprint.
    bool MakePlan(UWidgetBlueprint *InBlueprint, const FString &InJson,
              UIWTDesignImport::FReimportPlan &OutPlan)
    {
      WriteDesign(InJson);
      UIWTDesignImport::FReimportRequest Request;
      Request.Blueprint = InBlueprint;
      Request.DesignFile = DesignFile();
      FString Error;
      if (!UIWTDesignImport::PlanReimport(Request, OutPlan, Error))
      {
        AddError(TEXT("plan failed: ") + Error);
        return false;
      }
      AddInfo(UIWTDesignImport::PlanToJson(OutPlan));
      return true;
    }

    bool ApplyPlan(const UIWTDesignImport::FReimportPlan &InPlan,
                   UIWTDesignImport::FReimportResult &OutResult)
    {
      FString Error;
      if (!UIWTDesignImport::ApplyReimport(InPlan, OutResult, Error))
      {
        AddError(TEXT("apply failed: ") + Error);
        return false;
      }
      for (const FString &ApplyError : OutResult.ApplyErrors)
      {
        AddError(TEXT("Apply: ") + ApplyError);
      }
      AddInfo(OutResult.ApplyReport);
      return true;
    }

    bool HasReport(const UIWTDesignImport::FReimportPlan &InPlan, const FString &InNode,
                   const FString &InCategory)
    {
      return InPlan.Report.ContainsByPredicate(
          [&](const FReportEntry &Entry)
          { return Entry.Node == InNode && Entry.Category == InCategory; });
    }

    UWidget *Widget(UWidgetBlueprint *InBlueprint, const TCHAR *InName)
    {
      return InBlueprint && InBlueprint->WidgetTree
                 ? InBlueprint->WidgetTree->FindWidget(FName(InName))
                 : nullptr;
    }

    FString TextOf(UWidgetBlueprint *InBlueprint, const TCHAR *InName)
    {
      const UTextBlock *Text = Cast<UTextBlock>(Widget(InBlueprint, InName));
      return Text ? Text->GetText().ToString() : FString(TEXT("(no TextBlock)"));
    }

    void CleanUp()
    {
      IFileManager::Get().DeleteDirectory(*DesignDir(), false, true);
      const FString OnDisk = UIWTGenerated::GetFolderOnDisk(TargetFolder);
      if (!OnDisk.IsEmpty())
      {
        IFileManager::Get().DeleteDirectory(*OnDisk, false, true);
      }
    }
  };

  // A design whose values include class defaults (a white shape, white
  // text, zero slot padding), which Export leaves out.
  const TCHAR *const UnchangedDesign = TEXT(R"json({
    "version": 1, "source": "figma", "sourceRef": { "fileKey": "reimport", "nodeId": "u:root" },
    "referenceSize": { "w": 400, "h": 300 },
    "root": {
      "id": "u:root", "name": "Screen", "kind": "frame", "box": { "x": 0, "y": 0, "w": 400, "h": 300 },
      "fill": { "color": "#101820" },
      "children": [
        { "id": "u:white", "name": "White", "kind": "shape", "box": { "x": 10, "y": 10, "w": 50, "h": 30 },
          "fill": { "color": "#FFFFFF" } },
        { "id": "u:card", "name": "Card", "kind": "shape", "box": { "x": 70, "y": 10, "w": 100, "h": 60 },
          "constraints": { "h": "right", "v": "center" }, "fill": { "color": "#3366FF" },
          "stroke": { "color": "#000000", "weights": { "l": 2, "t": 2, "r": 2, "b": 2 }, "align": "center" },
          "radii": [8, 8, 8, 8], "opacity": 0.8 },
        { "id": "u:title", "name": "Title", "kind": "text", "box": { "x": 10, "y": 80, "w": 200, "h": 30 },
          "text": { "content": "Title", "runs": [ { "font": { "family": "Unmapped" }, "size": 21, "color": "#FFFFFF", "lineHeight": 25 } ] } },
        { "id": "u:body", "name": "Body", "kind": "text", "box": { "x": 10, "y": 120, "w": 150, "h": 40 },
          "text": { "content": "Some wrapping body text", "sizing": "fixedWidth", "align": "center",
                    "runs": [ { "font": { "family": "Unmapped" }, "size": 13, "color": "#CCCCCC", "tracking": 15 } ] } },
        { "id": "u:icon", "name": "Icon", "kind": "image", "box": { "x": 300, "y": 10, "w": 32, "h": 32 },
          "constraints": { "h": "scale", "v": "top" },
          "image": { "path": "images/icon.png", "scale": 0.25, "size": { "w": 8, "h": 8 } } },
        { "id": "u:row", "name": "Row", "kind": "frame", "box": { "x": 10, "y": 200, "w": 300, "h": 40 },
          "layout": { "mode": "horizontal", "spacing": 8, "padding": { "l": 4, "t": 4, "r": 4, "b": 4 },
                      "align": { "main": "center", "cross": "center" } },
          "fill": { "color": "#222222" },
          "children": [
            { "id": "u:a", "name": "A", "kind": "shape", "box": { "x": 0, "y": 0, "w": 20, "h": 20 }, "fill": { "color": "#FF0000" } },
            { "id": "u:b", "name": "B", "kind": "shape", "box": { "x": 0, "y": 0, "w": 20, "h": 20 },
              "sizing": { "h": "fill" }, "fill": { "color": "#00FF00" } }
          ] },
        { "id": "u:group", "name": "Faded", "kind": "group", "box": { "x": 200, "y": 100, "w": 100, "h": 50 },
          "opacity": 0.5, "rotation": 10,
          "children": [
            { "id": "u:g1", "name": "Inside", "kind": "shape", "box": { "x": 0, "y": 0, "w": 100, "h": 50 },
              "fill": { "color": "#FFFF00" } }
          ] }
      ]
    }
  })json");
}

// Re-importing an unchanged design into an untouched blueprint changes
// nothing and reports nothing: every value survives the float round trip
// and "missing means default".
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTReimportUnchangedTest, FUIWTReimportTestBase,
                                        "UIWidgetTool.DesignReimport.Unchanged", TestFlags)

bool FUIWTReimportUnchangedTest::RunTest(const FString &Parameters)
{
  WritePng(TEXT("images/icon.png"), FColor::Red);
  UWidgetBlueprint *Blueprint = Import(UnchangedDesign);
  UIWTDesignImport::FReimportPlan Plan;
  if (!Blueprint || !MakePlan(Blueprint, UnchangedDesign, Plan))
  {
    CleanUp();
    return false;
  }
  TestEqual(TEXT("no design changes"), Plan.Changes.Num(), 0);
  TestEqual(TEXT("no conflicts"), Plan.Conflicts, 0);
  TestEqual(TEXT("no UE-side changes"), Plan.KeptUEChanges, 0);
  TestEqual(TEXT("no widgets added in UE"), Plan.UEWidgets, 0);
  for (const FReportEntry &Entry : Plan.Report)
  {
    if (Entry.Category == TEXT("keptUEChange") || Entry.Category == TEXT("conflict"))
    {
      AddError(FString::Printf(TEXT("%s: %s"), *Entry.Category, *Entry.Detail));
    }
  }
  UIWTDesignImport::FReimportResult Applied;
  if (ApplyPlan(Plan, Applied))
  {
    TestTrue(TEXT("every widget kept"), Applied.ApplyReport.Contains(TEXT("0 created")) &&
                                            Applied.ApplyReport.Contains(TEXT("0 removed")));
    TestEqual(TEXT("no new textures"), Applied.TexturesCreated, 0);
    FString Error;
    TestTrue(TEXT("accepted"), UIWTDesignImport::AcceptReimport(Blueprint, false, Error));
    TestFalse(TEXT("nothing pending"), UIWTDesignImport::HasPendingReimport(Blueprint));
  }
  CleanUp();
  return true;
}

// The merge rules: a value changed only in UE stays, one changed only in
// the design is taken, one changed in both takes the design's with a
// conflict; widgets added in UE stay; removed layers go, new ones come.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTReimportMergeTest, FUIWTReimportTestBase,
                                        "UIWidgetTool.DesignReimport.Merge", TestFlags)

bool FUIWTReimportMergeTest::RunTest(const FString &Parameters)
{
  const FString Before = TEXT(R"json({
    "version": 1, "source": "figma", "referenceSize": { "w": 400, "h": 300 },
    "root": { "id": "m:root", "name": "Screen", "kind": "frame", "box": { "x": 0, "y": 0, "w": 400, "h": 300 },
      "children": [
        { "id": "m:title", "name": "Title", "kind": "text", "box": { "x": 10, "y": 10, "w": 200, "h": 30 },
          "text": { "content": "Design title", "runs": [ { "font": { "family": "Unmapped" }, "size": 20, "color": "#FFFFFF" } ] } },
        { "id": "m:sub", "name": "Subtitle", "kind": "text", "box": { "x": 10, "y": 50, "w": 200, "h": 30 },
          "text": { "content": "Design subtitle", "runs": [ { "font": { "family": "Unmapped" }, "size": 16, "color": "#FFFFFF" } ] } },
        { "id": "m:note", "name": "Note", "kind": "text", "box": { "x": 10, "y": 90, "w": 200, "h": 30 },
          "text": { "content": "Design note", "runs": [ { "font": { "family": "Unmapped" }, "size": 16, "color": "#FFFFFF" } ] } },
        { "id": "m:keep", "name": "Keep", "kind": "shape", "box": { "x": 10, "y": 130, "w": 20, "h": 20 }, "fill": { "color": "#FF0000" } },
        { "id": "m:gone", "name": "Gone", "kind": "shape", "box": { "x": 40, "y": 130, "w": 20, "h": 20 }, "fill": { "color": "#00FF00" } }
      ] } })json");
  const FString After = TEXT(R"json({
    "version": 1, "source": "figma", "referenceSize": { "w": 400, "h": 300 },
    "root": { "id": "m:root", "name": "Screen", "kind": "frame", "box": { "x": 0, "y": 0, "w": 400, "h": 300 },
      "children": [
        { "id": "m:title", "name": "Title", "kind": "text", "box": { "x": 10, "y": 10, "w": 200, "h": 30 },
          "text": { "content": "Design title", "runs": [ { "font": { "family": "Unmapped" }, "size": 20, "color": "#FFFFFF" } ] } },
        { "id": "m:sub", "name": "Subtitle", "kind": "text", "box": { "x": 10, "y": 50, "w": 200, "h": 30 },
          "text": { "content": "New subtitle", "runs": [ { "font": { "family": "Unmapped" }, "size": 16, "color": "#FFFFFF" } ] } },
        { "id": "m:note", "name": "Note", "kind": "text", "box": { "x": 10, "y": 90, "w": 200, "h": 30 },
          "text": { "content": "Changed in the design", "runs": [ { "font": { "family": "Unmapped" }, "size": 16, "color": "#FFFFFF" } ] } },
        { "id": "m:keep", "name": "Keep", "kind": "shape", "box": { "x": 10, "y": 130, "w": 20, "h": 20 }, "fill": { "color": "#FF0000" } },
        { "id": "m:new", "name": "Fresh", "kind": "shape", "box": { "x": 70, "y": 130, "w": 20, "h": 20 }, "fill": { "color": "#0000FF" } }
      ] } })json");

  UWidgetBlueprint *Blueprint = Import(Before);
  if (!Blueprint)
  {
    CleanUp();
    return false;
  }
  // Changes made in UE: two texts, and a widget of its own in the canvas.
  Cast<UTextBlock>(Widget(Blueprint, TEXT("Title")))->SetText(FText::FromString(TEXT("UE title")));
  Cast<UTextBlock>(Widget(Blueprint, TEXT("Note")))->SetText(FText::FromString(TEXT("UE note")));
  UPanelWidget *Canvas = Widget(Blueprint, TEXT("Title"))->GetParent();
  USpacer *Extra = Blueprint->WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass(), TEXT("Extra"));
  if (!TestNotNull(TEXT("canvas"), Canvas))
  {
    CleanUp();
    return false;
  }
  Canvas->AddChild(Extra);

  UIWTDesignImport::FReimportPlan Plan;
  if (!MakePlan(Blueprint, After, Plan))
  {
    CleanUp();
    return false;
  }
  TArray<FString> ChangedNodes;
  for (const UIWTDesignMerge::FChange &Change : Plan.Changes)
  {
    ChangedNodes.Add(Change.Node);
  }
  TestEqual(TEXT("changes"), FString::Join(ChangedNodes, TEXT(",")),
            FString(TEXT("m:gone,m:new,m:note,m:sub")));
  TestTrue(TEXT("both changed the note: a conflict"), HasReport(Plan, TEXT("m:note"), TEXT("conflict")));
  TestTrue(TEXT("the UE title is kept"), HasReport(Plan, TEXT("m:title"), TEXT("keptUEChange")));
  TestEqual(TEXT("one widget added in UE"), Plan.UEWidgets, 1);

  UIWTDesignImport::FReimportResult Applied;
  if (ApplyPlan(Plan, Applied))
  {
    TestEqual(TEXT("UE-only change kept"), TextOf(Blueprint, TEXT("Title")), FString(TEXT("UE title")));
    TestEqual(TEXT("design-only change taken"), TextOf(Blueprint, TEXT("Subtitle")),
              FString(TEXT("New subtitle")));
    TestEqual(TEXT("conflict: the design wins"), TextOf(Blueprint, TEXT("Note")),
              FString(TEXT("Changed in the design")));
    TestNotNull(TEXT("the UE widget stays"), Widget(Blueprint, TEXT("Extra")));
    TestNull(TEXT("the removed layer's widget is gone"), Widget(Blueprint, TEXT("Gone")));
    TestNotNull(TEXT("the new layer's widget is there"), Widget(Blueprint, TEXT("Fresh")));
    FString Error;
    UIWTDesignImport::AcceptReimport(Blueprint, false, Error);
  }
  CleanUp();
  return true;
}

// Textures keep their assets across re-imports: removing the node a shared
// texture was named after, and renaming another, creates nothing new.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTReimportTexturesTest, FUIWTReimportTestBase,
                                        "UIWidgetTool.DesignReimport.TextureStability", TestFlags)

bool FUIWTReimportTexturesTest::RunTest(const FString &Parameters)
{
  WritePng(TEXT("images/shared.png"), FColor::Red);
  WritePng(TEXT("images/other.png"), FColor::Green);
  const FString Before = TEXT(R"json({
    "version": 1, "source": "figma", "referenceSize": { "w": 400, "h": 300 },
    "root": { "id": "t:root", "name": "Screen", "kind": "frame", "box": { "x": 0, "y": 0, "w": 400, "h": 300 },
      "children": [
        { "id": "t:a", "name": "Alpha", "kind": "image", "box": { "x": 0, "y": 0, "w": 8, "h": 8 }, "image": { "path": "images/shared.png" } },
        { "id": "t:b", "name": "Beta", "kind": "image", "box": { "x": 10, "y": 0, "w": 8, "h": 8 }, "image": { "path": "images/shared.png" } },
        { "id": "t:c", "name": "Gamma", "kind": "image", "box": { "x": 20, "y": 0, "w": 8, "h": 8 }, "image": { "path": "images/other.png" } }
      ] } })json");
  const FString After = TEXT(R"json({
    "version": 1, "source": "figma", "referenceSize": { "w": 400, "h": 300 },
    "root": { "id": "t:root", "name": "Screen", "kind": "frame", "box": { "x": 0, "y": 0, "w": 400, "h": 300 },
      "children": [
        { "id": "t:b", "name": "Bravo", "kind": "image", "box": { "x": 10, "y": 0, "w": 8, "h": 8 }, "image": { "path": "images/shared.png" } },
        { "id": "t:c", "name": "Gamma", "kind": "image", "box": { "x": 20, "y": 0, "w": 8, "h": 8 }, "image": { "path": "images/other.png" } }
      ] } })json");
  UWidgetBlueprint *Blueprint = Import(Before);
  UIWTDesignImport::FSidecar First;
  FString Error;
  if (!Blueprint ||
      !UIWTDesignImport::ReadSidecar(Blueprint->GetOutermost()->GetName(), First, Error))
  {
    AddError(Error);
    CleanUp();
    return false;
  }
  const FString SharedTexture = First.Textures.FindRef(TEXT("images/shared.png"));
  TestTrue(TEXT("shared texture named after the first user"), SharedTexture.Contains(TEXT("Alpha")));

  WritePng(TEXT("images/shared.png"), FColor::Blue);
  UIWTDesignImport::FReimportPlan Plan;
  UIWTDesignImport::FReimportResult Applied;
  if (MakePlan(Blueprint, After, Plan) && ApplyPlan(Plan, Applied))
  {
    TestEqual(TEXT("no texture created"), Applied.TexturesCreated, 0);
    TestEqual(TEXT("both textures written in place"), Applied.TexturesWritten, 2);
    UIWTDesignImport::AcceptReimport(Blueprint, false, Error);
    UIWTDesignImport::FSidecar Second;
    UIWTDesignImport::ReadSidecar(Blueprint->GetOutermost()->GetName(), Second, Error);
    TestEqual(TEXT("the shared texture keeps its asset"),
              Second.Textures.FindRef(TEXT("images/shared.png")), SharedTexture);
    UTexture2D *Texture = LoadObject<UTexture2D>(nullptr, *SharedTexture);
    if (TestNotNull(TEXT("texture"), Texture))
    {
      FImage Pixels;
      Texture->Source.GetMipImage(Pixels, 0);
      TestTrue(TEXT("with the new pixels"),
               Pixels.SizeX > 0 && Pixels.AsBGRA8()[0] == FColor::Blue);
    }
  }
  CleanUp();
  return true;
}

// Protected widgets: a BindWidget whose layer changed class keeps its class,
// an optional BindWidget and an animated widget whose layers were deleted
// stay, a property-bound widget follows a restyle and a move, and every
// binding and animation still works afterwards.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTReimportProtectedTest, FUIWTReimportTestBase,
                                        "UIWidgetTool.DesignReimport.Protected", TestFlags)

bool FUIWTReimportProtectedTest::RunTest(const FString &Parameters)
{
  WritePng(TEXT("images/icon.png"), FColor::Red);
  const FString Before = TEXT(R"json({
    "version": 1, "source": "figma", "referenceSize": { "w": 400, "h": 300 },
    "root": { "id": "p:root", "name": "Screen", "kind": "frame", "box": { "x": 0, "y": 0, "w": 400, "h": 300 },
      "children": [
        { "id": "p:req", "name": "RequiredText", "kind": "text", "box": { "x": 10, "y": 10, "w": 200, "h": 30 },
          "text": { "content": "Required", "runs": [ { "font": { "family": "Unmapped" }, "size": 16, "color": "#FFFFFF" } ] } },
        { "id": "p:opt", "name": "OptionalImage", "kind": "image", "box": { "x": 10, "y": 50, "w": 16, "h": 16 },
          "image": { "path": "images/icon.png" } },
        { "id": "p:bound", "name": "Bound", "kind": "text", "box": { "x": 10, "y": 80, "w": 200, "h": 30 },
          "text": { "content": "Bound", "runs": [ { "font": { "family": "Unmapped" }, "size": 16, "color": "#FFFFFF" } ] } },
        { "id": "p:anim", "name": "Animated", "kind": "image", "box": { "x": 10, "y": 120, "w": 16, "h": 16 },
          "image": { "path": "images/icon.png" } },
        { "id": "p:plain", "name": "Plain", "kind": "shape", "box": { "x": 10, "y": 150, "w": 16, "h": 16 }, "fill": { "color": "#FF0000" } }
      ] } })json");
  const FString After = TEXT(R"json({
    "version": 1, "source": "figma", "referenceSize": { "w": 400, "h": 300 },
    "root": { "id": "p:root", "name": "Screen", "kind": "frame", "box": { "x": 0, "y": 0, "w": 400, "h": 300 },
      "children": [
        { "id": "p:req", "name": "RequiredText", "kind": "frame", "box": { "x": 10, "y": 10, "w": 200, "h": 30 },
          "children": [
            { "id": "p:inner", "name": "Inner", "kind": "text", "box": { "x": 0, "y": 0, "w": 200, "h": 30 },
              "text": { "content": "Inner", "runs": [ { "font": { "family": "Unmapped" }, "size": 16, "color": "#FFFFFF" } ] } }
          ] },
        { "id": "p:group", "name": "Group", "kind": "frame", "box": { "x": 200, "y": 80, "w": 200, "h": 100 },
          "children": [
            { "id": "p:bound", "name": "Bound", "kind": "text", "box": { "x": 0, "y": 0, "w": 200, "h": 30 },
              "text": { "content": "Bound", "runs": [ { "font": { "family": "Unmapped" }, "size": 16, "color": "#FF0000" } ] } }
          ] },
        { "id": "p:plain", "name": "Plain", "kind": "shape", "box": { "x": 10, "y": 150, "w": 16, "h": 16 }, "fill": { "color": "#FF0000" } }
      ] } })json");

  UWidgetBlueprint *Blueprint = Import(Before, UUIWTSpecTestWidget::StaticClass());
  if (!Blueprint)
  {
    CleanUp();
    return false;
  }
  TestNotNull(TEXT("the BindWidget layer took its name"),
              Cast<UTextBlock>(Widget(Blueprint, TEXT("RequiredText"))));
  FDelegateEditorBinding Binding;
  Binding.ObjectName = TEXT("Bound");
  Binding.PropertyName = TEXT("Text");
  Blueprint->Bindings.Add(Binding);
  UWidgetAnimation *Animation = NewObject<UWidgetAnimation>(Blueprint, TEXT("Fade"));
  FWidgetAnimationBinding AnimationBinding;
  AnimationBinding.WidgetName = TEXT("Animated");
  Animation->AnimationBindings.Add(AnimationBinding);
  Blueprint->Animations.Add(Animation);

  UIWTDesignImport::FReimportPlan Plan;
  UIWTDesignImport::FReimportResult Applied;
  if (MakePlan(Blueprint, After, Plan))
  {
    TestTrue(TEXT("protectedClass"), HasReport(Plan, TEXT("p:req"), TEXT("protectedClass")));
    TestTrue(TEXT("protectedKept for the optional BindWidget"),
             HasReport(Plan, TEXT("p:opt"), TEXT("protectedKept")));
    TestTrue(TEXT("protectedKept for the animated widget"),
             HasReport(Plan, TEXT("p:anim"), TEXT("protectedKept")));
    if (ApplyPlan(Plan, Applied))
    {
      TestNotNull(TEXT("RequiredText is still a TextBlock"),
                  Cast<UTextBlock>(Widget(Blueprint, TEXT("RequiredText"))));
      TestNotNull(TEXT("OptionalImage stays"), Widget(Blueprint, TEXT("OptionalImage")));
      TestNotNull(TEXT("Animated stays"), Widget(Blueprint, TEXT("Animated")));
      TestNotNull(TEXT("the new layer inside the frame is built"), Widget(Blueprint, TEXT("Inner")));
      const UWidget *Bound = Widget(Blueprint, TEXT("Bound"));
      TestTrue(TEXT("the bound widget moved into Group"),
               Bound && Bound->GetParent() && Bound->GetParent()->GetName() == TEXT("Group"));
      TestEqual(TEXT("the property binding is still there"), Blueprint->Bindings.Num(), 1);
      TestFalse(TEXT("the blueprint compiles"), Applied.ApplyReport.Contains(TEXT("FAILED")));
    }
  }
  if (UIWTDesignImport::HasPendingReimport(Blueprint))
  {
    FString Error;
    UIWTDesignImport::AcceptReimport(Blueprint, false, Error);
  }
  Blueprint->Animations.Empty();
  Blueprint->Bindings.Empty();
  CleanUp();
  return true;
}

// The preview lists exactly the nodes added, removed and changed; Discard
// puts the blueprint back as it is on disk and leaves the sidecar alone.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTReimportPreviewTest, FUIWTReimportTestBase,
                                        "UIWidgetTool.DesignReimport.PreviewAndDiscard", TestFlags)

bool FUIWTReimportPreviewTest::RunTest(const FString &Parameters)
{
  // Discard reloads from disk, so this blueprint is saved.
  TargetFolder = TEXT("/Game/UIWTTestTemp_") + Id;
  WritePng(TEXT("images/icon.png"), FColor::Red);
  WritePng(TEXT("images/new.png"), FColor::Green);
  const FString Before = TEXT(R"json({
    "version": 1, "source": "figma", "referenceSize": { "w": 400, "h": 300 },
    "root": { "id": "v:root", "name": "Screen", "kind": "frame", "box": { "x": 0, "y": 0, "w": 400, "h": 300 },
      "children": [
        { "id": "v:a", "name": "A", "kind": "shape", "box": { "x": 0, "y": 0, "w": 10, "h": 10 }, "fill": { "color": "#FF0000" } },
        { "id": "v:b", "name": "B", "kind": "shape", "box": { "x": 20, "y": 0, "w": 10, "h": 10 }, "fill": { "color": "#00FF00" } },
        { "id": "v:c", "name": "C", "kind": "image", "box": { "x": 40, "y": 0, "w": 8, "h": 8 }, "image": { "path": "images/icon.png" } }
      ] } })json");
  const FString After = TEXT(R"json({
    "version": 1, "source": "figma", "referenceSize": { "w": 400, "h": 300 },
    "root": { "id": "v:root", "name": "Screen", "kind": "frame", "box": { "x": 0, "y": 0, "w": 400, "h": 300 },
      "children": [
        { "id": "v:a", "name": "A", "kind": "shape", "box": { "x": 0, "y": 0, "w": 10, "h": 10 }, "fill": { "color": "#FF0000" } },
        { "id": "v:b", "name": "B", "kind": "shape", "box": { "x": 20, "y": 0, "w": 10, "h": 10 }, "fill": { "color": "#0000FF" } },
        { "id": "v:d", "name": "D", "kind": "image", "box": { "x": 60, "y": 0, "w": 8, "h": 8 }, "image": { "path": "images/new.png" } }
      ] } })json");

  UWidgetBlueprint *Blueprint = Import(Before, nullptr, true);
  if (!Blueprint)
  {
    CleanUp();
    return false;
  }
  const FString Package = Blueprint->GetOutermost()->GetName();
  const FString AssetFile =
      FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
  const FString SidecarFile = UIWTDesignImport::GetSidecarPath(Package);
  TArray<uint8> AssetBefore;
  TArray<uint8> SidecarBefore;
  FFileHelper::LoadFileToArray(AssetBefore, *AssetFile);
  FFileHelper::LoadFileToArray(SidecarBefore, *SidecarFile);
  FString ExportBefore;
  FString Error;
  UIWTWidgetSpec::Export(Blueprint, ExportBefore, Error);

  UIWTDesignImport::FReimportPlan Plan;
  UIWTDesignImport::FReimportResult Applied;
  if (MakePlan(Blueprint, After, Plan))
  {
    TArray<FString> Lines;
    for (const UIWTDesignMerge::FChange &Change : Plan.Changes)
    {
      Lines.Add(Change.Node + TEXT(" ") +
                (Change.Change == UIWTDesignMerge::EChange::Added     ? TEXT("added")
                 : Change.Change == UIWTDesignMerge::EChange::Removed ? TEXT("removed")
                                                                      : TEXT("changed")));
    }
    TestEqual(TEXT("exactly the changed nodes"), FString::Join(Lines, TEXT(", ")),
              FString(TEXT("v:b changed, v:c removed, v:d added")));
    if (ApplyPlan(Plan, Applied))
    {
      TestEqual(TEXT("one new texture"), Applied.TexturesCreated, 1);
      TestNotNull(TEXT("applied: D is there"), Widget(Blueprint, TEXT("D")));
      TestTrue(TEXT("discarded"), UIWTDesignImport::DiscardReimport(Blueprint, Error));
      if (!Error.IsEmpty())
      {
        AddError(Error);
      }
      UWidgetBlueprint *Reloaded = LoadObject<UWidgetBlueprint>(
          nullptr, *(Package + TEXT(".") + FPackageName::GetShortName(Package)));
      FString ExportAfter;
      UIWTWidgetSpec::Export(Reloaded, ExportAfter, Error);
      TestEqual(TEXT("the blueprint is as before"), ExportAfter, ExportBefore);
      TArray<uint8> AssetAfter;
      TArray<uint8> SidecarAfter;
      FFileHelper::LoadFileToArray(AssetAfter, *AssetFile);
      FFileHelper::LoadFileToArray(SidecarAfter, *SidecarFile);
      TestTrue(TEXT("the asset file is untouched"), AssetAfter == AssetBefore);
      TestTrue(TEXT("the sidecar is untouched"), SidecarAfter == SidecarBefore);
      TestFalse(TEXT("nothing pending"), UIWTDesignImport::HasPendingReimport(Reloaded));
      Blueprint = Reloaded;
    }
  }

  // Remove everything this test saved under /Game.
  TArray<UObject *> Assets;
  if (Blueprint)
  {
    Assets.Add(Blueprint);
  }
  TArray<FAssetData> InFolder;
  IAssetRegistry::GetChecked().GetAssetsByPath(FName(*(TargetFolder / TEXT("WBP_Test"))), InFolder,
                                               true);
  for (const FAssetData &Asset : InFolder)
  {
    if (UObject *Object = Asset.GetAsset())
    {
      Assets.AddUnique(Object);
    }
  }
  ObjectTools::ForceDeleteObjects(Assets, false);
  CleanUp();
  return true;
}

#endif
