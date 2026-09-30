#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Animation/WidgetAnimation.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Core/UIWTWidgetSpec.h"
#include "Dom/JsonObject.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/UIWTSpecTestWidget.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

// Editor automation tests for UIWTWidgetSpec (Apply, Export,
// FindProtectedWidgets). Each test builds a Widget Blueprint in the
// transient package, so nothing is saved. Run them from the Session
// Frontend, or headless:
//   UnrealEditor-Cmd.exe <project> -ExecCmds="Automation RunTests UIWidgetTool.WidgetSpec; Quit" -unattended -nullrhi
namespace
{
  // Compiling a half-built test blueprint logs compiler messages (a missing
  // required BindWidget, for one). The tests check Apply's results
  // themselves, so those log lines are not failures.
  class FUIWTSpecTestBase : public FAutomationTestBase
  {
  public:
    FUIWTSpecTestBase(const FString &InName, const bool bInComplexTask)
        : FAutomationTestBase(InName, bInComplexTask)
    {
    }
    virtual bool SuppressLogErrors() override { return true; }
    virtual bool SuppressLogWarnings() override { return true; }

  protected:
    UWidgetBlueprint *MakeBlueprint(UClass *InParent = UUserWidget::StaticClass())
    {
      UPackage *Package = GetTransientPackage();
      const FName Name = MakeUniqueObjectName(
          Package, UWidgetBlueprint::StaticClass(), TEXT("WBP_UIWTSpecTest"));
      return Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
          InParent, Package, Name, BPTYPE_Normal, UWidgetBlueprint::StaticClass(),
          UWidgetBlueprintGeneratedClass::StaticClass()));
    }

    // Apply, with every error message added to the test log.
    bool Apply(UWidgetBlueprint *InBlueprint, const FString &InSpec,
               FString &OutReport, TArray<FString> &OutErrors)
    {
      const bool bApplied =
          UIWTWidgetSpec::Apply(InBlueprint, InSpec, OutReport, OutErrors);
      for (const FString &Error : OutErrors)
      {
        AddInfo(FString::Printf(TEXT("Apply: %s"), *Error));
      }
      return bApplied;
    }

    bool ErrorsContain(const TArray<FString> &InErrors, const FString &InText)
    {
      return InErrors.ContainsByPredicate(
          [&InText](const FString &Error) { return Error.Contains(InText); });
    }
  };

  constexpr EAutomationTestFlags TestFlags =
      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
}

// Apply builds the tree, Export writes it without float noise, and applying
// the export again keeps every widget and exports the same text.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTSpecRoundTripTest, FUIWTSpecTestBase,
                                        "UIWidgetTool.WidgetSpec.RoundTrip", TestFlags)

bool FUIWTSpecRoundTripTest::RunTest(const FString &Parameters)
{
  UWidgetBlueprint *Blueprint = MakeBlueprint();
  if (!TestNotNull(TEXT("blueprint"), Blueprint))
  {
    return false;
  }
  const FString Spec = TEXT(R"json({"root":{"class":"CanvasPanel","name":"Root","children":[
    {"class":"Border","name":"Card",
     "props":{"Padding":{"Left":0,"Top":0,"Right":0,"Bottom":0},
              "BrushColor":{"r":0.5,"g":0.25,"b":0.1,"a":1}},
     "slot":{"LayoutData":{"Offsets":{"Left":10.1,"Top":20,"Right":200,"Bottom":100}}},
     "children":[{"class":"TextBlock","name":"Title"}]},
    {"class":"Image","name":"Icon","variable":false}]}})json");
  FString Report;
  TArray<FString> Errors;
  TestTrue(TEXT("first Apply succeeds"), Apply(Blueprint, Spec, Report, Errors));
  TestEqual(TEXT("first Apply errors"), Errors.Num(), 0);

  FString First;
  FString Error;
  TestTrue(TEXT("Export succeeds"), UIWTWidgetSpec::Export(Blueprint, First, Error));
  AddInfo(First);
  TestTrue(TEXT("a float offset is written as typed"), First.Contains(TEXT(":10.1,")));
  TestFalse(TEXT("no float noise"), First.Contains(TEXT("10.10000")));
  TestTrue(TEXT("colour channels are short"), First.Contains(TEXT("\"g\":0.25")));

  TestTrue(TEXT("second Apply succeeds"), Apply(Blueprint, First, Report, Errors));
  TestEqual(TEXT("second Apply errors"), Errors.Num(), 0);
  TestTrue(TEXT("every widget kept"), Report.Contains(TEXT("(0 created, 4 kept, 0 removed)")));

  FString Second;
  UIWTWidgetSpec::Export(Blueprint, Second, Error);
  TestEqual(TEXT("Export is stable"), Second, First);
  return true;
}

// A colour with a missing channel takes it from the current value, and the
// caller's parsed spec is left as it was.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTSpecIncompleteColorTest, FUIWTSpecTestBase,
                                        "UIWidgetTool.WidgetSpec.IncompleteColor", TestFlags)

