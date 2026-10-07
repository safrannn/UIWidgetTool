#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Algo/Reverse.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Core/UIWTDesignImport.h"
#include "Core/UIWTDesignTree.h"
#include "Core/UIWTWidgetSpec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

// Tests for the design tree and its converter (import-tree.md → Build and
// test). The fixtures are hand-written design trees in
// Plugins/UIWidgetToolPlugin/Tests/DesignTree. Run headless with
//   UnrealEditor-Cmd.exe <project> -ExecCmds="Automation RunTests UIWidgetTool.DesignTree; Quit" -unattended -nullrhi
namespace
{
  using namespace UIWTDesignTree;

  const TCHAR *const Fixtures[] = {TEXT("screen"), TEXT("layout"), TEXT("components")};

  FString FixtureDir()
  {
    const TSharedPtr<IPlugin> Plugin =
        IPluginManager::Get().FindPlugin(TEXT("UIWidgetToolPlugin"));
    return Plugin.IsValid() ? Plugin->GetBaseDir() / TEXT("Tests/DesignTree") : FString();
  }

  // A resolver that maps every font to the engine's Roboto, except the
  // family "Unmapped".
  FConvertOptions TestOptions()
  {
    FConvertOptions Options;
    Options.BlueprintName = TEXT("WBP_Fixture");
    Options.TargetFolder = TEXT("/Game/UI");
    Options.ComponentsFolder = TEXT("/Game/UI/Components");
    Options.ResolveFont = [](const FFontRef &InFont) -> TOptional<FResolvedFont>
    {
      if (InFont.Family == TEXT("Unmapped"))
      {
        return {};
      }
      return FResolvedFont{TEXT("/Engine/EngineFonts/Roboto.Roboto"),
                           InFont.Style == TEXT("Bold") ? FName(TEXT("Bold"))
                                                        : FName(TEXT("Regular"))};
    };
    Options.NaturalLineHeight = [](const TOptional<FResolvedFont> &, double InSizePx)
    { return InSizePx * 1.2; };
    return Options;
  }

  FString ToJson(const TSharedRef<FJsonObject> &InObject)
  {
    FString Json;
    TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);
    FJsonSerializer::Serialize(InObject, Writer);
    return Json;
  }

  const TCHAR *ActionName(EComponentAction InAction)
  {
    return InAction == EComponentAction::Create   ? TEXT("create")
           : InAction == EComponentAction::Update ? TEXT("update")
                                                  : TEXT("reuse");
  }

  // Everything a conversion produces, as one JSON document for golden files.
  TSharedRef<FJsonObject> ResultJson(const FConvertResult &InResult)
  {
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    if (InResult.Spec.IsValid())
    {
      Object->SetObjectField(TEXT("spec"), InResult.Spec);
    }
    TSharedRef<FJsonObject> Owned = MakeShared<FJsonObject>();
    TArray<FString> Ids;
    InResult.Owned.GetKeys(Ids);
    Ids.Sort();
    for (const FString &Id : Ids)
    {
      TSharedRef<FJsonObject> Roles = MakeShared<FJsonObject>();
      TArray<FString> RoleKeys;
      InResult.Owned[Id].GetKeys(RoleKeys);
      RoleKeys.Sort();
      for (const FString &Role : RoleKeys)
      {
        Roles->SetStringField(Role, InResult.Owned[Id][Role]);
      }
      Owned->SetObjectField(Id, Roles);
    }
    Object->SetObjectField(TEXT("owned"), Owned);
    TArray<TSharedPtr<FJsonValue>> Assets;
    for (const FTextureAsset &Asset : InResult.Assets)
    {
      TSharedRef<FJsonObject> AssetObject = MakeShared<FJsonObject>();
      AssetObject->SetStringField(TEXT("source"), Asset.SourcePath);
      AssetObject->SetStringField(TEXT("object"), Asset.ObjectPath);
      AssetObject->SetBoolField(TEXT("existing"), Asset.bExisting);
      Assets.Add(MakeShared<FJsonValueObject>(AssetObject));
    }
    Object->SetArrayField(TEXT("assets"), Assets);
    TArray<TSharedPtr<FJsonValue>> Components;
    for (const FComponentJob &Job : InResult.Components)
    {
      TSharedRef<FJsonObject> JobObject = MakeShared<FJsonObject>();
      JobObject->SetStringField(TEXT("key"), Job.Key);
      JobObject->SetStringField(TEXT("assetPath"), Job.AssetPath);
      JobObject->SetStringField(TEXT("action"), ActionName(Job.Action));
      JobObject->SetStringField(TEXT("hash"), Job.Hash);
      JobObject->SetStringField(TEXT("root"), Job.Tree.IsValid() ? Job.Tree->Root.Id : FString());
      Components.Add(MakeShared<FJsonValueObject>(JobObject));
    }
    Object->SetArrayField(TEXT("components"), Components);
    TArray<TSharedPtr<FJsonValue>> Report;
    for (const FReportEntry &Entry : InResult.Report)
    {
      TSharedRef<FJsonObject> EntryObject = MakeShared<FJsonObject>();
      EntryObject->SetStringField(TEXT("node"), Entry.Node);
      EntryObject->SetStringField(TEXT("category"), Entry.Category);
      EntryObject->SetStringField(TEXT("detail"), Entry.Detail);
      Report.Add(MakeShared<FJsonValueObject>(EntryObject));
    }
    Object->SetArrayField(TEXT("report"), Report);
    Object->SetNumberField(TEXT("widgetCount"), InResult.WidgetCount);
    Object->SetNumberField(TEXT("maxDepth"), InResult.MaxDepth);
    return Object;
  }

  // The spec node named InName, anywhere under InNode.
  TSharedPtr<FJsonObject> FindWidget(const TSharedPtr<FJsonObject> &InNode,
                                     const FString &InName)
  {
    if (!InNode.IsValid())
    {
      return nullptr;
    }
    FString Name;
    if (InNode->TryGetStringField(TEXT("name"), Name) && Name == InName)
    {
      return InNode;
    }
    const TArray<TSharedPtr<FJsonValue>> *Children = nullptr;
    if (InNode->TryGetArrayField(TEXT("children"), Children))
    {
      for (const TSharedPtr<FJsonValue> &Child : *Children)
      {
        if (TSharedPtr<FJsonObject> Found = FindWidget(Child->AsObject(), InName))
        {
          return Found;
        }
      }
    }
    return nullptr;
  }

  // A number or string at a dotted path, e.g. "slot.LayoutData.Offsets.Left".
  TSharedPtr<FJsonValue> At(const TSharedPtr<FJsonObject> &InObject, const FString &InPath)
  {
    TArray<FString> Parts;
    InPath.ParseIntoArray(Parts, TEXT("."));
    TSharedPtr<FJsonObject> Current = InObject;
    for (int32 Index = 0; Current.IsValid() && Index < Parts.Num(); ++Index)
    {
      const TSharedPtr<FJsonValue> Field = Current->TryGetField(Parts[Index]);
      if (!Field.IsValid())
      {
        return nullptr;
      }
      if (Index == Parts.Num() - 1)
      {
        return Field;
      }
      const TSharedPtr<FJsonObject> *Next = nullptr;
      Current = Field->TryGetObject(Next) ? *Next : nullptr;
    }
    return nullptr;
  }

  bool HasReport(const FConvertResult &InResult, const FString &InNode,
                 const FString &InCategory)
  {
    return InResult.Report.ContainsByPredicate(
        [&](const FReportEntry &Entry)
        { return Entry.Node == InNode && Entry.Category == InCategory; });
  }

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

  FNode MakeShape(const FString &InId, const FString &InName, double InX, double InY,
                  double InW, double InH)
  {
    FNode Node;
    Node.Id = InId;
    Node.Name = InName;
    Node.Kind = EKind::Shape;
    Node.Box = {InX, InY, InW, InH};
    TSharedRef<FFill> Fill = MakeShared<FFill>();
    Fill->Color = FColor(200, 100, 50);
    Node.Fill = Fill;
    return Node;
  }

  FDocument MakeCanvasDocument(double InW, double InH)
  {
    FDocument Doc;
    Doc.ReferenceSize = FVector2D(InW, InH);
    Doc.Root.Id = TEXT("root");
    Doc.Root.Name = TEXT("Root");
    Doc.Root.Kind = EKind::Frame;
    Doc.Root.Box = {0.0, 0.0, InW, InH};
    return Doc;
  }

  // Compiling half-built test blueprints logs compiler messages; the tests
  // check their results themselves.
  class FUIWTDesignTestBase : public FAutomationTestBase
  {
  public:
    FUIWTDesignTestBase(const FString &InName, const bool bInComplexTask)
        : FAutomationTestBase(InName, bInComplexTask)
    {
    }
    virtual bool SuppressLogErrors() override { return true; }
    virtual bool SuppressLogWarnings() override { return true; }

  protected:
    bool LoadFixture(const FString &InName, FDocument &OutDoc)
    {
      const FString Path = FixtureDir() / InName + TEXT(".design.json");
      FString Json;
      if (!FFileHelper::LoadFileToString(Json, *Path))
      {
        AddError(FString::Printf(TEXT("can't read %s"), *Path));
        return false;
      }
      TArray<FString> Errors;
      const bool bRead = ReadDocument(Json, OutDoc, Errors);
      for (const FString &Error : Errors)
      {
        AddError(FString::Printf(TEXT("%s: %s"), *InName, *Error));
      }
      return bRead;
    }

    // Names are unique ignoring case, and none is reserved.
    void CheckNames(const FConvertResult &InResult, const TSet<FString> &InReserved)
    {
      TSet<FString> Seen;
      for (const TPair<FString, FRoleNames> &Node : InResult.Owned)
      {
        for (const TPair<FString, FString> &Role : Node.Value)
        {
          bool bAlreadySeen = false;
          Seen.Add(Role.Value.ToLower(), &bAlreadySeen);
          TestFalse(FString::Printf(TEXT("'%s' is unique"), *Role.Value), bAlreadySeen);
          TestFalse(FString::Printf(TEXT("'%s' is not reserved"), *Role.Value),
                    InReserved.Contains(Role.Value));
        }
      }
    }

    double Number(const TSharedPtr<FJsonObject> &InObject, const FString &InPath)
    {
      const TSharedPtr<FJsonValue> Value = At(InObject, InPath);
      double Result = 0.0;
      if (!Value.IsValid() || !Value->TryGetNumber(Result))
      {
        AddError(FString::Printf(TEXT("no number at %s"), *InPath));
      }
      return Result;
    }

    FString String(const TSharedPtr<FJsonObject> &InObject, const FString &InPath)
    {
      const TSharedPtr<FJsonValue> Value = At(InObject, InPath);
      FString Result;
      if (!Value.IsValid() || !Value->TryGetString(Result))
      {
        AddError(FString::Printf(TEXT("no string at %s"), *InPath));
      }
      return Result;
    }

    TSharedPtr<FJsonObject> Widget(const FConvertResult &InResult, const FString &InId,
                                   const FString &InRole = TEXT("main"))
    {
      const FRoleNames *Roles = InResult.Owned.Find(InId);
      const FString *Name = Roles ? Roles->Find(InRole) : nullptr;
      TSharedPtr<FJsonObject> Found =
          Name && InResult.Spec.IsValid()
              ? FindWidget(InResult.Spec->GetObjectField(TEXT("root")), *Name)
              : nullptr;
      if (!Found.IsValid())
      {
        AddError(FString::Printf(TEXT("no %s widget for node %s"), *InRole, *InId));
      }
      return Found;
    }
  };

  constexpr EAutomationTestFlags TestFlags =
      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
}

