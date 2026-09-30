#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Claude/UIWTClaudeRunner.h"
#include "Claude/UIWTClaudeService.h"
#include "Claude/UIWTToolset.h"
#include "Core/UIWTDesignImport.h"
#include "Core/UIWTGeneratedBlueprints.h"
#include "Core/UIWTLocalSettings.h"
#include "Core/UIWTRunImages.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Core/UIWTWidgetSpec.h"
#include "Design/UIWTDesignRefine.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

// Tests for the AI pass after a design import (import-tree.md →
// Implementation order, Phase 7): the cost estimate, the subtree specs, the
// renames written into the sidecar and kept by a re-import, and what the
// pass reads (outline, nodes, request). No Claude process runs.
namespace
{
  using namespace UIWTDesignTree;

  constexpr EAutomationTestFlags TestFlags =
      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

  class FUIWTRefineTestBase : public FAutomationTestBase
  {
  public:
    FUIWTRefineTestBase(const FString &InName, const bool bInComplexTask)
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
                                               TEXT("UIWTDesignRefine") / Id);
    }

    FString TargetFolder() const { return TEXT("/Temp/UIWTRefineTest_") + Id; }

    UWidgetBlueprint *MakeBlueprint()
    {
      UPackage *Package = GetTransientPackage();
      const FName Name = MakeUniqueObjectName(Package, UWidgetBlueprint::StaticClass(),
                                              TEXT("WBP_UIWTSubtreeTest"));
      return Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
          UUserWidget::StaticClass(), Package, Name, BPTYPE_Normal,
          UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
    }

    FString ExportOf(UWidgetBlueprint *InBlueprint)
    {
      FString Json;
      FString Error;
      UIWTWidgetSpec::Export(InBlueprint, Json, Error);
      return Json;
    }

    UWidgetBlueprint *ImportDesign(const FString &InJson)
    {
      const FString DesignFile = DesignDir() / TEXT("design.json");
      if (!FFileHelper::SaveStringToFile(InJson, *DesignFile))
      {
        AddError(TEXT("can't write ") + DesignFile);
        return nullptr;
      }
      UIWTDesignImport::FRequest Request;
      Request.DesignFile = DesignFile;
      Request.TargetFolder = TargetFolder();
      Request.BlueprintName = TEXT("WBP_Refine");
      Request.ComponentsFolder = TargetFolder() / TEXT("Components");
      Request.bSave = false;
      FString Error;
      if (!UIWTDesignImport::Import(Request, Imported, Error))
      {
        AddError(TEXT("import failed: ") + Error);
        return nullptr;
      }
      return Imported.Blueprint;
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

    UIWTDesignImport::FResult Imported;
  };

  const TCHAR *DesignJson = TEXT(R"json({
    "version": 1, "source": "figma",
    "sourceRef": { "fileKey": "refine", "nodeId": "1:1" },
    "referenceSize": { "w": 640, "h": 360 },
    "root": {
      "id": "1:1", "name": "Refine Test", "kind": "frame",
      "box": { "x": 0, "y": 0, "w": 640, "h": 360 },
      "fill": { "color": "#101820" },
      "children": [
        { "id": "1:2", "name": "Title", "kind": "text", "box": { "x": 60, "y": 10, "w": 200, "h": 30 },
          "text": { "content": "Hello", "runs": [ { "font": { "family": "Unmapped" }, "size": 24, "color": "#FFFFFF" } ] } },
        { "id": "1:3", "name": "Plate", "kind": "frame", "box": { "x": 10, "y": 60, "w": 120, "h": 40 },
          "fill": { "color": "#3366FF" }, "hints": [ "prefix:btn" ],
          "children": [
            { "id": "1:4", "name": "Label", "kind": "text", "box": { "x": 8, "y": 8, "w": 104, "h": 24 },
              "text": { "content": "OK", "runs": [ { "font": { "family": "Unmapped" }, "size": 16, "color": "#FFFFFF" } ] } }
          ] }
      ]
    }
  })json");
}

// The estimate follows import-tree.md's table for subtree specs (within
// 20%), and says so when it has no prices for the model.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTRefineEstimateTest, FUIWTRefineTestBase,
                                        "UIWidgetTool.DesignRefine.Estimate", TestFlags)