bool FUIWTSpecIncompleteColorTest::RunTest(const FString &Parameters)
{
  UWidgetBlueprint *Blueprint = MakeBlueprint();
  if (!TestNotNull(TEXT("blueprint"), Blueprint))
  {
    return false;
  }
  TSharedPtr<FJsonObject> Spec;
  FJsonSerializer::Deserialize(
      TJsonReaderFactory<>::Create(TEXT(R"json({"root":{"class":"CanvasPanel","name":"Root","children":[
        {"class":"Border","name":"Card","props":{"BrushColor":{"r":0.5}}}]}})json")),
      Spec);
  if (!TestTrue(TEXT("spec parses"), Spec.IsValid()))
  {
    return false;
  }
  FString Report;
  TArray<FString> Errors;
  TestTrue(TEXT("Apply succeeds"),
           UIWTWidgetSpec::Apply(Blueprint, Spec.ToSharedRef(), Report, Errors));
  TestEqual(TEXT("Apply errors"), Errors.Num(), 0);

  const TSharedPtr<FJsonObject> Color = Spec->GetObjectField(TEXT("root"))
                                            ->GetArrayField(TEXT("children"))[0]
                                            ->AsObject()
                                            ->GetObjectField(TEXT("props"))
                                            ->GetObjectField(TEXT("BrushColor"));
  TestEqual(TEXT("the caller's colour keeps one channel"), Color->Values.Num(), 1);

  const UBorder *Card = Cast<UBorder>(Blueprint->WidgetTree->FindWidget(TEXT("Card")));
  if (TestNotNull(TEXT("Card"), Card))
  {
    // g, b and a from the new Border's current (default) white.
    const FLinearColor BrushColor = Card->GetBrushColor();
    AddInfo(FString::Printf(TEXT("BrushColor %s"), *BrushColor.ToString()));
    TestTrue(TEXT("completed colour"), BrushColor.Equals(FLinearColor(0.5f, 1.f, 1.f, 1.f)));
  }
  return true;
}

// New widgets can't take names the blueprint already uses, but a widget can
// be replaced by one of another class under the same name, even when it's a
// variable.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTSpecNamesTest, FUIWTSpecTestBase,
                                        "UIWidgetTool.WidgetSpec.Names", TestFlags)

bool FUIWTSpecNamesTest::RunTest(const FString &Parameters)
{
  UWidgetBlueprint *Blueprint = MakeBlueprint();
  if (!TestNotNull(TEXT("blueprint"), Blueprint))
  {
    return false;
  }
  FString Report;
  TArray<FString> Errors;
  TestFalse(TEXT("a widget named like a UWidget property is refused"),
            Apply(Blueprint, TEXT(R"json({"root":{"class":"CanvasPanel","name":"Root","children":[
              {"class":"TextBlock","name":"Visibility"}]}})json"),
                  Report, Errors));
  TestTrue(TEXT("the error names it"), ErrorsContain(Errors, TEXT("Visibility")));
  TestNull(TEXT("nothing was built"), Blueprint->WidgetTree->RootWidget.Get());

  TestTrue(TEXT("a variable panel is created"),
           Apply(Blueprint, TEXT(R"json({"root":{"class":"CanvasPanel","name":"Root","children":[
             {"class":"CanvasPanel","name":"Panel","variable":true}]}})json"),
                 Report, Errors));
  TestTrue(TEXT("its class can change"),
           Apply(Blueprint, TEXT(R"json({"root":{"class":"CanvasPanel","name":"Root","children":[
             {"class":"HorizontalBox","name":"Panel","variable":true}]}})json"),
                 Report, Errors));
  TestEqual(TEXT("no errors"), Errors.Num(), 0);
  TestNotNull(TEXT("Panel is a HorizontalBox now"),
              Cast<UHorizontalBox>(Blueprint->WidgetTree->FindWidget(TEXT("Panel"))));
  return true;
}

// Property bindings and animations protect their widgets: FindProtectedWidgets
// lists them, and Apply refuses to delete them.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTSpecProtectedTest, FUIWTSpecTestBase,
                                        "UIWidgetTool.WidgetSpec.Protected", TestFlags)