// Reading and writing a fixture again gives the same document.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignReadWriteTest, FUIWTDesignTestBase,
                                        "UIWidgetTool.DesignTree.ReadWrite", TestFlags)

bool FUIWTDesignReadWriteTest::RunTest(const FString &Parameters)
{
  for (const TCHAR *Fixture : Fixtures)
  {
    FDocument Doc;
    if (!LoadFixture(Fixture, Doc))
    {
      continue;
    }
    const FString First = WriteDocumentString(Doc);
    FDocument Again;
    TArray<FString> Errors;
    TestTrue(FString::Printf(TEXT("%s reads back"), Fixture),
             ReadDocument(First, Again, Errors));
    TestEqual(FString::Printf(TEXT("%s writes the same"), Fixture),
              WriteDocumentString(Again), First);
  }

  FDocument Bad;
  TArray<FString> Errors;
  TestFalse(TEXT("a broken document is refused"),
            ReadDocument(TEXT(R"json({"version":1,"referenceSize":{"w":10,"h":10},
              "root":{"id":"a","kind":"frame","box":{"x":0,"y":0,"w":10,"h":10},
              "children":[{"id":"a","kind":"blob","box":{"x":0,"y":0,"w":1,"h":1}}]}})json"),
                         Bad, Errors));
  TestTrue(TEXT("the unknown kind is named"),
           Errors.ContainsByPredicate([](const FString &Error)
                                      { return Error.Contains(TEXT("blob")); }));
  TestTrue(TEXT("the duplicate id is named"),
           Errors.ContainsByPredicate([](const FString &Error)
                                      { return Error.Contains(TEXT("more than one node")); }));
  return true;
}

// Each fixture's whole result against <fixture>.expected.json. The actual
// result is always written to Saved/UIWidgetTool/Golden for review; copy it
// next to the fixture once it's right.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignGoldenTest, FUIWTDesignTestBase,
                                        "UIWidgetTool.DesignTree.Golden", TestFlags)

bool FUIWTDesignGoldenTest::RunTest(const FString &Parameters)
{
  for (const TCHAR *Fixture : Fixtures)
  {
    FDocument Doc;
    if (!LoadFixture(Fixture, Doc))
    {
      continue;
    }
    const FString Actual = ToJson(ResultJson(Convert(Doc, TestOptions())));
    const FString Saved = FPaths::ProjectSavedDir() / TEXT("UIWidgetTool/Golden") /
                          FString(Fixture) + TEXT(".expected.json");
    FFileHelper::SaveStringToFile(Actual, *Saved);
    FString Expected;
    if (!FFileHelper::LoadFileToString(
            Expected, *(FixtureDir() / FString(Fixture) + TEXT(".expected.json"))))
    {
      AddWarning(FString::Printf(TEXT("%s has no expected result yet; review %s and "
                                      "copy it next to the fixture"),
                                 Fixture, *Saved));
      continue;
    }
    Expected.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
    TestEqual(FString::Printf(TEXT("%s matches its expected result (actual: %s)"),
                              Fixture, *Saved),
              Actual.Replace(TEXT("\r\n"), TEXT("\n")), Expected);
  }
  return true;
}

// The screen fixture: canvas constraints, groups, strokes, text, images,
// names and the report.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignScreenTest, FUIWTDesignTestBase,
                                        "UIWidgetTool.DesignTree.Screen", TestFlags)