bool FUIWTRefineEstimateTest::RunTest(const FString &Parameters)
{
  using UIWTDesignRefine::EPrices;
  struct FRow
  {
    int32 Nodes;
    double Opus3, Opus6, Sonnet3, Sonnet6;
  };
  const FRow Table[] = {{100, 1.30, 2.50, 0.70, 1.40},
                        {300, 1.50, 2.80, 0.80, 1.60},
                        {1000, 2.10, 3.50, 1.20, 2.10}};
  auto Check = [this](int32 InNodes, int32 InRounds, EPrices InPrices, double InTable)
  {
    const double Dollars = UIWTDesignRefine::Estimate(InNodes, InRounds, InPrices).Dollars;
    if (FMath::Abs(Dollars - InTable) > InTable * 0.2)
    {
      AddError(FString::Printf(TEXT("%d nodes, %d rounds, %s: $%.2f, the table says $%.2f"),
                               InNodes, InRounds,
                               InPrices == EPrices::Opus ? TEXT("Opus") : TEXT("Sonnet"),
                               Dollars, InTable));
    }
  };
  for (const FRow &Row : Table)
  {
    Check(Row.Nodes, 3, EPrices::Opus, Row.Opus3);
    Check(Row.Nodes, 6, EPrices::Opus, Row.Opus6);
    Check(Row.Nodes, 3, EPrices::Sonnet, Row.Sonnet3);
    Check(Row.Nodes, 6, EPrices::Sonnet, Row.Sonnet6);
  }
  const UIWTDesignRefine::FEstimate Unknown =
      UIWTDesignRefine::Estimate(300, 3, EPrices::Unknown);
  TestTrue(TEXT("no price for an unknown model"), Unknown.Dollars < 0.0);
  TestTrue(TEXT("tokens still estimated"), Unknown.InputTokens > 0 && Unknown.OutputTokens > 0);
  TestTrue(TEXT("more nodes read more"),
           UIWTDesignRefine::Estimate(1000, 3, EPrices::Sonnet).InputTokens >
               UIWTDesignRefine::Estimate(100, 3, EPrices::Sonnet).InputTokens);
  TestTrue(TEXT("Sonnet by name"),
           UIWTDesignRefine::PricesFor(EUIWTClaudeModel::Custom, TEXT("claude-sonnet-5-5")) ==
               EPrices::Sonnet);
  return true;
}

// ApplySubtree changes only the named subtree, keeps the root's slot,
// refuses a name used outside it, and ends where a whole-spec Apply of the
// same change ends.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTRefineSubtreeTest, FUIWTRefineTestBase,
                                        "UIWidgetTool.DesignRefine.Subtree", TestFlags)

