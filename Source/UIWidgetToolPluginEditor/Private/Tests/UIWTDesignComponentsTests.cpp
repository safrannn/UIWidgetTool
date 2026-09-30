#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Core/UIWTDesignImport.h"
#include "Core/UIWTGeneratedBlueprints.h"
#include "Dom/JsonObject.h"
#include "Figma/UIWTFigmaNormalize.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

// Tests for child WBPs made from components (import-tree.md → Assets →
// Child WBPs; Implementation order, Phase 5). Offline: the reader part runs
// on the hand-written library response in Plugins/UIWidgetToolPlugin/Tests/
// Figma, the import part on design trees with a components map, imported
// into /Temp without saving.
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

  // A screen with two instances of the button component (one resized). The
  // instances show "Old look"; the components entry, when there is one, is
  // the main component with InText.
  FString MakeScreen(const FString &InPrefix, bool bInWithDefinition, const FString &InHash,
                     const FString &InText, const FString &InVersion)
  {
    FString Components;
    if (bInWithDefinition)
    {
      Components = TEXT(R"json("components": {
        "btn": { "name": "Button", "sourceRef": { "fileKey": "lib", "nodeId": "9:1", "version": "{VERSION}" },
          "hash": "{HASH}",
          "root": { "id": "9:1", "name": "Button", "kind": "frame", "box": { "x": 0, "y": 0, "w": 120, "h": 40 },
            "fill": { "color": "#3366FF" },
            "children": [
              { "id": "9:2", "name": "Label", "kind": "text", "box": { "x": 10, "y": 10, "w": 100, "h": 20 },
                "constraints": { "h": "leftRight", "v": "center" },
                "text": { "content": "{TEXT}", "runs": [ { "font": { "family": "Unmapped" }, "size": 14, "color": "#FFFFFF" } ] } }
            ] } } },)json");
    }
    FString Json = TEXT(R"json({
      "version": 1, "source": "figma", "sourceRef": { "fileKey": "screens", "nodeId": "{P}:1" },
      "referenceSize": { "w": 400, "h": 300 },
      {COMPONENTS}
      "root": { "id": "{P}:1", "name": "Screen", "kind": "frame", "box": { "x": 0, "y": 0, "w": 400, "h": 300 },
        "children": [
          { "id": "{P}:2", "name": "Buy", "kind": "instance", "box": { "x": 10, "y": 10, "w": 120, "h": 40 },
            "component": { "key": "btn", "name": "Button" },
            "children": [
              { "id": "I{P}:2;9:2", "name": "Label", "kind": "text", "box": { "x": 10, "y": 10, "w": 100, "h": 20 },
                "text": { "content": "Old look", "runs": [ { "font": { "family": "Unmapped" }, "size": 14, "color": "#FFFFFF" } ] } }
            ] },
          { "id": "{P}:3", "name": "Wide", "kind": "instance", "box": { "x": 10, "y": 60, "w": 200, "h": 40 },
            "component": { "key": "btn", "name": "Button" },
            "children": [
              { "id": "I{P}:3;9:2", "name": "Label", "kind": "text", "box": { "x": 10, "y": 10, "w": 180, "h": 20 },
                "text": { "content": "Old look", "runs": [ { "font": { "family": "Unmapped" }, "size": 14, "color": "#FFFFFF" } ] } }
            ] }
        ] } })json");
    Json.ReplaceInline(TEXT("{COMPONENTS}"), *Components);
    Json.ReplaceInline(TEXT("{P}"), *InPrefix);
    Json.ReplaceInline(TEXT("{HASH}"), *InHash);
    Json.ReplaceInline(TEXT("{TEXT}"), *InText);
    Json.ReplaceInline(TEXT("{VERSION}"), *InVersion);
    return Json;
  }

  // A screen using a card component that holds an icon component.
  const TCHAR *const NestedScreen = TEXT(R"json({
    "version": 1, "source": "figma", "referenceSize": { "w": 400, "h": 300 },
    "components": {
      "card": { "name": "Card", "hash": "c1",
        "root": { "id": "8:1", "name": "Card", "kind": "frame", "box": { "x": 0, "y": 0, "w": 200, "h": 100 },
          "fill": { "color": "#222222" },
          "children": [
            { "id": "8:2", "name": "Icon", "kind": "instance", "box": { "x": 10, "y": 10, "w": 24, "h": 24 },
              "component": { "key": "icon", "name": "Icon" },
              "children": [ { "id": "I8:2;7:2", "name": "Dot", "kind": "shape", "box": { "x": 4, "y": 4, "w": 16, "h": 16 }, "fill": { "color": "#FF0000" } } ] }
          ] } },
      "icon": { "name": "Icon", "hash": "i1",
        "root": { "id": "7:1", "name": "Icon", "kind": "frame", "box": { "x": 0, "y": 0, "w": 24, "h": 24 },
          "children": [ { "id": "7:2", "name": "Dot", "kind": "shape", "box": { "x": 4, "y": 4, "w": 16, "h": 16 }, "fill": { "color": "#FF0000" } } ] } }
    },
    "root": { "id": "e:1", "name": "Screen", "kind": "frame", "box": { "x": 0, "y": 0, "w": 400, "h": 300 },
      "children": [
        { "id": "e:2", "name": "Card", "kind": "instance", "box": { "x": 10, "y": 10, "w": 200, "h": 100 },
          "component": { "key": "card", "name": "Card" },
          "children": [
            { "id": "Ie:2;8:2", "name": "Icon", "kind": "instance", "box": { "x": 10, "y": 10, "w": 24, "h": 24 },
              "component": { "key": "icon", "name": "Icon" } }
          ] }
      ] } })json");

  class FUIWTComponentsTestBase : public FAutomationTestBase
  {
  public:
    FUIWTComponentsTestBase(const FString &InName, const bool bInComplexTask)
        : FAutomationTestBase(InName, bInComplexTask)
    {
    }
    virtual bool SuppressLogErrors() override { return true; }
    virtual bool SuppressLogWarnings() override { return true; }

  protected:
    FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FString Root = TEXT("/Temp/UIWTComponentsTest_") + Id;

    FString DesignDir() const
    {
      return FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() /
                                               TEXT("UIWTComponents") / Id);
    }

    FString ComponentsFolder() const { return Root / TEXT("Components"); }

    FString WriteDesign(const FString &InName, const FString &InJson)
    {
      const FString Path = DesignDir() / InName + TEXT(".design.json");
      if (!FFileHelper::SaveStringToFile(InJson, *Path))
      {
        AddError(TEXT("can't write ") + Path);
      }
      return Path;
    }

    bool Import(const FString &InName, const FString &InJson, UIWTDesignImport::FResult &OutResult)
    {
      UIWTDesignImport::FRequest Request;
      Request.DesignFile = WriteDesign(InName, InJson);
      Request.TargetFolder = Root;
      Request.BlueprintName = TEXT("WBP_") + InName;
      Request.ComponentsFolder = ComponentsFolder();
      Request.bSave = false;
      FString Error;
      if (!UIWTDesignImport::Import(Request, OutResult, Error))
      {
        AddError(FString::Printf(TEXT("importing %s failed: %s"), *InName, *Error));
        return false;
      }
      for (const FString &ApplyError : OutResult.ApplyErrors)
      {
        AddError(TEXT("Apply: ") + ApplyError);
      }
      AddInfo(FString::Printf(TEXT("%s: %s"), *InName, *FString::Join(OutResult.Components, TEXT("; "))));
      return true;
    }

    bool HasLine(const TArray<FString> &InLines, const FString &InStart)
    {
      return InLines.ContainsByPredicate([&](const FString &Line) { return Line.StartsWith(InStart); });
    }

    UWidgetBlueprint *Child(const TCHAR *InName)
    {
      return LoadObject<UWidgetBlueprint>(
          nullptr, *FString::Printf(TEXT("%s/%s/%s.%s"), *ComponentsFolder(), InName, InName, InName));
    }

    FString LabelOf(UWidgetBlueprint *InBlueprint)
    {
      const UTextBlock *Label =
          InBlueprint ? Cast<UTextBlock>(InBlueprint->WidgetTree->FindWidget(TEXT("Label"))) : nullptr;
      return Label ? Label->GetText().ToString() : FString(TEXT("(no Label)"));
    }

    void CleanUp()
    {
      IFileManager::Get().DeleteDirectory(*DesignDir(), false, true);
      const FString OnDisk = UIWTGenerated::GetFolderOnDisk(Root);
      if (!OnDisk.IsEmpty())
      {
        IFileManager::Get().DeleteDirectory(*OnDisk, false, true);
      }
    }
  };
}