bool FUIWTDesignScreenTest::RunTest(const FString &Parameters)
{
  FDocument Doc;
  if (!LoadFixture(TEXT("screen"), Doc))
  {
    return false;
  }
  const FConvertResult Result = Convert(Doc, TestOptions());
  if (!TestTrue(TEXT("a spec"), Result.Spec.IsValid()))
  {
    return false;
  }
  CheckNames(Result, {});

  // Screen root: the fill is a background Border around the canvas.
  const TSharedPtr<FJsonObject> Root = Result.Spec->GetObjectField(TEXT("root"));
  TestEqual(TEXT("root main name"), Result.Owned[TEXT("1:1")][TEXT("main")], FString(TEXT("Shop_Screen")));
  TestEqual(TEXT("root is the background"), String(Root, TEXT("name")), FString(TEXT("Shop_Screen_Bg")));
  TestEqual(TEXT("Border padding is written, as 0"), Number(Root, TEXT("props.Padding.Left")), 0.0);

  // A centered 2px stroke grows the shape by 1 on each side.
  const TSharedPtr<FJsonObject> Card = Widget(Result, TEXT("1:2"));
  TestEqual(TEXT("Card left"), Number(Card, TEXT("slot.LayoutData.Offsets.Left")), 99.0);
  TestEqual(TEXT("Card top"), Number(Card, TEXT("slot.LayoutData.Offsets.Top")), 79.0);
  TestEqual(TEXT("Card width"), Number(Card, TEXT("slot.LayoutData.Offsets.Right")), 402.0);
  TestEqual(TEXT("Card height"), Number(Card, TEXT("slot.LayoutData.Offsets.Bottom")), 242.0);
  TestEqual(TEXT("Card brush"), String(Card, TEXT("props.Background.DrawAs")), FString(TEXT("RoundedBox")));
  TestEqual(TEXT("Card outline"), Number(Card, TEXT("props.Background.OutlineSettings.Width")), 2.0);

  // Two "Title" nodes: the lowest id (1:13 < 1:3 as strings) keeps the name.
  TestEqual(TEXT("1:13 keeps Title"), Result.Owned[TEXT("1:13")][TEXT("main")], FString(TEXT("Title")));
  TestTrue(TEXT("1:3 gets a suffix"), Result.Owned[TEXT("1:3")][TEXT("main")].StartsWith(TEXT("Title_")));

  // Wrapping text in a canvas: AutoSize, a width override, wrap on.
  const TSharedPtr<FJsonObject> DescriptionSize = Widget(Result, TEXT("1:4"), TEXT("size"));
  TestEqual(TEXT("wrap width"), Number(DescriptionSize, TEXT("props.WidthOverride")), 360.0);
  TestFalse(TEXT("no height override"), At(DescriptionSize, TEXT("props.HeightOverride")).IsValid());
  TestTrue(TEXT("AutoSize"), At(DescriptionSize, TEXT("slot.bAutoSize")).IsValid());
  const TSharedPtr<FJsonObject> Description = Widget(Result, TEXT("1:4"));
  TestEqual(TEXT("line height 150% of 16px over 19.2px"), Number(Description, TEXT("props.LineHeightPercentage")), 1.25);
  TestEqual(TEXT("font size in points"), Number(Description, TEXT("props.Font.Size")), 12.0);
  TestEqual(TEXT("tracking"), Number(Description, TEXT("props.Font.LetterSpacing")), 20.0);

  // Constraints.
  const TSharedPtr<FJsonObject> Close = Widget(Result, TEXT("1:5"));
  TestEqual(TEXT("right anchor"), Number(Close, TEXT("slot.LayoutData.Anchors.Minimum.X")), 1.0);
  TestEqual(TEXT("right offset"), Number(Close, TEXT("slot.LayoutData.Offsets.Left")), -80.0);
  const TSharedPtr<FJsonObject> Footer = Widget(Result, TEXT("1:6"));
  TestEqual(TEXT("stretch min"), Number(Footer, TEXT("slot.LayoutData.Anchors.Minimum.X")), 0.0);
  TestEqual(TEXT("stretch max"), Number(Footer, TEXT("slot.LayoutData.Anchors.Maximum.X")), 1.0);
  TestEqual(TEXT("stretch right margin"), Number(Footer, TEXT("slot.LayoutData.Offsets.Right")), 0.0);
  TestEqual(TEXT("bottom offset"), Number(Footer, TEXT("slot.LayoutData.Offsets.Top")), -80.0);
  const TSharedPtr<FJsonObject> Badge = Widget(Result, TEXT("1:7"));
  TestEqual(TEXT("center x"), Number(Badge, TEXT("slot.LayoutData.Offsets.Left")), -60.0);
  TestEqual(TEXT("center y"), Number(Badge, TEXT("slot.LayoutData.Offsets.Top")), -40.0);
  TestEqual(TEXT("rotation"), Number(Badge, TEXT("props.RenderTransform.Angle")), 15.0);
  const TSharedPtr<FJsonObject> Hero = Widget(Result, TEXT("1:8"), TEXT("fit"));
  TestEqual(TEXT("fit is a ScaleBox"), String(Hero, TEXT("class")), FString(TEXT("ScaleBox")));
  TestEqual(TEXT("scale min"), Number(Hero, TEXT("slot.LayoutData.Anchors.Minimum.X")), 0.5);
  TestEqual(TEXT("scale max"), Number(Hero, TEXT("slot.LayoutData.Anchors.Maximum.X")), 0.75);

  // A group with opacity covers the root; its children keep root coordinates.
  const TSharedPtr<FJsonObject> Faded = Widget(Result, TEXT("1:9"));
  TestEqual(TEXT("cover anchors"), Number(Faded, TEXT("slot.LayoutData.Anchors.Maximum.Y")), 1.0);
  TestEqual(TEXT("cover offsets"), Number(Faded, TEXT("slot.LayoutData.Offsets.Right")), 0.0);
  TestEqual(TEXT("group opacity"), Number(Faded, TEXT("props.RenderOpacity")), 0.5);
  TestEqual(TEXT("child constraint against the root"),
            Number(Widget(Result, TEXT("1:10")), TEXT("slot.LayoutData.Offsets.Left")), -1820.0);
  TestEqual(TEXT("hidden"), String(Widget(Result, TEXT("1:11")), TEXT("props.Visibility")),
            FString(TEXT("Collapsed")));

  // A plain group is kept, so the hierarchy matches the design: a canvas
  // covering the root, its child in root coordinates.
  const TSharedPtr<FJsonObject> Plain = Widget(Result, TEXT("1:12"));
  TestEqual(TEXT("plain group is a canvas"), String(Plain, TEXT("class")), FString(TEXT("CanvasPanel")));
  TestEqual(TEXT("plain group covers"), Number(Plain, TEXT("slot.LayoutData.Anchors.Maximum.X")), 1.0);
  const TSharedPtr<FJsonObject> SecondTitle = Widget(Result, TEXT("1:13"));
  TestEqual(TEXT("child x"), Number(SecondTitle, TEXT("slot.LayoutData.Offsets.Left")), 1510.0);
  TestEqual(TEXT("child y"), Number(SecondTitle, TEXT("slot.LayoutData.Offsets.Top")), 610.0);

  // Images.
  const TSharedPtr<FJsonObject> Pattern = Widget(Result, TEXT("1:15"));
  TestEqual(TEXT("tiled"), String(Pattern, TEXT("props.Brush.Tiling")), FString(TEXT("Both")));
  TestEqual(TEXT("tile size at 1x"), Number(Pattern, TEXT("props.Brush.ImageSize.X")), 32.0);
  const TSharedPtr<FJsonObject> Avatar = Widget(Result, TEXT("1:16"));
  TestEqual(TEXT("rounded image"), String(Avatar, TEXT("props.Brush.DrawAs")), FString(TEXT("RoundedBox")));
  TestEqual(TEXT("texture named after the node"),
            String(Avatar, TEXT("props.Brush.ResourceObject")),
            FString(TEXT("/Game/UI/WBP_Fixture/Textures/T_WBP_Fixture_Avatar.T_WBP_Fixture_Avatar")));
  TestEqual(TEXT("four textures"), Result.Assets.Num(), 4);

  // Report.
  TestTrue(TEXT("reader note passed through"), HasReport(Result, TEXT("1:9"), TEXT("gradientAveraged")));
  TestTrue(TEXT("blend mode"), HasReport(Result, TEXT("1:14"), TEXT("blendMode")));
  TestTrue(TEXT("unmapped font"), HasReport(Result, TEXT("1:13"), TEXT("fontUnmapped")));
  // Not UMG's default Bold: the run has no style, so Regular.
  TestEqual(TEXT("unmapped font keeps its weight"),
            String(SecondTitle, TEXT("props.Font.TypefaceFontName")), FString(TEXT("Regular")));
  return true;
}

// An unmapped font's style picks the default font's typeface.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignDefaultTypefaceTest, FUIWTDesignTestBase,
                                        "UIWidgetTool.DesignTree.DefaultTypeface", TestFlags)

bool FUIWTDesignDefaultTypefaceTest::RunTest(const FString &Parameters)
{
  auto Typeface = [](const TCHAR *InStyle, const TCHAR *InPostScript = TEXT(""))
  {
    FFontRef Font;
    Font.Family = TEXT("Unmapped");
    Font.Style = InStyle;
    Font.PostScript = InPostScript;
    return DefaultTypeface(Font).ToString();
  };
  TestEqual(TEXT("no style"), Typeface(TEXT("")), FString(TEXT("Regular")));
  TestEqual(TEXT("Regular"), Typeface(TEXT("Regular")), FString(TEXT("Regular")));
  TestEqual(TEXT("Medium"), Typeface(TEXT("Medium")), FString(TEXT("Regular")));
  TestEqual(TEXT("Bold"), Typeface(TEXT("Bold")), FString(TEXT("Bold")));
  TestEqual(TEXT("SemiBold"), Typeface(TEXT("SemiBold")), FString(TEXT("Bold")));
  TestEqual(TEXT("Black"), Typeface(TEXT("Black")), FString(TEXT("Bold")));
  TestEqual(TEXT("Italic"), Typeface(TEXT("Italic")), FString(TEXT("Italic")));
  TestEqual(TEXT("Bold Italic"), Typeface(TEXT("Bold Italic")), FString(TEXT("Bold Italic")));
  TestEqual(TEXT("Light"), Typeface(TEXT("Light")), FString(TEXT("Light")));
  TestEqual(TEXT("ExtraLight"), Typeface(TEXT("ExtraLight")), FString(TEXT("Light")));
  TestEqual(TEXT("PostScript only"), Typeface(TEXT(""), TEXT("Inter-SemiBoldItalic")),
            FString(TEXT("Bold Italic")));
  return true;
}

// The layout fixture: auto layout, wrapping, absolute children, strokes with
// clipping, sizing.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignLayoutTest, FUIWTDesignTestBase,
                                        "UIWidgetTool.DesignTree.Layout", TestFlags)