bool FUIWTRefineSubtreeTest::RunTest(const FString &Parameters)
{
  const FString Before = TEXT(R"json({"root":{"class":"CanvasPanel","name":"Root","children":[
    {"class":"Border","name":"Card",
     "slot":{"LayoutData":{"Offsets":{"Left":10,"Top":20,"Right":200,"Bottom":100}}},
     "children":[{"class":"TextBlock","name":"Title","props":{"Text":"Hello"}}]},
    {"class":"Image","name":"Other","slot":{"LayoutData":{"Offsets":{"Left":300,"Top":20,"Right":32,"Bottom":32}}}}]}})json");
  // The same change, as a subtree and as a whole spec.
  const FString CardAfter = TEXT(R"json({"root":{"class":"Border","name":"Card","children":[
    {"class":"HorizontalBox","name":"Row","children":[
      {"class":"TextBlock","name":"Title","props":{"Text":"Hello"}},
      {"class":"TextBlock","name":"Subtitle","props":{"Text":"World"}}]}]}})json");
  const FString WholeAfter = TEXT(R"json({"root":{"class":"CanvasPanel","name":"Root","children":[
    {"class":"Border","name":"Card",
     "slot":{"LayoutData":{"Offsets":{"Left":10,"Top":20,"Right":200,"Bottom":100}}},
     "children":[{"class":"HorizontalBox","name":"Row","children":[
       {"class":"TextBlock","name":"Title","props":{"Text":"Hello"}},
       {"class":"TextBlock","name":"Subtitle","props":{"Text":"World"}}]}]},
    {"class":"Image","name":"Other","slot":{"LayoutData":{"Offsets":{"Left":300,"Top":20,"Right":32,"Bottom":32}}}}]}})json");

  UWidgetBlueprint *Blueprint = MakeBlueprint();
  UWidgetBlueprint *Reference = MakeBlueprint();
  FString Report;
  TArray<FString> Errors;
  if (!TestTrue(TEXT("setup"), Blueprint && Reference &&
                                   UIWTWidgetSpec::Apply(Blueprint, Before, Report, Errors) &&
                                   UIWTWidgetSpec::Apply(Reference, Before, Report, Errors)))
  {
    return false;
  }
  UWidget *Title = Blueprint->WidgetTree->FindWidget(TEXT("Title"));
  UWidget *Other = Blueprint->WidgetTree->FindWidget(TEXT("Other"));

  FString Subtree;
  FString Error;
  TestTrue(TEXT("ExportSubtree"),
           UIWTWidgetSpec::ExportSubtree(Blueprint, TEXT("Card"), Subtree, Error));
  TestTrue(TEXT("the subtree has the child"), Subtree.Contains(TEXT("\"Title\"")));
  TestTrue(TEXT("the subtree has its slot"), Subtree.Contains(TEXT("\"slot\"")));
  TestFalse(TEXT("the subtree leaves the rest out"), Subtree.Contains(TEXT("\"Other\"")));
  TestFalse(TEXT("unknown widget"),
            UIWTWidgetSpec::ExportSubtree(Blueprint, TEXT("Nope"), Subtree, Error));

  // A name from outside is refused, and nothing changes.
  const FString Stolen = TEXT(R"json({"root":{"class":"Border","name":"Card","children":[
    {"class":"Image","name":"Other"}]}})json");
  const FString Untouched = ExportOf(Blueprint);
  TestFalse(TEXT("outside name refused"),
            UIWTWidgetSpec::ApplySubtree(Blueprint, TEXT("Card"), Stolen, Report, Errors));
  TestTrue(TEXT("refusal says why"),
           Errors.ContainsByPredicate([](const FString &E) { return E.Contains(TEXT("outside")); }));
  TestEqual(TEXT("refused: nothing changed"), ExportOf(Blueprint), Untouched);

  TestTrue(TEXT("ApplySubtree"),
           UIWTWidgetSpec::ApplySubtree(Blueprint, TEXT("Card"), CardAfter, Report, Errors));
  TestEqual(TEXT("no property errors"), Errors.Num(), 0);
  TestTrue(TEXT("Title kept (same object)"),
           Blueprint->WidgetTree->FindWidget(TEXT("Title")) == Title);
  TestTrue(TEXT("Other kept (same object)"),
           Blueprint->WidgetTree->FindWidget(TEXT("Other")) == Other);
  TestNotNull(TEXT("Subtitle created"), Blueprint->WidgetTree->FindWidget(TEXT("Subtitle")));

  TestTrue(TEXT("whole-spec Apply"),
           UIWTWidgetSpec::Apply(Reference, WholeAfter, Report, Errors));
  TestEqual(TEXT("same result as a whole-spec Apply (the Card kept its slot)"),
            ExportOf(Blueprint), ExportOf(Reference));
  return true;
}

// A rename made with RenameWidgets keeps the widget's GUID, SyncRenames
// writes it into the sidecar, and a re-import of the unchanged design keeps
// the new name: no removal, no widget added in UE.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTRefineRenameTest, FUIWTRefineTestBase,
                                        "UIWidgetTool.DesignRefine.Renames", TestFlags)