// The reader: a main component from a library response, its nested
// instance's key and local id, library renders under render/<file key>/,
// and a hash that changes with the component.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUIWTFigmaComponentReaderTest, "UIWidgetTool.Figma.Components",
                                 TestFlags)

bool FUIWTFigmaComponentReaderTest::RunTest(const FString &Parameters)
{
  const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UIWidgetToolPlugin"));
  const FString Path =
      Plugin.IsValid() ? Plugin->GetBaseDir() / TEXT("Tests/Figma/library.nodes.json") : FString();
  FString Text;
  TSharedPtr<FJsonObject> Response;
  if (!FFileHelper::LoadFileToString(Text, *Path) ||
      !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Response) || !Response.IsValid())
  {
    AddError(TEXT("can't read ") + Path);
    return false;
  }
  UIWTFigmaNormalize::FOptions Options;
  Options.FileKey = TEXT("lib");
  Options.NodeId = TEXT("9:1");
  Options.RenderFileKey = TEXT("lib");
  UIWTFigmaNormalize::FResult Result;
  FNode Root;
  FString Error;
  if (!TestTrue(TEXT("normalizes"), UIWTFigmaNormalize::NormalizeComponent(
                                        Response.ToSharedRef(), Options, Result, Root, Error)))
  {
    AddError(Error);
    return false;
  }
  TestTrue(TEXT("root at the origin"), Root.Box.X == 0.0 && Root.Box.Y == 0.0 &&
                                           Root.Box.W == 120.0 && Root.Box.H == 40.0);
  TestTrue(TEXT("a component is a frame"), Root.Kind == EKind::Frame);
  const FNode *Label = FindNode(Root, TEXT("9:2"));
  TestTrue(TEXT("child placed in the component"),
           Label && Label->Box.X == 10.0 && Label->Box.Y == 10.0);

  TArray<FString> Keys;
  TMap<FString, FString> Names;
  UIWTFigmaNormalize::CollectComponentKeys(Root, Keys, &Names);
  TestEqual(TEXT("nested key"), FString::Join(Keys, TEXT(",")), FString(TEXT("iconkey")));
  TestEqual(TEXT("nested name"), Names.FindRef(TEXT("iconkey")), FString(TEXT("Icon")));
  TestEqual(TEXT("its local id"), Result.ComponentNodeIds.FindRef(TEXT("iconkey")),
            FString(TEXT("9:10")));
  bool bLibraryRender = false;
  for (const UIWTFigmaNormalize::FImageJob &Job : Result.Images)
  {
    bLibraryRender |= Job.FileKey == TEXT("lib") && Job.Image->Path == TEXT("render/lib/I9-3_9-11.png");
  }
  TestTrue(TEXT("library render under render/lib/"), bLibraryRender);

  const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir());
  const FString Hash = UIWTFigmaNormalize::HashComponent(Root, Directory);
  TestEqual(TEXT("hash is stable"), UIWTFigmaNormalize::HashComponent(Root, Directory), Hash);
  FNode Changed = Root;
  Changed.Children[0].Name = TEXT("Caption");
  TestNotEqual(TEXT("hash follows the component"), UIWTFigmaNormalize::HashComponent(Changed, Directory),
               Hash);
  return true;
}