bool FUIWTDesignLayoutTest::RunTest(const FString &Parameters)
{
  FDocument Doc;
  if (!LoadFixture(TEXT("layout"), Doc))
  {
    return false;
  }
  const FConvertResult Result = Convert(Doc, TestOptions());
  if (!TestTrue(TEXT("a spec"), Result.Spec.IsValid()))
  {
    return false;
  }
  CheckNames(Result, {});

  // Widget root, 2px outside stroke, clipping: size > bg > clip > main.
  const FRoleNames &Panel = Result.Owned[TEXT("2:1")];
  TestTrue(TEXT("root roles"), Panel.Contains(TEXT("size")) && Panel.Contains(TEXT("bg")) &&
                                   Panel.Contains(TEXT("clip")));
  const TSharedPtr<FJsonObject> Size = Widget(Result, TEXT("2:1"), TEXT("size"));
  TestEqual(TEXT("root width with the stroke"), Number(Size, TEXT("props.WidthOverride")), 364.0);
  TestEqual(TEXT("root height with the stroke"), Number(Size, TEXT("props.HeightOverride")), 644.0);
  const TSharedPtr<FJsonObject> Bg = Widget(Result, TEXT("2:1"), TEXT("bg"));
  TestEqual(TEXT("bg padding is the growth"), Number(Bg, TEXT("props.Padding.Left")), 2.0);
  const TSharedPtr<FJsonObject> Clip = Widget(Result, TEXT("2:1"), TEXT("clip"));
  TestEqual(TEXT("clip holds the layout padding"), Number(Clip, TEXT("props.Padding.Left")), 16.0);
  TestEqual(TEXT("clip clips"), String(Clip, TEXT("props.Clipping")), FString(TEXT("ClipToBounds")));
  TestEqual(TEXT("main"), String(Widget(Result, TEXT("2:1")), TEXT("class")), FString(TEXT("VerticalBox")));

  // Header: space-between with two children → one Spacer; fills the width.
  TestTrue(TEXT("header spacer"), Result.Owned[TEXT("2:2")].Contains(TEXT("spacer1")));
  const TSharedPtr<FJsonObject> Header = Widget(Result, TEXT("2:2"), TEXT("size"));
  TestEqual(TEXT("header stretches across"), String(Header, TEXT("slot.HorizontalAlignment")),
            FString(TEXT("HAlign_Fill")));
  TestEqual(TEXT("header height"), Number(Header, TEXT("props.HeightOverride")), 48.0);
  TestEqual(TEXT("first child has no spacing"), Number(Header, TEXT("slot.Padding.Top")), 0.0);

  // Row: absolute child → Overlay outside the background; centered → 2 Spacers.
  const FRoleNames &Row = Result.Owned[TEXT("2:5")];
  TestTrue(TEXT("row layers and abs"), Row.Contains(TEXT("layers")) && Row.Contains(TEXT("abs")));
  TestTrue(TEXT("row spacers"), Row.Contains(TEXT("spacer2")));
  const TSharedPtr<FJsonObject> Layers = Widget(Result, TEXT("2:5"), TEXT("layers"));
  const TArray<TSharedPtr<FJsonValue>> *LayerChildren = nullptr;
  const TSharedPtr<FJsonObject> *FirstLayer = nullptr;
  if (Layers.IsValid() && Layers->TryGetArrayField(TEXT("children"), LayerChildren) &&
      !LayerChildren->IsEmpty() && (*LayerChildren)[0]->TryGetObject(FirstLayer))
  {
    TestEqual(TEXT("the Overlay holds the background"), String(*FirstLayer, TEXT("name")),
              Row[TEXT("bg")]);
  }
  else
  {
    AddError(TEXT("the row's Overlay has no children"));
  }
  TestEqual(TEXT("spacing before the row"),
            Number(Widget(Result, TEXT("2:5"), TEXT("size")), TEXT("slot.Padding.Top")), 12.0);
  const TSharedPtr<FJsonObject> BadgeWidget = Widget(Result, TEXT("2:8"));
  TestEqual(TEXT("absolute child right-anchored to the frame"),
            Number(BadgeWidget, TEXT("slot.LayoutData.Offsets.Left")), -28.0);
  TestEqual(TEXT("label fills the cross axis"),
            String(Widget(Result, TEXT("2:7")), TEXT("slot.VerticalAlignment")),
            FString(TEXT("VAlign_Fill")));

  // Wrapping.
  const TSharedPtr<FJsonObject> Tags = Widget(Result, TEXT("2:9"));
  TestEqual(TEXT("wrap"), String(Tags, TEXT("class")), FString(TEXT("WrapBox")));
  TestEqual(TEXT("wrap gaps"), Number(Tags, TEXT("props.InnerSlotPadding.Y")), 4.0);
  TestTrue(TEXT("space-between in a wrap reported"), HasReport(Result, TEXT("2:9"), TEXT("alignApprox")));
  const TSharedPtr<FJsonObject> Column = Widget(Result, TEXT("2:13"));
  TestEqual(TEXT("vertical wrap"), String(Column, TEXT("props.Orientation")), FString(TEXT("Orient_Vertical")));
  TestEqual(TEXT("vertical wrap gaps swap"), Number(Column, TEXT("props.InnerSlotPadding.X")), 10.0);
  TestTrue(TEXT("vertical wrap alignment reported"), HasReport(Result, TEXT("2:13"), TEXT("alignApprox")));
  int32 PlainTags = 0;
  for (const TCHAR *Id : {TEXT("2:10"), TEXT("2:11"), TEXT("2:12")})
  {
    PlainTags += Result.Owned[Id][TEXT("main")] == TEXT("Tag") ? 1 : 0;
  }
  TestEqual(TEXT("one Tag keeps the plain name"), PlainTags, 1);

  // Sizing.
  TestEqual(TEXT("fill"), String(Widget(Result, TEXT("2:16")), TEXT("slot.Size.SizeRule")),
            FString(TEXT("Fill")));
  const TSharedPtr<FJsonObject> FooterSize = Widget(Result, TEXT("2:17"), TEXT("size"));
  TestFalse(TEXT("a filled width has no override"), At(FooterSize, TEXT("props.WidthOverride")).IsValid());
  TestEqual(TEXT("footer height"), Number(FooterSize, TEXT("props.HeightOverride")), 96.0);
  TestEqual(TEXT("min height"), Number(FooterSize, TEXT("props.MinDesiredHeight")), 40.0);
  const TSharedPtr<FJsonObject> Footer = Widget(Result, TEXT("2:17"));
  TestEqual(TEXT("valign in the SizeBox"), String(Footer, TEXT("slot.VerticalAlignment")),
            FString(TEXT("VAlign_Bottom")));
  TestEqual(TEXT("centered text"), String(Footer, TEXT("props.Justification")), FString(TEXT("Center")));
  return true;
}

// The components fixture: child WBPs from main components, the fallback, and
// the index deciding create / update / reuse.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignTreeComponentsTest, FUIWTDesignTestBase,
                                        "UIWidgetTool.DesignTree.Components", TestFlags)

bool FUIWTDesignTreeComponentsTest::RunTest(const FString &Parameters)
{
  FDocument Doc;
  if (!LoadFixture(TEXT("components"), Doc))
  {
    return false;
  }
  FConvertOptions Options = TestOptions();
  FConvertResult Result = Convert(Doc, Options);
  if (!TestEqual(TEXT("two child WBPs"), Result.Components.Num(), 2))
  {
    return false;
  }
  const FComponentJob &Button = Result.Components[0];
  TestEqual(TEXT("sorted by key"), Button.Key, FString(TEXT("btn-key")));
  TestEqual(TEXT("created"), (int32)Button.Action, (int32)EComponentAction::Create);
  TestEqual(TEXT("path"), Button.AssetPath, FString(TEXT("/Game/UI/Components/WBP_Button/WBP_Button")));
  TestEqual(TEXT("hash"), Button.Hash, FString(TEXT("h1")));
  TestTrue(TEXT("built from the main component"), Button.Tree.IsValid() && Button.Tree->Root.Id == TEXT("9:1"));
  const FComponentJob &Lost = Result.Components[1];
  TestEqual(TEXT("fallback path"), Lost.AssetPath,
            FString(TEXT("/Game/UI/Components/WBP_Lost_thing/WBP_Lost_thing")));
  TestTrue(TEXT("fallback built from the instance"),
           Lost.Tree.IsValid() && Lost.Tree->Root.Id == TEXT("3:5") && Lost.Tree->Root.Kind == EKind::Frame);
  TestTrue(TEXT("fallback reported"), HasReport(Result, TEXT("3:5"), TEXT("componentFallback")));

  TestEqual(TEXT("instance is the child WBP"), String(Widget(Result, TEXT("3:2")), TEXT("class")), Button.AssetPath);
  TestEqual(TEXT("a resized instance keeps its own size"),
            Number(Widget(Result, TEXT("3:3")), TEXT("slot.LayoutData.Offsets.Right")), 300.0);
  TestEqual(TEXT("an instance with overrides is expanded"),
            String(Widget(Result, TEXT("3:4")), TEXT("class")), FString(TEXT("HorizontalBox")));
  TestFalse(TEXT("the instance's children aren't converted in the screen"),
            Result.Owned.Contains(TEXT("I3:2;9:2")));

  // The child's own conversion: no size role on its root.
  FConvertOptions ChildOptions = TestOptions();
  ChildOptions.bChildComponent = true;
  ChildOptions.BlueprintName = TEXT("WBP_Button");
  const FConvertResult Child = Convert(*Button.Tree, ChildOptions);
  TestFalse(TEXT("child root has no size role"), Child.Owned[TEXT("9:1")].Contains(TEXT("size")));

  // The index: same hash → reuse, another hash → update.
  Options.ComponentIndex.Add(TEXT("btn-key"), {TEXT("/Game/Shared/WBP_Btn/WBP_Btn"), TEXT("h1")});
  Result = Convert(Doc, Options);
  TestEqual(TEXT("reused"), (int32)Result.Components[0].Action, (int32)EComponentAction::Reuse);
  TestEqual(TEXT("indexed path kept"), Result.Components[0].AssetPath, FString(TEXT("/Game/Shared/WBP_Btn/WBP_Btn")));
  Options.ComponentIndex[TEXT("btn-key")].Hash = TEXT("old");
  Result = Convert(Doc, Options);
  TestEqual(TEXT("updated"), (int32)Result.Components[0].Action, (int32)EComponentAction::Update);
  return true;
}