bool FUIWTRefineRenameTest::RunTest(const FString &Parameters)
{
  UWidgetBlueprint *Blueprint = ImportDesign(DesignJson);
  FUIWTClaudeService *Service = FUIWTClaudeService::TryGet();
  if (!Blueprint || !TestNotNull(TEXT("service"), Service))
  {
    CleanUp();
    return false;
  }
  UIWTDesignImport::FSidecar Sidecar;
  FString Error;
  UIWTDesignImport::ReadSidecar(Blueprint->GetOutermost()->GetName(), Sidecar, Error);
  const FString *TitleWidget =
      Sidecar.Owned.Contains(TEXT("1:2")) ? Sidecar.Owned[TEXT("1:2")].Find(TEXT("main")) : nullptr;
  if (!TestNotNull(TEXT("the title's widget"), TitleWidget))
  {
    CleanUp();
    return false;
  }
  const FString OldName = *TitleWidget;
  const TMap<FGuid, FName> Before = UIWTDesignRefine::SnapshotWidgets(Blueprint);
  TestTrue(TEXT("widgets have GUIDs"), Before.Num() >= 3);

  // RenameWidgets needs a run: a run without a Claude process.
  const FGuid EntryId = FGuid::NewGuid();
  FText RunError;
  if (!TestTrue(TEXT("dev run"), Service->DevBeginRun(EntryId, Blueprint, nullptr, RunError)))
  {
    AddError(RunError.ToString());
    CleanUp();
    return false;
  }
  const FString Renamed = UUIWTToolset::RenameWidgets(
      Blueprint, FString::Printf(TEXT("{\"%s\": \"Txt_Title\"}"), *OldName));
  TestTrue(TEXT("renamed"), Renamed.Contains(TEXT("Txt_Title")));
  TestTrue(TEXT("a taken name is refused"),
           UUIWTToolset::RenameWidgets(Blueprint, TEXT("{\"Txt_Title\": \"Label\"}")).IsEmpty());
  Service->DevEndRun(true);
  Service->ForgetChatState(EntryId);
  TestNotNull(TEXT("new name"), Blueprint->WidgetTree->FindWidget(TEXT("Txt_Title")));

  int32 Count = 0;
  TestTrue(TEXT("SyncRenames"), UIWTDesignRefine::SyncRenames(Blueprint, Before, Count, Error));
  TestEqual(TEXT("one rename"), Count, 1);
  UIWTDesignImport::ReadSidecar(Blueprint->GetOutermost()->GetName(), Sidecar, Error);
  TestTrue(TEXT("owned has the new name"),
           Sidecar.Owned.Contains(TEXT("1:2")) &&
               Sidecar.Owned[TEXT("1:2")].FindRef(TEXT("main")) == TEXT("Txt_Title"));
  TestTrue(TEXT("base widgets renamed"), Sidecar.Widgets.Contains(TEXT("Txt_Title")) &&
                                             !Sidecar.Widgets.Contains(OldName));

  // The outline and the request name the new widget.
  UIWTDesignRefine::FPass Pass;
  if (TestTrue(TEXT("Prepare"), UIWTDesignRefine::Prepare(Blueprint, 3, UIWTDesignRefine::FScope(),
                                                          Imported.Report, Pass, Error)))
  {
    TestEqual(TEXT("node count"), Pass.NodeCount, 4);
    TestEqual(TEXT("source"), Pass.Source, FString(TEXT("figma")));
    TestTrue(TEXT("outline names the widget"), Pass.Request.Contains(TEXT("\"w\":\"Txt_Title\"")));
    TestTrue(TEXT("outline has hints"), Pass.Request.Contains(TEXT("prefix:btn")));
    TestTrue(TEXT("round limit"), Pass.Request.Contains(TEXT("at most 3 rounds")));
  }

  // Re-importing the same design keeps the name.
  UIWTDesignImport::FReimportRequest Reimport;
  Reimport.Blueprint = Blueprint;
  UIWTDesignImport::FReimportPlan Plan;
  if (TestTrue(TEXT("plan"), UIWTDesignImport::PlanReimport(Reimport, Plan, Error)))
  {
    TestEqual(TEXT("no design changes"), Plan.Changes.Num(), 0);
    TestEqual(TEXT("the renamed widget isn't a UE widget"), Plan.UEWidgets, 0);
    UIWTDesignImport::FReimportResult Applied;
    if (TestTrue(TEXT("apply"), UIWTDesignImport::ApplyReimport(Plan, Applied, Error)))
    {
      TestNotNull(TEXT("the new name stays"), Blueprint->WidgetTree->FindWidget(TEXT("Txt_Title")));
      TestNull(TEXT("the old name doesn't come back"),
               Blueprint->WidgetTree->FindWidget(FName(*OldName)));
      UIWTDesignImport::AcceptReimport(Blueprint, false, Error);
    }
  }
  CleanUp();
  return true;
}

// Scopes: the node a widget belongs to, a part's outline, node count,
// report and cropped reference, the root meaning the whole blueprint, and
// a re-import's changed nodes kept in the sidecar until cleared.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTRefineScopeTest, FUIWTRefineTestBase,
                                        "UIWidgetTool.DesignRefine.Scope", TestFlags)