// Child WBPs across screens: one per key built from the main component,
// reused while its hash holds, updated through the merge when it changes,
// kept when the main component can't be read, nested ones first, and
// updated along with a screen re-import.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignComponentsTest, FUIWTComponentsTestBase,
                                        "UIWidgetTool.DesignComponents.Children", TestFlags)

bool FUIWTDesignComponentsTest::RunTest(const FString &Parameters)
{
  // Screen A: the child is created from the main component, not the
  // instances (which show an older look).
  UIWTDesignImport::FResult A;
  if (!Import(TEXT("A"), MakeScreen(TEXT("a"), true, TEXT("h1"), TEXT("Buy"), TEXT("v1")), A))
  {
    CleanUp();
    return false;
  }
  TestTrue(TEXT("created"), HasLine(A.Components, TEXT("btn: created")));
  UWidgetBlueprint *Button = Child(TEXT("WBP_Button"));
  if (!TestNotNull(TEXT("child WBP"), Button))
  {
    CleanUp();
    return false;
  }
  TestEqual(TEXT("built from the main component"), LabelOf(Button), FString(TEXT("Buy")));
  const UWidget *Buy = A.Blueprint->WidgetTree->FindWidget(TEXT("Buy"));
  const UWidget *Wide = A.Blueprint->WidgetTree->FindWidget(TEXT("Wide"));
  TestTrue(TEXT("both instances are the child WBP"),
           Buy && Wide && Buy->GetClass() == Button->GeneratedClass &&
               Wide->GetClass() == Button->GeneratedClass);
  const UCanvasPanelSlot *WideSlot = Wide ? Cast<UCanvasPanelSlot>(Wide->Slot) : nullptr;
  TestTrue(TEXT("the resized instance keeps its own size"),
           WideSlot && FMath::IsNearlyEqual(WideSlot->GetSize().X, 200.0));
  TestEqual(TEXT("indexed with its hash"),
            UIWTDesignImport::ReadComponentIndex(ComponentsFolder()).FindRef(TEXT("btn")).Hash,
            FString(TEXT("h1")));

  // Screen B: same hash, new library version: reused, not converted.
  UIWTDesignImport::FResult B;
  if (Import(TEXT("B"), MakeScreen(TEXT("b"), true, TEXT("h1"), TEXT("Buy"), TEXT("v2")), B))
  {
    TestTrue(TEXT("reused"), HasLine(B.Components, TEXT("btn: reused")));
    TestTrue(TEXT("the same asset"), Child(TEXT("WBP_Button")) == Button);
  }

  // Screen C: the component changed: the child is updated in place.
  UIWTDesignImport::FResult C;
  if (Import(TEXT("C"), MakeScreen(TEXT("c"), true, TEXT("h2"), TEXT("Buy now"), TEXT("v3")), C))
  {
    TestTrue(TEXT("updated"), HasLine(C.Components, TEXT("btn: updated")));
    TestEqual(TEXT("the new look"), LabelOf(Child(TEXT("WBP_Button"))), FString(TEXT("Buy now")));
    TestEqual(TEXT("new hash indexed"),
              UIWTDesignImport::ReadComponentIndex(ComponentsFolder()).FindRef(TEXT("btn")).Hash,
              FString(TEXT("h2")));
  }

  // Screen D: no main component: the existing child is kept, and that's
  // reported.
  UIWTDesignImport::FResult D;
  if (Import(TEXT("D"), MakeScreen(TEXT("d"), false, FString(), FString(), FString()), D))
  {
    TestTrue(TEXT("kept"), HasLine(D.Components, TEXT("btn: reused")));
    TestTrue(TEXT("componentFallback"),
             D.Report.ContainsByPredicate([](const FReportEntry &Entry)
                                          { return Entry.Category == TEXT("componentFallback"); }));
    TestEqual(TEXT("unchanged"), LabelOf(Child(TEXT("WBP_Button"))), FString(TEXT("Buy now")));
  }

  // Nested: the icon inside the card is built first.
  UIWTDesignImport::FResult E;
  if (Import(TEXT("E"), NestedScreen, E))
  {
    const int32 IconLine = E.Components.IndexOfByPredicate(
        [](const FString &Line) { return Line.StartsWith(TEXT("icon: created")); });
    const int32 CardLine = E.Components.IndexOfByPredicate(
        [](const FString &Line) { return Line.StartsWith(TEXT("card: created")); });
    TestTrue(TEXT("the nested component is created before its parent"),
             IconLine != INDEX_NONE && CardLine != INDEX_NONE && IconLine < CardLine);
    UWidgetBlueprint *Card = Child(TEXT("WBP_Card"));
    UWidgetBlueprint *Icon = Child(TEXT("WBP_Icon"));
    bool bUsesIcon = false;
    if (Card && Icon)
    {
      Card->WidgetTree->ForEachWidget([&](const UWidget *InWidget)
                                      { bUsesIcon |= InWidget->GetClass() == Icon->GeneratedClass; });
    }
    TestTrue(TEXT("the card uses the icon's child WBP"), bUsesIcon);
  }

  // Re-importing screen A with a changed component: the child's update is
  // pending with the screen and accepted with it.
  UIWTDesignImport::FReimportRequest Request;
  Request.Blueprint = A.Blueprint;
  Request.DesignFile = WriteDesign(TEXT("A"), MakeScreen(TEXT("a"), true, TEXT("h3"), TEXT("Buy later"), TEXT("v4")));
  UIWTDesignImport::FReimportPlan Plan;
  UIWTDesignImport::FReimportResult Applied;
  FString Error;
  if (TestTrue(TEXT("planned"), UIWTDesignImport::PlanReimport(Request, Plan, Error)))
  {
    TestTrue(TEXT("the plan updates the child"), HasLine(Plan.Components, TEXT("btn: update")));
    if (TestTrue(TEXT("applied"), UIWTDesignImport::ApplyReimport(Plan, Applied, Error)))
    {
      UWidgetBlueprint *Updated = Child(TEXT("WBP_Button"));
      TestEqual(TEXT("child updated"), LabelOf(Updated), FString(TEXT("Buy later")));
      TestTrue(TEXT("pending with the screen"), UIWTDesignImport::HasPendingReimport(Updated));
      TestTrue(TEXT("accepted"), UIWTDesignImport::AcceptReimport(A.Blueprint, false, Error));
      TestFalse(TEXT("nothing pending"), UIWTDesignImport::HasPendingReimport(Updated));
      TestEqual(TEXT("index follows"),
                UIWTDesignImport::ReadComponentIndex(ComponentsFolder()).FindRef(TEXT("btn")).Hash,
                FString(TEXT("h3")));
    }
  }
  if (!Error.IsEmpty())
  {
    AddError(Error);
  }
  CleanUp();
  return true;
}

#endif