// Names survive a re-import: feed the owned map back, insert a node earlier
// and rename one; no existing widget changes name.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignNameStabilityTest, FUIWTDesignTestBase,
                                        "UIWidgetTool.DesignTree.NameStability", TestFlags)

bool FUIWTDesignNameStabilityTest::RunTest(const FString &Parameters)
{
  FDocument Doc;
  if (!LoadFixture(TEXT("layout"), Doc))
  {
    return false;
  }
  FConvertOptions Options = TestOptions();
  const FConvertResult First = Convert(Doc, Options);

  Doc.Root.Children.Insert(MakeShape(TEXT("2:99"), TEXT("Header"), 16, 0, 328, 10), 0);
  Doc.Root.Children[2].Children[0].Name = TEXT("Glyph");
  Options.Names = First.Owned;
  const FConvertResult Second = Convert(Doc, Options);
  CheckNames(Second, {});

  for (const TPair<FString, FRoleNames> &Node : First.Owned)
  {
    const FRoleNames *Again = Second.Owned.Find(Node.Key);
    if (!TestNotNull(FString::Printf(TEXT("%s still converted"), *Node.Key), Again))
    {
      continue;
    }
    for (const TPair<FString, FString> &Role : Node.Value)
    {
      TestEqual(FString::Printf(TEXT("%s %s keeps its name"), *Node.Key, *Role.Key),
                Again->FindRef(Role.Key), Role.Value);
    }
  }
  TestNotEqual(TEXT("the new node doesn't take the existing name"),
               Second.Owned[TEXT("2:99")][TEXT("main")], FString(TEXT("Header")));
  return true;
}

// Hundreds of same-named nodes get unique names that don't depend on order.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignSuffixTest, FUIWTDesignTestBase,
                                        "UIWidgetTool.DesignTree.SuffixCollision", TestFlags)

bool FUIWTDesignSuffixTest::RunTest(const FString &Parameters)
{
  FDocument Doc = MakeCanvasDocument(1000, 1000);
  for (int32 Index = 0; Index < 300; ++Index)
  {
    Doc.Root.Children.Add(MakeShape(FString::Printf(TEXT("L%03d"), Index), TEXT("Label"),
                                    Index, Index, 10, 10));
  }
  const FConvertResult Forward = Convert(Doc, TestOptions());
  CheckNames(Forward, {});
  TestEqual(TEXT("the lowest id keeps the plain name"), Forward.Owned[TEXT("L000")][TEXT("main")],
            FString(TEXT("Label")));

  Algo::Reverse(Doc.Root.Children);
  const FConvertResult Reversed = Convert(Doc, TestOptions());
  for (const TPair<FString, FRoleNames> &Node : Forward.Owned)
  {
    TestEqual(FString::Printf(TEXT("%s is named the same in any order"), *Node.Key),
              Reversed.Owned[Node.Key][TEXT("main")], Node.Value[TEXT("main")]);
  }
  return true;
}

// Reserved names, case, and role names colliding with layer names.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignReservedTest, FUIWTDesignTestBase,
                                        "UIWidgetTool.DesignTree.ReservedAndCase", TestFlags)

bool FUIWTDesignReservedTest::RunTest(const FString &Parameters)
{
  FDocument Doc = MakeCanvasDocument(800, 600);
  Doc.Root.Children.Add(MakeShape(TEXT("a"), TEXT("Visibility"), 0, 0, 10, 10));
  Doc.Root.Children.Add(MakeShape(TEXT("b"), TEXT("title"), 0, 20, 10, 10));
  Doc.Root.Children.Add(MakeShape(TEXT("c"), TEXT("Title"), 0, 40, 10, 10));
  FNode Card = MakeShape(TEXT("d"), TEXT("Card"), 0, 60, 100, 50);
  Card.Kind = EKind::Frame;
  Doc.Root.Children.Add(Card);
  Doc.Root.Children.Add(MakeShape(TEXT("e"), TEXT("Card_Bg"), 0, 120, 10, 10));
  Doc.Root.Children.Add(MakeShape(TEXT("f"), TEXT("None"), 0, 140, 10, 10));

  FConvertOptions Options = TestOptions();
  Options.ReservedNames = {TEXT("Visibility")};
  const FConvertResult Result = Convert(Doc, Options);
  CheckNames(Result, {TEXT("Visibility"), TEXT("None")});
  TestEqual(TEXT("the layer keeps Card_Bg"), Result.Owned[TEXT("e")][TEXT("main")], FString(TEXT("Card_Bg")));
  TestNotEqual(TEXT("the frame's background moves aside"), Result.Owned[TEXT("d")][TEXT("bg")],
               FString(TEXT("Card_Bg")));
  TestTrue(TEXT("title and Title differ"),
           !Result.Owned[TEXT("b")][TEXT("main")].Equals(Result.Owned[TEXT("c")][TEXT("main")],
                                                          ESearchCase::IgnoreCase));

  // A sidecar name that became reserved is replaced and reported.
  Options.Names.Add(TEXT("c"), {{TEXT("main"), TEXT("Padding")}});
  Options.ReservedNames.Add(TEXT("Padding"));
  const FConvertResult Again = Convert(Doc, Options);
  TestNotEqual(TEXT("reserved sidecar name replaced"), Again.Owned[TEXT("c")][TEXT("main")],
               FString(TEXT("Padding")));
  TestTrue(TEXT("nameReserved"), HasReport(Again, TEXT("c"), TEXT("nameReserved")));
  return true;
}

// Over Apply's widget limit: no spec, and the largest subtrees reported.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignLimitsTest, FUIWTDesignTestBase,
                                        "UIWidgetTool.DesignTree.Limits", TestFlags)

bool FUIWTDesignLimitsTest::RunTest(const FString &Parameters)
{
  FDocument Doc = MakeCanvasDocument(1000, 1000);
  for (int32 Index = 0; Index < MaxWidgets + 50; ++Index)
  {
    Doc.Root.Children.Add(MakeShape(FString::Printf(TEXT("n%d"), Index), TEXT("Dot"), 0, 0, 1, 1));
  }
  const FConvertResult Result = Convert(Doc, TestOptions());
  TestFalse(TEXT("no spec"), Result.Spec.IsValid());
  TestTrue(TEXT("limits reported"), HasReport(Result, TEXT("root"), TEXT("limits")));
  TestTrue(TEXT("over the limit"), Result.WidgetCount > MaxWidgets);
  return true;
}

namespace
{
  // Builds an Apply-ready option set: textures point at an engine texture,
  // reserved names come from UUserWidget, child WBPs go under /Temp.
  FConvertOptions ApplyOptions(const FDocument &InDoc, const FString &InRoot)
  {
    FConvertOptions Options = TestOptions();
    Options.TargetFolder = InRoot;
    Options.ComponentsFolder = InRoot / TEXT("Components");
    TSet<FString> Paths;
    CollectImagePaths(InDoc.Root, Paths);
    for (const TPair<FString, FComponentDef> &Def : InDoc.Components)
    {
      CollectImagePaths(Def.Value.Root, Paths);
    }
    for (const FString &Path : Paths)
    {
      Options.Textures.Add(Path, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
    }
    Options.ReservedNames = UIWTDesignImport::ReservedNamesForClass(UUserWidget::StaticClass());
    return Options;
  }

  UWidgetBlueprint *MakeBlueprint(UObject *InOuter, const FString &InName)
  {
    return Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
        UUserWidget::StaticClass(), InOuter, FName(*InName), BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
  }
}

// Every fixture's spec goes through Apply without an error, child WBPs
// first. This is what catches wrong UMG property names and value forms,
// which the converter itself can't check.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignApplyTest, FUIWTDesignTestBase,
                                        "UIWidgetTool.DesignTree.Apply", TestFlags)