bool FUIWTRefineScopeTest::RunTest(const FString &Parameters)
{
  // A reference at 1x: the import copies it next to the blueprint.
  FImage Reference(640, 360, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
  for (FColor &Pixel : Reference.AsBGRA8())
  {
    Pixel = FColor(16, 24, 32);
  }
  FString Error;
  UIWTRunImages::SavePng(Reference, DesignDir() / TEXT("reference.png"), Error);
  UWidgetBlueprint *Blueprint = ImportDesign(DesignJson);
  if (!Blueprint)
  {
    CleanUp();
    return false;
  }
  UIWTDesignImport::FSidecar Sidecar;
  UIWTDesignImport::ReadSidecar(Blueprint->GetOutermost()->GetName(), Sidecar, Error);
  TestFalse(TEXT("the report is kept in the sidecar"), Sidecar.Report.IsEmpty());
  const FString LabelWidget = Sidecar.Owned.Contains(TEXT("1:4"))
                                  ? Sidecar.Owned[TEXT("1:4")].FindRef(TEXT("main"))
                                  : FString();
  TestEqual(TEXT("a widget's node"),
            UIWTDesignRefine::NodeForWidget(Blueprint, Sidecar.Owned, LabelWidget),
            FString(TEXT("1:4")));
  TestEqual(TEXT("the root widget's node"),
            UIWTDesignRefine::NodeForWidget(Blueprint, Sidecar.Owned,
                                            Blueprint->WidgetTree->RootWidget->GetName()),
            FString(TEXT("1:1")));
  TestTrue(TEXT("no node for an unknown widget"),
           UIWTDesignRefine::NodeForWidget(Blueprint, Sidecar.Owned, TEXT("Nope")).IsEmpty());

  // The plate's part: two nodes, its outline only, the reference cropped
  // to its box (10, 60, 120 x 40) and 8 pixels around.
  UIWTDesignRefine::FScope Plate;
  Plate.Nodes.Add(TEXT("1:3"));
  UIWTDesignRefine::FPass Pass;
  if (TestTrue(TEXT("part"), UIWTDesignRefine::Prepare(Blueprint, 3, Plate, {}, Pass, Error)))
  {
    TestEqual(TEXT("part nodes"), Pass.NodeCount, 2);
    TestEqual(TEXT("one part"), Pass.ScopeWidgets.Num(), 1);
    TestTrue(TEXT("its outline"), Pass.Request.Contains(TEXT("\"id\":\"1:4\"")));
    TestFalse(TEXT("not the rest"), Pass.Request.Contains(TEXT("\"id\":\"1:2\"")));
    TestTrue(TEXT("says it's a part"), Pass.Request.Contains(TEXT("Change only these widgets")));
    TestTrue(TEXT("cropped"), Pass.Crop == FIntRect(2, 52, 138, 108));
    FImage Cropped;
    TestTrue(TEXT("crop written"),
             Pass.AttachImage != Pass.ReferenceImage &&
                 UIWTRunImages::LoadImageFile(Pass.AttachImage, Cropped, Error) &&
                 Cropped.SizeX == 136 && Cropped.SizeY == 56);
  }
  UIWTDesignRefine::FScope Root;
  Root.Nodes.Add(TEXT("1:1"));
  if (TestTrue(TEXT("root"), UIWTDesignRefine::Prepare(Blueprint, 3, Root, {}, Pass, Error)))
  {
    TestTrue(TEXT("the root is the whole blueprint"),
             Pass.ScopeWidgets.IsEmpty() && Pass.NodeCount == 4 && Pass.Crop.Area() == 0 &&
                 Pass.AttachImage == Pass.ReferenceImage);
  }
  UIWTDesignRefine::FScope Gone;
  Gone.Nodes.Add(TEXT("9:9"));
  TestFalse(TEXT("a scope with no widgets is refused"),
            UIWTDesignRefine::Prepare(Blueprint, 3, Gone, {}, Pass, Error));

  // A re-import that changes the label: its node is the changed part.
  const FString Changed = FString(DesignJson).Replace(TEXT("\"OK\""), TEXT("\"Go\""));
  FFileHelper::SaveStringToFile(Changed, *(DesignDir() / TEXT("design.json")));
  UIWTDesignImport::FReimportRequest Reimport;
  Reimport.Blueprint = Blueprint;
  UIWTDesignImport::FReimportPlan Plan;
  UIWTDesignImport::FReimportResult Applied;
  if (TestTrue(TEXT("re-import"), UIWTDesignImport::PlanReimport(Reimport, Plan, Error) &&
                                      UIWTDesignImport::ApplyReimport(Plan, Applied, Error) &&
                                      UIWTDesignImport::AcceptReimport(Blueprint, false, Error)))
  {
    UIWTDesignImport::ReadSidecar(Blueprint->GetOutermost()->GetName(), Sidecar, Error);
    TestEqual(TEXT("changed nodes"), FString::Join(Sidecar.ChangedNodes, TEXT(",")),
              FString(TEXT("1:4")));
    UIWTDesignRefine::FScope ChangedParts;
    ChangedParts.Nodes = Sidecar.ChangedNodes;
    ChangedParts.bChangedParts = true;
    if (TestTrue(TEXT("changed parts"),
                 UIWTDesignRefine::Prepare(Blueprint, 3, ChangedParts, {}, Pass, Error)))
    {
      TestEqual(TEXT("changed part nodes"), Pass.NodeCount, 1);
      TestTrue(TEXT("says they're the re-import's"),
               Pass.Request.Contains(TEXT("the last re-import")));
    }
    TestTrue(TEXT("clear"), UIWTDesignImport::ClearChangedNodes(
                                Blueprint->GetOutermost()->GetName(), Error));
    UIWTDesignImport::ReadSidecar(Blueprint->GetOutermost()->GetName(), Sidecar, Error);
    TestTrue(TEXT("cleared"), Sidecar.ChangedNodes.IsEmpty());
  }
  IFileManager::Get().Delete(*Pass.AttachImage, false, true, true);
  CleanUp();
  return true;
}

// What Claude Code reports a run used, read from its result line.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTRefineUsageTest, FUIWTRefineTestBase,
                                        "UIWidgetTool.DesignRefine.Usage", TestFlags)