bool FUIWTSpecProtectedTest::RunTest(const FString &Parameters)
{
  UWidgetBlueprint *Blueprint = MakeBlueprint();
  if (!TestNotNull(TEXT("blueprint"), Blueprint))
  {
    return false;
  }
  FString Report;
  TArray<FString> Errors;
  TestTrue(TEXT("tree built"),
           Apply(Blueprint, TEXT(R"json({"root":{"class":"CanvasPanel","name":"Root","children":[
             {"class":"TextBlock","name":"Label"},{"class":"Image","name":"Img"}]}})json"),
                 Report, Errors));

  FDelegateEditorBinding Binding;
  Binding.ObjectName = TEXT("Label");
  Binding.PropertyName = TEXT("Text");
  Blueprint->Bindings.Add(Binding);
  UWidgetAnimation *Animation = NewObject<UWidgetAnimation>(Blueprint, TEXT("Fade"));
  FWidgetAnimationBinding AnimationBinding;
  AnimationBinding.WidgetName = TEXT("Img");
  Animation->AnimationBindings.Add(AnimationBinding);
  Blueprint->Animations.Add(Animation);

  const TArray<UIWTWidgetSpec::FProtectedWidget> Protected =
      UIWTWidgetSpec::FindProtectedWidgets(Blueprint);
  auto ReasonsOf = [&Protected](const TCHAR *InName)
  {
    const UIWTWidgetSpec::FProtectedWidget *Found = Protected.FindByPredicate(
        [InName](const UIWTWidgetSpec::FProtectedWidget &Widget)
        { return Widget.Name == FName(InName); });
    return Found ? Found->Reasons : TArray<FString>();
  };
  TestTrue(TEXT("Label is protected by its binding"),
           ReasonsOf(TEXT("Label")).Contains(TEXT("property binding Text")));
  TestTrue(TEXT("Img is protected by its animation"),
           ReasonsOf(TEXT("Img")).Contains(TEXT("animation Fade")));

  TestFalse(TEXT("deleting them is refused"),
            Apply(Blueprint, TEXT(R"json({"root":{"class":"CanvasPanel","name":"Root"}})json"),
                  Report, Errors));
  TestTrue(TEXT("the binding is named"), ErrorsContain(Errors, TEXT("property binding on Text")));
  TestTrue(TEXT("the animation is named"), ErrorsContain(Errors, TEXT("animated by 'Fade'")));
  TestNotNull(TEXT("Label is still there"), Blueprint->WidgetTree->FindWidget(TEXT("Label")));

  // Leave nothing a later compile would trip over.
  Blueprint->Animations.Empty();
  Blueprint->Bindings.Empty();
  return true;
}

// Parent-class BindWidget properties: required ones must be present, every
// one must get a widget of its class, and their names don't count as taken.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTSpecBindWidgetTest, FUIWTSpecTestBase,
                                        "UIWidgetTool.WidgetSpec.BindWidget", TestFlags)

bool FUIWTSpecBindWidgetTest::RunTest(const FString &Parameters)
{
  UWidgetBlueprint *Blueprint = MakeBlueprint(UUIWTSpecTestWidget::StaticClass());
  if (!TestNotNull(TEXT("blueprint"), Blueprint))
  {
    return false;
  }
  FString Report;
  TArray<FString> Errors;
  TestFalse(TEXT("an optional binding of the wrong class is refused"),
            Apply(Blueprint, TEXT(R"json({"root":{"class":"CanvasPanel","name":"Root","children":[
              {"class":"TextBlock","name":"RequiredText"},
              {"class":"TextBlock","name":"OptionalImage"}]}})json"),
                  Report, Errors));
  TestTrue(TEXT("the error says why"), ErrorsContain(Errors, TEXT("BindWidgetOptional")));

  TestFalse(TEXT("a missing required binding is refused"),
            Apply(Blueprint, TEXT(R"json({"root":{"class":"CanvasPanel","name":"Root","children":[
              {"class":"Image","name":"OptionalImage"}]}})json"),
                  Report, Errors));
  TestTrue(TEXT("the error names it"), ErrorsContain(Errors, TEXT("RequiredText")));

  TestTrue(TEXT("both bindings with the right classes"),
           Apply(Blueprint, TEXT(R"json({"root":{"class":"CanvasPanel","name":"Root","children":[
             {"class":"TextBlock","name":"RequiredText"},
             {"class":"Image","name":"OptionalImage"}]}})json"),
                 Report, Errors));
  TestEqual(TEXT("no errors"), Errors.Num(), 0);

  TestTrue(TEXT("the optional one may be left out"),
           Apply(Blueprint, TEXT(R"json({"root":{"class":"CanvasPanel","name":"Root","children":[
             {"class":"TextBlock","name":"RequiredText"}]}})json"),
                 Report, Errors));
  return true;
}

// Messages name the node by its path, built only when there is a message.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTSpecPathsTest, FUIWTSpecTestBase,
                                        "UIWidgetTool.WidgetSpec.Paths", TestFlags)

bool FUIWTSpecPathsTest::RunTest(const FString &Parameters)
{
  UWidgetBlueprint *Blueprint = MakeBlueprint();
  if (!TestNotNull(TEXT("blueprint"), Blueprint))
  {
    return false;
  }
  FString Report;
  TArray<FString> Errors;
  TestFalse(TEXT("an unknown property is refused"),
            Apply(Blueprint, TEXT(R"json({"root":{"class":"CanvasPanel","name":"Root","children":[
              {"class":"CanvasPanel","name":"Panel","children":[
                {"class":"TextBlock","name":"Label","props":{"Nope":1}}]}]}})json"),
                  Report, Errors));
  TestTrue(TEXT("the error has the full path"),
           ErrorsContain(Errors, TEXT("Root/Panel/Label: 'Nope' is not a property of TextBlock")));
  return true;
}

#endif