bool FUIWTDesignApplyTest::RunTest(const FString &Parameters)
{
  for (const TCHAR *Fixture : Fixtures)
  {
    FDocument Doc;
    if (!LoadFixture(Fixture, Doc))
    {
      continue;
    }
    const FString Root = FString::Printf(TEXT("/Temp/UIWTDesignTest_%s"),
                                         *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FConvertOptions Options = ApplyOptions(Doc, Root);
    const FConvertResult Result = Convert(Doc, Options);
    if (!TestTrue(FString::Printf(TEXT("%s converts"), Fixture), Result.Spec.IsValid()))
    {
      continue;
    }

    // Child WBPs, deepest first.
    TArray<const FComponentJob *> Pending;
    for (const FComponentJob &Job : Result.Components)
    {
      Pending.Add(&Job);
    }
    TArray<FConvertResult> ChildResults;
    ChildResults.Reserve(16);
    while (!Pending.IsEmpty())
    {
      const FComponentJob &Job = *Pending.Pop();
      if (Job.Action != EComponentAction::Create || !Job.Tree.IsValid())
      {
        continue;
      }
      const FString Name = FPackageName::GetShortName(Job.AssetPath);
      FConvertOptions ChildOptions = ApplyOptions(*Job.Tree, Root);
      ChildOptions.bChildComponent = true;
      ChildOptions.BlueprintName = Name;
      ChildOptions.TargetFolder = Options.ComponentsFolder;
      const FConvertResult &Child = ChildResults.Add_GetRef(Convert(*Job.Tree, ChildOptions));
      UWidgetBlueprint *ChildBlueprint = MakeBlueprint(CreatePackage(*Job.AssetPath), Name);
      FString Report;
      TArray<FString> Errors;
      TestTrue(FString::Printf(TEXT("%s: child %s applies"), Fixture, *Name),
               ChildBlueprint && Child.Spec.IsValid() &&
                   UIWTWidgetSpec::Apply(ChildBlueprint, Child.Spec.ToSharedRef(), Report, Errors));
      for (const FString &Error : Errors)
      {
        AddError(FString::Printf(TEXT("%s: child %s: %s"), Fixture, *Name, *Error));
      }
    }

    UWidgetBlueprint *Blueprint = MakeBlueprint(GetTransientPackage(),
                                                FString::Printf(TEXT("WBP_%s_Apply"), Fixture));
    FString Report;
    TArray<FString> Errors;
    TestTrue(FString::Printf(TEXT("%s applies"), Fixture),
             Blueprint && UIWTWidgetSpec::Apply(Blueprint, Result.Spec.ToSharedRef(), Report, Errors));
    for (const FString &Error : Errors)
    {
      AddError(FString::Printf(TEXT("%s: %s"), Fixture, *Error));
    }
    AddInfo(FString::Printf(TEXT("%s: %s"), Fixture, *Report));
    TestFalse(FString::Printf(TEXT("%s compiles"), Fixture), Report.Contains(TEXT("FAILED")));
  }
  return true;
}

// A generated screen of about 750 nodes: converter, Apply and Export times,
// widgets per node, spec size.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignScaleTest, FUIWTDesignTestBase,
                                        "UIWidgetTool.DesignTree.Scale", TestFlags)

bool FUIWTDesignScaleTest::RunTest(const FString &Parameters)
{
  FDocument Doc = MakeCanvasDocument(1920, 1080);
  TSharedRef<FLayout> Column = MakeShared<FLayout>();
  Column->Mode = ELayoutMode::Vertical;
  Column->Spacing = 4;
  Doc.Root.Layout = Column;
  TSharedRef<FLayout> RowLayout = MakeShared<FLayout>();
  RowLayout->Spacing = 8;
  RowLayout->Padding = {8, 4, 8, 4};
  RowLayout->AlignMain = EAlignMain::Center;
  RowLayout->AlignCross = EAlignCross::Center;
  TSharedRef<FStroke> Stroke = MakeShared<FStroke>();
  Stroke->Weights = {1, 1, 1, 1};
  Stroke->Align = EStrokeAlign::Center;
  TSharedRef<FTextBlock> Text = MakeShared<FTextBlock>();
  Text->Content = TEXT("Item");
  Text->Runs.AddDefaulted();
  Text->Runs[0].Size = 14;
  for (int32 Row = 0; Row < 150; ++Row)
  {
    FNode RowNode = MakeShape(FString::Printf(TEXT("r%d"), Row), TEXT("Row"), 0, Row * 7, 1920, 40);
    RowNode.Kind = EKind::Frame;
    RowNode.Layout = RowLayout;
    FNode Label;
    Label.Id = FString::Printf(TEXT("r%d/label"), Row);
    Label.Name = TEXT("Label");
    Label.Kind = EKind::Text;
    Label.Box = {0, 0, 200, 20};
    Label.Text = Text;
    RowNode.Children.Add(Label);
    FNode Swatch = MakeShape(FString::Printf(TEXT("r%d/swatch"), Row), TEXT("Swatch"), 0, 0, 30, 30);
    Swatch.Stroke = Stroke;
    RowNode.Children.Add(Swatch);
    RowNode.Children.Add(MakeShape(FString::Printf(TEXT("r%d/bar"), Row), TEXT("Bar"), 0, 0, 300, 8));
    Label.Id = FString::Printf(TEXT("r%d/value"), Row);
    Label.Name = TEXT("Value");
    RowNode.Children.Add(Label);
    Doc.Root.Children.Add(RowNode);
  }

  double Start = FPlatformTime::Seconds();
  const FConvertResult Result = Convert(Doc, TestOptions());
  const double ConvertSeconds = FPlatformTime::Seconds() - Start;
  if (!TestTrue(TEXT("converts"), Result.Spec.IsValid()))
  {
    return false;
  }
  const int32 Nodes = 1 + 150 * 5;
  FString Report;
  TArray<FString> Errors;
  UWidgetBlueprint *Blueprint = MakeBlueprint(GetTransientPackage(), TEXT("WBP_Scale_Apply"));
  Start = FPlatformTime::Seconds();
  TestTrue(TEXT("applies"), UIWTWidgetSpec::Apply(Blueprint, Result.Spec.ToSharedRef(), Report, Errors));
  const double ApplySeconds = FPlatformTime::Seconds() - Start;
  TestEqual(TEXT("no Apply errors"), Errors.Num(), 0);
  FString Exported;
  FString Error;
  Start = FPlatformTime::Seconds();
  UIWTWidgetSpec::Export(Blueprint, Exported, Error);
  const double ExportSeconds = FPlatformTime::Seconds() - Start;
  AddInfo(FString::Printf(
      TEXT("%d nodes -> %d widgets (%.2f per node), depth %d. Convert %.3fs, Apply %.3fs, "
           "Export %.3fs; exported spec %d bytes."),
      Nodes, Result.WidgetCount, double(Result.WidgetCount) / Nodes, Result.MaxDepth,
      ConvertSeconds, ApplySeconds, ExportSeconds, Exported.Len()));
  return true;
}

// Hints a reader can't express otherwise (import-image.md, section 2): a
// measured row and column, a Button, a scrolling list, overlapping children
// left alone, and a spec Apply takes.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignHintsTest, FUIWTDesignTestBase,
                                        "UIWidgetTool.DesignTree.Hints", TestFlags)