bool FUIWTRefineUsageTest::RunTest(const FString &Parameters)
{
  TSharedPtr<FJsonObject> Result;
  FJsonSerializer::Deserialize(
      TJsonReaderFactory<>::Create(TEXT(
          R"json({"type":"result","subtype":"success","is_error":false,"result":"Done.",
            "session_id":"abc","total_cost_usd":0.8125,
            "usage":{"input_tokens":1200,"cache_creation_input_tokens":90000,
                     "cache_read_input_tokens":750000,"output_tokens":36000}})json")),
      Result);
  if (!TestTrue(TEXT("parsed"), Result.IsValid()))
  {
    return false;
  }
  const FUIWTRunUsage Usage = FUIWTRunUsage::FromResult(*Result);
  TestTrue(TEXT("valid"), Usage.bValid);
  TestEqual(TEXT("input"), Usage.InputTokens, int64(1200));
  TestEqual(TEXT("cache creation"), Usage.CacheCreationTokens, int64(90000));
  TestEqual(TEXT("cache read"), Usage.CacheReadTokens, int64(750000));
  TestEqual(TEXT("output"), Usage.OutputTokens, int64(36000));
  TestEqual(TEXT("cost"), Usage.CostUsd, 0.8125);
  TestFalse(TEXT("nothing reported"), FUIWTRunUsage::FromResult(FJsonObject()).bValid);
  return true;
}

// GetDesignNode's node JSON: the node as design.json writes it, children cut
// at the depth and counted.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTRefineNodeTest, FUIWTRefineTestBase,
                                        "UIWidgetTool.DesignRefine.Node", TestFlags)

bool FUIWTRefineNodeTest::RunTest(const FString &Parameters)
{
  FDocument Doc;
  TArray<FString> Errors;
  if (!TestTrue(TEXT("read"), ReadDocument(DesignJson, Doc, Errors)))
  {
    return false;
  }
  const FNode *Plate = UIWTDesignRefine::FindNode(Doc, TEXT("1:3"));
  if (!TestNotNull(TEXT("found"), Plate))
  {
    return false;
  }
  const FString Shallow = UIWTDesignRefine::NodeJson(*Plate, 0);
  TestTrue(TEXT("depth 0 counts the children"), Shallow.Contains(TEXT("\"hiddenChildren\":1")));
  TestFalse(TEXT("depth 0 leaves them out"), Shallow.Contains(TEXT("Label")));
  const FString Deep = UIWTDesignRefine::NodeJson(*Plate, 1);
  TestTrue(TEXT("depth 1 has the child"), Deep.Contains(TEXT("Label")));
  TestTrue(TEXT("full node data"), Deep.Contains(TEXT("#3366FF")));
  TestNull(TEXT("unknown id"), UIWTDesignRefine::FindNode(Doc, TEXT("9:9")));

  const FString Outline = UIWTDesignRefine::MakeOutline(Doc.Root, FOwnedMap());
  TestTrue(TEXT("outline entry"), Outline.Contains(TEXT("\"id\":\"1:4\",\"n\":\"Label\",\"k\":\"text\"")));
  TestTrue(TEXT("outline box"), Outline.Contains(TEXT("\"b\":[8,8,104,24]")));
  return true;
}

#endif