bool FUIWTDesignHintsTest::RunTest(const FString &Parameters)
{
  const FString Json = TEXT(R"json({
    "version": 1, "source": "image", "referenceSize": { "w": 400, "h": 300 },
    "root": { "id": "root", "name": "Root", "kind": "frame", "box": { "x": 0, "y": 0, "w": 400, "h": 300 },
      "children": [
        { "id": "bar", "name": "Bar", "kind": "frame", "box": { "x": 10, "y": 10, "w": 300, "h": 60 },
          "hints": [ "layout:row" ],
          "children": [
            { "id": "b1", "name": "B1", "kind": "shape", "box": { "x": 20, "y": 10, "w": 60, "h": 40 }, "fill": { "color": "#FF0000" } },
            { "id": "b3", "name": "B3", "kind": "shape", "box": { "x": 180, "y": 10, "w": 60, "h": 40 }, "fill": { "color": "#0000FF" } },
            { "id": "b2", "name": "B2", "kind": "shape", "box": { "x": 100, "y": 10, "w": 60, "h": 40 }, "fill": { "color": "#00FF00" } } ] },
        { "id": "col", "name": "Col", "kind": "group", "box": { "x": 10, "y": 80, "w": 100, "h": 120 },
          "hints": [ "layout:column" ],
          "children": [
            { "id": "c1", "name": "C1", "kind": "shape", "box": { "x": 20, "y": 0, "w": 60, "h": 30 }, "fill": { "color": "#FFFFFF" } },
            { "id": "c2", "name": "C2", "kind": "shape", "box": { "x": 30, "y": 40, "w": 40, "h": 30 }, "fill": { "color": "#FFFFFF" } },
            { "id": "c3", "name": "C3", "kind": "shape", "box": { "x": 25, "y": 84, "w": 50, "h": 30 }, "fill": { "color": "#FFFFFF" } } ] },
        { "id": "btn", "name": "Btn_Play", "kind": "frame", "box": { "x": 200, "y": 100, "w": 120, "h": 40 },
          "fill": { "color": "#3366FF" }, "radii": [ 8, 8, 8, 8 ], "hints": [ "role:button" ],
          "children": [
            { "id": "lbl", "name": "Label", "kind": "text", "box": { "x": 0, "y": 0, "w": 120, "h": 40 },
              "text": { "content": "Play", "runs": [ { "font": { "family": "Unmapped" }, "size": 20, "color": "#FFFFFF" } ],
                        "align": "center", "valign": "center", "sizing": "fixed" } } ] },
        { "id": "list", "name": "List", "kind": "frame", "box": { "x": 200, "y": 150, "w": 150, "h": 100 },
          "hints": [ "layout:column", "role:list" ],
          "children": [
            { "id": "l1", "name": "L1", "kind": "shape", "box": { "x": 0, "y": 0, "w": 150, "h": 40 }, "fill": { "color": "#222222" } },
            { "id": "l2", "name": "L2", "kind": "shape", "box": { "x": 0, "y": 50, "w": 150, "h": 40 }, "fill": { "color": "#222222" } } ] },
        { "id": "ov", "name": "Overlap", "kind": "frame", "box": { "x": 10, "y": 220, "w": 100, "h": 50 },
          "hints": [ "layout:row" ],
          "children": [
            { "id": "o1", "name": "O1", "kind": "shape", "box": { "x": 0, "y": 0, "w": 60, "h": 40 }, "fill": { "color": "#FFFFFF" } },
            { "id": "o2", "name": "O2", "kind": "shape", "box": { "x": 40, "y": 0, "w": 60, "h": 40 }, "fill": { "color": "#FFFFFF" } } ] }
      ] } })json");
  FDocument Doc;
  TArray<FString> Errors;
  if (!TestTrue(TEXT("read"), ReadDocument(Json, Doc, Errors)))
  {
    return false;
  }
  TestTrue(TEXT("has hints"), HasLayoutHints(Doc));
  const FConvertResult Result = Convert(Doc, TestOptions());
  if (!TestTrue(TEXT("spec"), Result.Spec.IsValid()))
  {
    return false;
  }

  // A row, children in box order, the gaps and margins measured.
  TestEqual(TEXT("row"), String(Widget(Result, TEXT("bar")), TEXT("class")),
            FString(TEXT("HorizontalBox")));
  const TSharedPtr<FJsonObject> BarBg = Widget(Result, TEXT("bar"), TEXT("bg"));
  TestEqual(TEXT("row padding left"), Number(BarBg, TEXT("props.Padding.Left")), 20.0);
  TestEqual(TEXT("row padding top"), Number(BarBg, TEXT("props.Padding.Top")), 10.0);
  TestEqual(TEXT("row padding right"), Number(BarBg, TEXT("props.Padding.Right")), 60.0);
  const TSharedPtr<FJsonObject> Second = Widget(Result, TEXT("b2"), TEXT("size"));
  TestEqual(TEXT("B2 is second, after the gap"), Number(Second, TEXT("slot.Padding.Left")), 20.0);
  TestEqual(TEXT("aligned at the top"), String(Second, TEXT("slot.VerticalAlignment")),
            FString(TEXT("VAlign_Top")));

  // A column from a group, centred, with uneven gaps noted.
  TestEqual(TEXT("column"), String(Widget(Result, TEXT("col")), TEXT("class")),
            FString(TEXT("VerticalBox")));
  TestEqual(TEXT("centred"),
            String(Widget(Result, TEXT("c2"), TEXT("size")), TEXT("slot.HorizontalAlignment")),
            FString(TEXT("HAlign_Center")));
  TestTrue(TEXT("uneven gaps noted"), HasReport(Result, TEXT("col"), TEXT("layoutApprox")));

  // A Button carrying the frame's look.
  const TSharedPtr<FJsonObject> Button = Widget(Result, TEXT("btn"), TEXT("button"));
  TestEqual(TEXT("button"), String(Button, TEXT("class")), FString(TEXT("Button")));
  TestEqual(TEXT("normal style"), String(Button, TEXT("props.WidgetStyle.Normal.DrawAs")),
            FString(TEXT("RoundedBox")));
  TestTrue(TEXT("hovered is lighter"),
           Number(Button, TEXT("props.WidgetStyle.Hovered.TintColor.SpecifiedColor.r")) >
               Number(Button, TEXT("props.WidgetStyle.Normal.TintColor.SpecifiedColor.r")));
  TestTrue(TEXT("pressed is darker"),
           Number(Button, TEXT("props.WidgetStyle.Pressed.TintColor.SpecifiedColor.b")) <
               Number(Button, TEXT("props.WidgetStyle.Normal.TintColor.SpecifiedColor.b")));
  TestFalse(TEXT("no Border as well"), Result.Owned[TEXT("btn")].Contains(TEXT("bg")));

  // A list scrolls.
  const TSharedPtr<FJsonObject> Scroll = Widget(Result, TEXT("list"), TEXT("scroll"));
  TestEqual(TEXT("scroll"), String(Scroll, TEXT("class")), FString(TEXT("ScrollBox")));
  TestEqual(TEXT("scrolls vertically"), String(Scroll, TEXT("props.Orientation")),
            FString(TEXT("Orient_Vertical")));
  TestEqual(TEXT("list items in a column"), String(Widget(Result, TEXT("list")), TEXT("class")),
            FString(TEXT("VerticalBox")));

  // Overlapping children aren't a row.
  TestEqual(TEXT("overlap stays a canvas"), String(Widget(Result, TEXT("ov")), TEXT("class")),
            FString(TEXT("CanvasPanel")));
  TestTrue(TEXT("overlap noted"), HasReport(Result, TEXT("ov"), TEXT("hintIgnored")));

  // A tree without hints is left alone.
  FDocument Plain;
  if (LoadFixture(TEXT("screen"), Plain))
  {
    TestFalse(TEXT("no hints in the fixture"), HasLayoutHints(Plain));
  }

  // UMG takes it: the Button style's property names are right.
  UWidgetBlueprint *Blueprint = MakeBlueprint(
      GetTransientPackage(),
      MakeUniqueObjectName(GetTransientPackage(), UWidgetBlueprint::StaticClass(),
                           TEXT("WBP_UIWTHints"))
          .ToString());
  FString Report;
  TestTrue(TEXT("applies"),
           Blueprint && UIWTWidgetSpec::Apply(Blueprint, Result.Spec.ToSharedRef(), Report, Errors));
  for (const FString &Error : Errors)
  {
    AddError(TEXT("Apply: ") + Error);
  }
  return true;
}

// Text strokes and shadows: a font outline and a TextBlock shadow, the
// TextBlock grown by them as Slate measures them (so the glyphs stay put),
// and what Slate can't draw reported.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTDesignTextEffectsTest, FUIWTDesignTestBase,
                                        "UIWidgetTool.DesignTree.TextEffects", TestFlags)

bool FUIWTDesignTextEffectsTest::RunTest(const FString &Parameters)
{
  FDocument Doc;
  TArray<FString> Errors;
  const bool bRead = ReadDocument(TEXT(R"json({
    "version": 1, "source": "figma", "referenceSize": { "w": 400, "h": 300 },
    "root": {
      "id": "t:1", "name": "Strokes", "kind": "frame", "box": { "x": 0, "y": 0, "w": 400, "h": 300 },
      "children": [
        { "id": "t:2", "name": "Outside", "kind": "text", "box": { "x": 20, "y": 30, "w": 100, "h": 40 },
          "stroke": { "color": "#FF0000", "weights": { "l": 4, "t": 4, "r": 4, "b": 4 }, "align": "outside" },
          "text": { "content": "SPIN!", "runs": [ { "font": { "family": "Inter", "style": "Bold" }, "size": 32, "color": "#FFFFFF" } ] } },
        { "id": "t:3", "name": "Center", "kind": "text", "box": { "x": 20, "y": 100, "w": 200, "h": 24 },
          "stroke": { "color": "#000000", "weights": { "l": 3, "t": 3, "r": 3, "b": 3 }, "align": "center" },
          "text": { "content": "Centered", "runs": [ { "font": { "family": "Inter", "style": "Regular" }, "size": 16, "color": "#FFFFFF" } ], "sizing": "fixedWidth" } },
        { "id": "t:4", "name": "Inside", "kind": "text", "box": { "x": 20, "y": 150, "w": 100, "h": 24 },
          "stroke": { "color": "#000000", "weights": { "l": 2, "t": 2, "r": 2, "b": 2 }, "align": "inside" },
          "text": { "content": "Inside", "runs": [ { "font": { "family": "Inter", "style": "Regular" }, "size": 16, "color": "#FFFFFF" } ] } },
        { "id": "t:5", "name": "Row", "kind": "frame", "box": { "x": 20, "y": 200, "w": 300, "h": 40 },
          "layout": { "mode": "horizontal", "spacing": 10 },
          "children": [
            { "id": "t:6", "name": "First", "kind": "text", "box": { "x": 0, "y": 0, "w": 60, "h": 24 },
              "text": { "content": "x 1", "runs": [ { "font": { "family": "Inter", "style": "Bold" }, "size": 16, "color": "#FFFFFF" } ] } },
            { "id": "t:7", "name": "Second", "kind": "text", "box": { "x": 70, "y": 0, "w": 60, "h": 24 },
              "stroke": { "color": "#000000", "weights": { "l": 2, "t": 2, "r": 2, "b": 2 }, "align": "outside" },
              "text": { "content": "x 4", "runs": [ { "font": { "family": "Inter", "style": "Bold" }, "size": 16, "color": "#FFFFFF" } ] } }
          ] }
,
        { "id": "t:8", "name": "Jackpot", "kind": "text", "box": { "x": 20, "y": 250, "w": 100, "h": 40 },
          "stroke": { "color": "#000000", "weights": { "l": 4, "t": 4, "r": 4, "b": 4 }, "align": "outside" },
          "effects": [ { "type": "dropShadow", "baked": false, "color": "#00000040", "offset": { "x": 0, "y": 4 }, "radius": 4, "spread": 0 } ],
          "text": { "content": "JACKPOT", "runs": [ { "font": { "family": "Inter", "style": "Bold" }, "size": 32, "color": "#FFFFFF" } ] } },
        { "id": "t:9", "name": "Raised", "kind": "text", "box": { "x": 200, "y": 30, "w": 100, "h": 24 },
          "effects": [ { "type": "innerShadow", "baked": false, "color": "#000000", "offset": { "x": 1, "y": 1 }, "radius": 0, "spread": 0 },
                       { "type": "dropShadow", "baked": false, "color": "#FF0000", "offset": { "x": -2, "y": -3 }, "radius": 0, "spread": 0 } ],
          "text": { "content": "Up", "runs": [ { "font": { "family": "Inter", "style": "Regular" }, "size": 16, "color": "#FFFFFF" } ] } }
      ]
    }})json"),
                                  Doc, Errors);
  for (const FString &Error : Errors)
  {
    AddError(Error);
  }
  if (!TestTrue(TEXT("reads"), bRead))
  {
    return false;
  }
  const FConvertResult Result = Convert(Doc, TestOptions());
  if (!TestTrue(TEXT("a spec"), Result.Spec.IsValid()))
  {
    return false;
  }

  // Outside: the full weight, in linear colour; the slot moves out by it.
  const TSharedPtr<FJsonObject> Outside = Widget(Result, TEXT("t:2"));
  TestEqual(TEXT("outside size"), Number(Outside, TEXT("props.Font.OutlineSettings.OutlineSize")), 4.0);
  TestEqual(TEXT("outside colour"), Number(Outside, TEXT("props.Font.OutlineSettings.OutlineColor.r")), 1.0);
  TestEqual(TEXT("outside colour is linear"), Number(Outside, TEXT("props.Font.OutlineSettings.OutlineColor.g")), 0.0);
  TestEqual(TEXT("outside left"), Number(Outside, TEXT("slot.LayoutData.Offsets.Left")), 16.0);
  TestEqual(TEXT("outside top"), Number(Outside, TEXT("slot.LayoutData.Offsets.Top")), 26.0);
  TestFalse(TEXT("outside not reported"), HasReport(Result, TEXT("t:2"), TEXT("textStroke")));

  // Center: the outer half, rounded to whole units; the wrap width grows.
  TestEqual(TEXT("center size"),
            Number(Widget(Result, TEXT("t:3")), TEXT("props.Font.OutlineSettings.OutlineSize")), 2.0);
  const TSharedPtr<FJsonObject> CenterSize = Widget(Result, TEXT("t:3"), TEXT("size"));
  TestEqual(TEXT("center wrap width"), Number(CenterSize, TEXT("props.WidthOverride")), 204.0);
  TestEqual(TEXT("center left"), Number(CenterSize, TEXT("slot.LayoutData.Offsets.Left")), 18.0);
  TestTrue(TEXT("center reported"), HasReport(Result, TEXT("t:3"), TEXT("textStroke")));

  // Inside: nothing to draw, nothing grown.
  const TSharedPtr<FJsonObject> Inside = Widget(Result, TEXT("t:4"));
  TestFalse(TEXT("inside has no outline"), At(Inside, TEXT("props.Font.OutlineSettings")).IsValid());
  TestEqual(TEXT("inside left"), Number(Inside, TEXT("slot.LayoutData.Offsets.Left")), 20.0);
  TestTrue(TEXT("inside reported"), HasReport(Result, TEXT("t:4"), TEXT("textStroke")));

  // In a row the outline overlaps its neighbours instead of pushing them.
  const TSharedPtr<FJsonObject> Second = Widget(Result, TEXT("t:7"));
  TestEqual(TEXT("row lead less the outline"), Number(Second, TEXT("slot.Padding.Left")), 8.0);
  TestEqual(TEXT("row top less the outline"), Number(Second, TEXT("slot.Padding.Top")), -2.0);
  TestFalse(TEXT("unstroked text has no outline"),
            At(Widget(Result, TEXT("t:6")), TEXT("props.Font.OutlineSettings")).IsValid());

  // A shadow under an outline: the outline casts it too, the TextBlock grows
  // down by the offset, the blur is reported.
  const TSharedPtr<FJsonObject> Jackpot = Widget(Result, TEXT("t:8"));
  TestEqual(TEXT("shadow offset"), Number(Jackpot, TEXT("props.ShadowOffset.Y")), 4.0);
  TestEqual(TEXT("shadow alpha"), Number(Jackpot, TEXT("props.ShadowColorAndOpacity.a")), 0.25098);
  TestTrue(TEXT("outline casts the shadow"),
           At(Jackpot, TEXT("props.Font.OutlineSettings.bApplyOutlineToDropShadows")).IsValid());
  TestEqual(TEXT("shadowed left"), Number(Jackpot, TEXT("slot.LayoutData.Offsets.Left")), 16.0);
  TestEqual(TEXT("shadowed top"), Number(Jackpot, TEXT("slot.LayoutData.Offsets.Top")), 246.0);
  TestTrue(TEXT("blur reported"), HasReport(Result, TEXT("t:8"), TEXT("shadowApprox")));
  TestFalse(TEXT("shadow not dropped"), HasReport(Result, TEXT("t:8"), TEXT("effectDropped")));

  // A negative offset moves the glyphs right and down in the TextBlock, so
  // the slot moves left and up by it. The inner shadow is still dropped.
  const TSharedPtr<FJsonObject> Raised = Widget(Result, TEXT("t:9"));
  TestEqual(TEXT("raised shadow x"), Number(Raised, TEXT("props.ShadowOffset.X")), -2.0);
  TestEqual(TEXT("raised left"), Number(Raised, TEXT("slot.LayoutData.Offsets.Left")), 198.0);
  TestEqual(TEXT("raised top"), Number(Raised, TEXT("slot.LayoutData.Offsets.Top")), 27.0);
  TestFalse(TEXT("raised has no outline"), At(Raised, TEXT("props.Font.OutlineSettings")).IsValid());
  TestTrue(TEXT("inner shadow dropped"), HasReport(Result, TEXT("t:9"), TEXT("effectDropped")));
  TestFalse(TEXT("sharp shadow not reported"), HasReport(Result, TEXT("t:9"), TEXT("shadowApprox")));

  // UMG takes it.
  UWidgetBlueprint *Blueprint = MakeBlueprint(
      GetTransientPackage(),
      MakeUniqueObjectName(GetTransientPackage(), UWidgetBlueprint::StaticClass(),
                           TEXT("WBP_UIWTTextEffects"))
          .ToString());
  FString Report;
  TestTrue(TEXT("applies"),
           Blueprint && UIWTWidgetSpec::Apply(Blueprint, Result.Spec.ToSharedRef(), Report, Errors));
  for (const FString &Error : Errors)
  {
    AddError(TEXT("Apply: ") + Error);
  }
  const UTextBlock *TextBlock =
      Blueprint ? Cast<UTextBlock>(Blueprint->WidgetTree->FindWidget(
                      FName(*Result.Owned[TEXT("t:2")][TEXT("main")])))
                : nullptr;
  if (TestNotNull(TEXT("outlined TextBlock"), TextBlock))
  {
    TestEqual(TEXT("applied outline size"), TextBlock->GetFont().OutlineSettings.OutlineSize, 4);
    TestTrue(TEXT("applied outline colour"),
             TextBlock->GetFont().OutlineSettings.OutlineColor.Equals(FLinearColor::Red));
  }
  const UTextBlock *Shadowed =
      Blueprint ? Cast<UTextBlock>(Blueprint->WidgetTree->FindWidget(
                      FName(*Result.Owned[TEXT("t:8")][TEXT("main")])))
                : nullptr;
  if (TestNotNull(TEXT("shadowed TextBlock"), Shadowed))
  {
    TestTrue(TEXT("applied shadow offset"), Shadowed->GetShadowOffset().Equals(FVector2D(0.0, 4.0)));
    TestTrue(TEXT("applied shadow colour"),
             Shadowed->GetShadowColorAndOpacity().Equals(FLinearColor(0.0f, 0.0f, 0.0f, 0.25098f), 1e-4f));
    TestTrue(TEXT("applied outline casts the shadow"),
             Shadowed->GetFont().OutlineSettings.bApplyOutlineToDropShadows);
  }
  return true;
}

#endif
