#include "UIWTWidgetSpec.h"

#include "Animation/WidgetAnimation.h"
#include "AssetRegistry/AssetData.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Spacer.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ToolsetRegistry/ToolsetLibrary.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditorUtils.h"
#include "WidgetBlueprintOperationUtils.h"

namespace
{
  constexpr int32 MaxDepth = 64;
  constexpr int32 MaxNodes = 4000;

  const TCHAR *const UmgPackagePrefix = TEXT("/Script/UMG.");

  // Editor bookkeeping on UWidget that the spec carries elsewhere ("variable",
  // the slot) or that has no meaning outside the designer.
  const TSet<FName> &SkippedExportProperties()
  {
    static const TSet<FName> Names = {
        TEXT("Slot"), TEXT("bIsVariable"), TEXT("bExpandedInDesigner"),
        TEXT("bLockedInDesigner"), TEXT("bHiddenInDesigner")};
    return Names;
  }

  struct FSpecNode
  {
    // Only for messages; see Path(). Stays valid because each node's
    // Children array is reserved before it is filled.
    const FSpecNode *Parent = nullptr;
    // The name as written, or "(unnamed)".
    FString ShownName;
    FName Name;
    UClass *Class = nullptr;
    TOptional<bool> bVariable;
    TSharedPtr<FJsonObject> Props;
    TSharedPtr<FJsonObject> Slot;
    TArray<FSpecNode> Children;

    // "Root/Panel/Btn_Play". Built only when a message needs it: storing it
    // on every node would cost node count x depth in string building.
    FString Path() const
    {
      TArray<const FSpecNode *, TInlineAllocator<32>> Chain;
      for (const FSpecNode *Node = this; Node; Node = Node->Parent)
      {
        Chain.Add(Node);
      }
      FString Result;
      for (int32 Index = Chain.Num() - 1; Index >= 0; --Index)
      {
        if (!Result.IsEmpty())
        {
          Result += TEXT("/");
        }
        Result += Chain[Index]->ShownName;
      }
      return Result;
    }
  };

  FString ClassDisplayName(const UClass *InClass)
  {
    return InClass ? InClass->GetName() : FString(TEXT("None"));
  }

  UClass *ResolveWidgetClass(const FString &InName, FString &OutError)
  {
    FString Name = InName.TrimStartAndEnd();
    UClass *Class = nullptr;
    if (Name.StartsWith(TEXT("/")))
    {
      Class = LoadObject<UClass>(nullptr, *Name, nullptr,
                                 LOAD_NoWarn | LOAD_Quiet);
      if (!Class)
      {
        // A Widget Blueprint asset path, with or without the object name.
        FString AssetPath = Name;
        if (!AssetPath.Contains(TEXT(".")))
        {
          AssetPath += TEXT(".") + FPackageName::GetShortName(AssetPath);
        }
        if (const UWidgetBlueprint *Blueprint = LoadObject<UWidgetBlueprint>(
                nullptr, *AssetPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
        {
          Class = Blueprint->GeneratedClass;
        }
      }
    }
    else
    {
      Name.RemoveFromStart(TEXT("UMG."));
      for (const TCHAR *Package : {TEXT("/Script/UMG"), TEXT("/Script/CommonUI")})
      {
        Class = FindObject<UClass>(FTopLevelAssetPath(Package, *Name));
        if (!Class && Name.Len() > 1 && Name[0] == TCHAR('U'))
        {
          Class = FindObject<UClass>(FTopLevelAssetPath(Package, *Name.Mid(1)));
        }
        if (Class)
        {
          break;
        }
      }
    }

    if (!Class)
    {
      OutError = FString::Printf(
          TEXT("unknown widget class '%s' (use a UMG class name such as "
               "Button, or a /Script/... or /Game/... path)"),
          *InName);
      return nullptr;
    }
    if (!Class->IsChildOf(UWidget::StaticClass()))
    {
      OutError = FString::Printf(TEXT("'%s' is not a widget class"), *InName);
      return nullptr;
    }
    if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated |
                                CLASS_NewerVersionExists))
    {
      OutError = FString::Printf(
          TEXT("'%s' is abstract or deprecated and cannot be created"),
          *InName);
      return nullptr;
    }
    return Class;
  }

  // Top-level keys of InNode's props must be properties of its class.
  void CheckPropertyNames(const FSpecNode &InNode, TArray<FString> &OutErrors)
  {
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : InNode.Props->Values)
    {
      if (!FindFProperty<FProperty>(InNode.Class, FName(*Pair.Key)))
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: '%s' is not a property of %s (ListWidgetProperties "
                 "shows the names)"),
            *InNode.Path(), *Pair.Key, *ClassDisplayName(InNode.Class)));
      }
    }
  }

  bool ParseNode(const TSharedPtr<FJsonObject> &InObject,
                 const FSpecNode *InParent, bool bInIsRoot, int32 InDepth,
                 FSpecNode &OutNode, TSet<FName> &InOutNames,
                 int32 &InOutCount, TArray<FString> &OutErrors)
  {
    if (InDepth > MaxDepth || ++InOutCount > MaxNodes)
    {
      OutErrors.Add(FString::Printf(
          TEXT("the spec is deeper than %d levels or has more than %d "
               "widgets"),
          MaxDepth, MaxNodes));
      return false;
    }

    static const TSet<FString> AllowedKeys = {
        TEXT("class"), TEXT("name"), TEXT("variable"),
        TEXT("props"), TEXT("slot"), TEXT("children")};

    FString Name;
    InObject->TryGetStringField(TEXT("name"), Name);
    Name.TrimStartAndEndInline();
    OutNode.Parent = InParent;
    OutNode.ShownName = Name.IsEmpty() ? FString(TEXT("(unnamed)")) : Name;

    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : InObject->Values)
    {
      if (!AllowedKeys.Contains(Pair.Key))
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: unknown key '%s' (allowed: class, name, variable, "
                 "props, slot, children)"),
            *OutNode.Path(), *Pair.Key));
      }
    }

    FText NameError;
    if (Name.IsEmpty())
    {
      OutErrors.Add(FString::Printf(TEXT("%s: \"name\" is required"), *OutNode.Path()));
    }
    else if (!FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS,
                                  &NameError))
    {
      OutErrors.Add(FString::Printf(TEXT("%s: invalid name: %s"), *OutNode.Path(),
                                    *NameError.ToString()));
    }
    else
    {
      OutNode.Name = FName(*Name);
      bool bAlreadyUsed = false;
      InOutNames.Add(OutNode.Name, &bAlreadyUsed);
      if (bAlreadyUsed)
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: the name is used by more than one widget"), *OutNode.Path()));
      }
    }

    FString ClassName;
    if (!InObject->TryGetStringField(TEXT("class"), ClassName))
    {
      OutErrors.Add(FString::Printf(TEXT("%s: \"class\" is required"), *OutNode.Path()));
    }
    else
    {
      FString ClassError;
      OutNode.Class = ResolveWidgetClass(ClassName, ClassError);
      if (!OutNode.Class)
      {
        OutErrors.Add(FString::Printf(TEXT("%s: %s"), *OutNode.Path(), *ClassError));
      }
    }

    bool bVariable = false;
    if (InObject->HasField(TEXT("variable")))
    {
      if (InObject->TryGetBoolField(TEXT("variable"), bVariable))
      {
        OutNode.bVariable = bVariable;
      }
      else
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: \"variable\" must be true or false"), *OutNode.Path()));
      }
    }

    const TSharedPtr<FJsonObject> *Props = nullptr;
    if (InObject->HasField(TEXT("props")))
    {
      if (InObject->TryGetObjectField(TEXT("props"), Props))
      {
        OutNode.Props = *Props;
        if (OutNode.Class)
        {
          CheckPropertyNames(OutNode, OutErrors);
        }
      }
      else
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: \"props\" must be an object"), *OutNode.Path()));
      }
    }

    const TSharedPtr<FJsonObject> *Slot = nullptr;
    if (InObject->HasField(TEXT("slot")))
    {
      if (bInIsRoot)
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: the root widget has no slot; remove \"slot\""), *OutNode.Path()));
      }
      else if (InObject->TryGetObjectField(TEXT("slot"), Slot))
      {
        OutNode.Slot = *Slot;
      }
      else
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: \"slot\" must be an object"), *OutNode.Path()));
      }
    }

    const TArray<TSharedPtr<FJsonValue>> *Children = nullptr;
    if (InObject->HasField(TEXT("children")))
    {
      if (!InObject->TryGetArrayField(TEXT("children"), Children))
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: \"children\" must be an array"), *OutNode.Path()));
      }
      else if (Children->Num() > 0 && OutNode.Class)
      {
        const UPanelWidget *Panel =
            Cast<UPanelWidget>(OutNode.Class->GetDefaultObject());
        if (!Panel)
        {
          OutErrors.Add(FString::Printf(
              TEXT("%s: %s cannot have children (only panels such as "
                   "CanvasPanel, Border, Button, Overlay or VerticalBox can)"),
              *OutNode.Path(), *ClassDisplayName(OutNode.Class)));
        }
        else if (!Panel->CanHaveMultipleChildren() && Children->Num() > 1)
        {
          OutErrors.Add(FString::Printf(
              TEXT("%s: %s holds one child but %d are given; wrap them in "
                   "a panel such as Overlay or VerticalBox"),
              *OutNode.Path(), *ClassDisplayName(OutNode.Class), Children->Num()));
        }
      }
    }

    if (Children)
    {
      // Children keep a pointer to their parent: no reallocation from here on.
      OutNode.Children.Reserve(Children->Num());
      for (int32 Index = 0; Index < Children->Num(); ++Index)
      {
        const TSharedPtr<FJsonObject> *ChildObject = nullptr;
        if (!(*Children)[Index]->TryGetObject(ChildObject))
        {
          OutErrors.Add(FString::Printf(
              TEXT("%s: children[%d] must be an object"), *OutNode.Path(), Index));
          continue;
        }
        FSpecNode &Child = OutNode.Children.AddDefaulted_GetRef();
        if (!ParseNode(*ChildObject, &OutNode, false, InDepth + 1, Child,
                       InOutNames, InOutCount, OutErrors) &&
            InOutCount > MaxNodes)
        {
          return false;
        }
      }
    }
    return true;
  }

  void CollectNodes(const FSpecNode &InNode,
                    TMap<FName, const FSpecNode *> &OutByName)
  {
    OutByName.Add(InNode.Name, &InNode);
    for (const FSpecNode &Child : InNode.Children)
    {
      CollectNodes(Child, OutByName);
    }
  }

  // Colour JSON (FLinearColor / FColor): an object whose keys are all
  // channel names.
  bool IsColorObject(const TSharedPtr<FJsonObject> &InObject)
  {
    if (!InObject.IsValid() || InObject->Values.IsEmpty())
    {
      return false;
    }
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : InObject->Values)
    {
      if (Pair.Key.Len() != 1 || !FCString::Strchr(TEXT("rgbaRGBA"), Pair.Key[0]))
      {
        return false;
      }
    }
    return true;
  }

  // The engine's colour converter parses into an uninitialised colour, so a
  // channel missing from the JSON is written as garbage. Gives every colour
  // object in InValue all four channels, taking missing ones from the same
  // path in InCurrent (the property's current value) or else r, g, b = 0 and
  // a = 1.
  void CompleteColors(const TSharedPtr<FJsonValue> &InValue,
                      const TSharedPtr<FJsonValue> &InCurrent)
  {
    if (!InValue.IsValid())
    {
      return;
    }
    const TSharedPtr<FJsonObject> *Object = nullptr;
    const TArray<TSharedPtr<FJsonValue>> *Array = nullptr;
    if (InValue->TryGetObject(Object))
    {
      const TSharedPtr<FJsonObject> *Current = nullptr;
      if (InCurrent.IsValid())
      {
        InCurrent->TryGetObject(Current);
      }
      if (IsColorObject(*Object))
      {
        for (const TCHAR *Channel : {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")})
        {
          if ((*Object)->HasField(Channel))
          {
            continue;
          }
          double Value = Channel[0] == TCHAR('a') ? 1.0 : 0.0;
          if (Current)
          {
            (*Current)->TryGetNumberField(Channel, Value);
          }
          (*Object)->SetNumberField(Channel, Value);
        }
        return;
      }
      for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : (*Object)->Values)
      {
        CompleteColors(Pair.Value,
                       Current ? (*Current)->TryGetField(Pair.Key) : nullptr);
      }
    }
    else if (InValue->TryGetArray(Array))
    {
      const TArray<TSharedPtr<FJsonValue>> *CurrentArray = nullptr;
      if (InCurrent.IsValid())
      {
        InCurrent->TryGetArray(CurrentArray);
      }
      for (int32 Index = 0; Index < Array->Num(); ++Index)
      {
        CompleteColors((*Array)[Index],
                       CurrentArray && CurrentArray->IsValidIndex(Index)
                           ? (*CurrentArray)[Index]
                           : nullptr);
      }
    }
  }

  // True when some colour object in InValue lacks a channel, i.e. when
  // CompleteColors has something to do.
  bool HasIncompleteColor(const TSharedPtr<FJsonValue> &InValue)
  {
    const TSharedPtr<FJsonObject> *Object = nullptr;
    const TArray<TSharedPtr<FJsonValue>> *Array = nullptr;
    if (!InValue.IsValid())
    {
      return false;
    }
    if (InValue->TryGetObject(Object))
    {
      if (IsColorObject(*Object))
      {
        for (const TCHAR *Channel : {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")})
        {
          if (!(*Object)->HasField(Channel))
          {
            return true;
          }
        }
        return false;
      }
      for (const auto &Pair : (*Object)->Values)
      {
        if (HasIncompleteColor(Pair.Value))
        {
          return true;
        }
      }
    }
    else if (InValue->TryGetArray(Array))
    {
      for (const TSharedPtr<FJsonValue> &Element : *Array)
      {
        if (HasIncompleteColor(Element))
        {
          return true;
        }
      }
    }
    return false;
  }

  TSharedPtr<FJsonObject> ParseObject(const FString &InJson)
  {
    TSharedPtr<FJsonObject> Object;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InJson);
    FJsonSerializer::Deserialize(Reader, Object);
    return Object;
  }

  // Sets the spec's properties (InNode's props, or its slot's when bInSlot)
  // on InObject, skipping names the object's class does not have (reported,
  // since slot classes are only known here).
  void ApplyProperties(UObject *InObject, const TSharedPtr<FJsonObject> &InProps,
                       const FSpecNode &InNode, bool bInSlot,
                       TArray<FString> &OutErrors)
  {
    if (!InObject || !InProps.IsValid() || InProps->Values.IsEmpty())
    {
      return;
    }
    auto Where = [&InNode, bInSlot]()
    {
      return bInSlot ? InNode.Path() + TEXT(" slot") : InNode.Path();
    };
    TSharedRef<FJsonObject> Known = MakeShared<FJsonObject>();
    TArray<FName> IncompleteNames;
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : InProps->Values)
    {
      if (FindFProperty<FProperty>(InObject->GetClass(), FName(*Pair.Key)))
      {
        Known->SetField(Pair.Key, Pair.Value);
        if (HasIncompleteColor(Pair.Value))
        {
          IncompleteNames.Add(FName(*Pair.Key));
        }
      }
      else
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: '%s' is not a property of %s"), *Where(), *Pair.Key,
            *ClassDisplayName(InObject->GetClass())));
      }
    }
    if (Known->Values.IsEmpty())
    {
      return;
    }
    // Only a colour with a missing channel needs the current value, so only
    // those properties are read back; every other value is written as given.
    if (!IncompleteNames.IsEmpty())
    {
      // Completing edits the colour objects, which belong to the caller's
      // spec: complete a copy.
      const TSharedPtr<const FJsonObject> Source = Known;
      TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
      FJsonObject::Duplicate(Source, Copy);
      Known = Copy.ToSharedRef();
      const TSharedPtr<FJsonObject> Current =
          ParseObject(UToolsetLibrary::GetObjectProperties(InObject, IncompleteNames));
      CompleteColors(MakeShared<FJsonValueObject>(Known),
                     Current.IsValid() ? MakeShared<FJsonValueObject>(Current)
                                       : TSharedPtr<FJsonValue>());
    }

    FString Json;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
    FJsonSerializer::Serialize(Known, Writer);
    InObject->Modify();
    if (!UToolsetLibrary::SetObjectProperties(InObject, Json))
    {
      OutErrors.Add(FString::Printf(
          TEXT("%s: some properties could not be set (see the property "
               "errors above)"),
          *Where()));
    }
  }

  // What FWidgetBlueprintEditorUtils::DeleteWidgets does for one widget,
  // minus removing graph nodes (Apply refuses to delete widgets the graph
  // uses) and minus marking the blueprint structurally modified.
  void RemoveWidget(UWidgetBlueprint *InBlueprint, UWidget *InWidget)
  {
    const FName Name = InWidget->GetFName();
    for (int32 Index = InBlueprint->Bindings.Num() - 1; Index >= 0; --Index)
    {
      if (InBlueprint->Bindings[Index].ObjectName == Name.ToString())
      {
        InBlueprint->Bindings.RemoveAt(Index);
      }
    }
    if (UPanelWidget *Parent = InWidget->GetParent())
    {
      Parent->SetFlags(RF_Transactional);
      Parent->Modify();
      Parent->RemoveChild(InWidget);
    }
    if (InBlueprint->WidgetTree->RootWidget == InWidget)
    {
      InBlueprint->WidgetTree->RootWidget = nullptr;
    }
    FWidgetBlueprintEditorUtils::ReplaceDesiredFocus(InBlueprint, Name, FName());
    InWidget->SetFlags(RF_Transactional);
    InWidget->Modify();
    // Out of the tree's outer, so a new widget can take the name.
    InWidget->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
    InBlueprint->OnVariableRemoved(Name);
  }

  // Widgets reachable from the root through panel children; anything else
  // in the tree (named-slot content) is outside what a spec describes.
  void CollectPanelTree(UWidget *InWidget, TSet<UWidget *> &OutWidgets)
  {
    if (!InWidget)
    {
      return;
    }
    OutWidgets.Add(InWidget);
    if (const UPanelWidget *Panel = Cast<UPanelWidget>(InWidget))
    {
      for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
      {
        CollectPanelTree(Panel->GetChildAt(Index), OutWidgets);
      }
    }
  }

  struct FBuildContext
  {
    UWidgetBlueprint *Blueprint = nullptr;
    TMap<FName, UWidget *> Kept;
    int32 Created = 0;
    int32 Reused = 0;
    TArray<FString> Errors;
  };

  void BuildNode(FBuildContext &Ctx, const FSpecNode &InNode,
                 UPanelWidget *InParent)
  {
    UWidgetBlueprint *Blueprint = Ctx.Blueprint;
    UWidget *Widget = nullptr;
    if (UWidget *const *Existing = Ctx.Kept.Find(InNode.Name))
    {
      Widget = *Existing;
      if (InParent)
      {
        InParent->AddChild(Widget);
      }
      else
      {
        Blueprint->WidgetTree->RootWidget = Widget;
      }
      ++Ctx.Reused;
    }
    else
    {
      FText Error;
      Widget = FWidgetBlueprintOperationUtils::CreateWidgetFromAsset(
          Blueprint, FAssetData(InNode.Class), Blueprint->WidgetTree, Error);
      if (!Widget)
      {
        Ctx.Errors.Add(FString::Printf(TEXT("%s: could not create %s: %s"),
                                       *InNode.Path(),
                                       *ClassDisplayName(InNode.Class),
                                       *Error.ToString()));
        return;
      }
      // Apply checked the name against the blueprint's names up front, with
      // one validator for the whole spec (see CheckNewNames). What is left is
      // a clash with another object in the widget tree, such as a slot
      // named like the widget; renaming onto it would be fatal.
      if (!Widget->Rename(*InNode.Name.ToString(), nullptr, REN_Test))
      {
        FWidgetBlueprintOperationUtils::RemoveTransientWidgetFromTree(
            Blueprint, Widget);
        Ctx.Errors.Add(FString::Printf(
            TEXT("%s: another object in the widget tree is already named "
                 "'%s'"),
            *InNode.Path(), *InNode.Name.ToString()));
        return;
      }
      Widget->Rename(*InNode.Name.ToString(), nullptr,
                     REN_DontCreateRedirectors);
      if (InParent)
      {
        InParent->AddChild(Widget);
      }
      else
      {
        Blueprint->WidgetTree->RootWidget = Widget;
      }
      Blueprint->OnVariableAdded(InNode.Name);
      ++Ctx.Created;
    }

    if (InNode.bVariable.IsSet())
    {
      FWidgetBlueprintOperationUtils::ToggleWidgetAsVariable(
          Blueprint, Widget, InNode.bVariable.GetValue(), false);
    }
    ApplyProperties(Widget->Slot, InNode.Slot, InNode, true, Ctx.Errors);
    ApplyProperties(Widget, InNode.Props, InNode, false, Ctx.Errors);

    if (UPanelWidget *Panel = Cast<UPanelWidget>(Widget))
    {
      for (const FSpecNode &Child : InNode.Children)
      {
        BuildNode(Ctx, Child, Panel);
      }
    }
  }

  // Null when InValue equals InDefault; otherwise InValue, reduced to the
  // fields that differ when both are objects.
  TSharedPtr<FJsonValue> DiffJson(const TSharedPtr<FJsonValue> &InValue,
                                  const TSharedPtr<FJsonValue> &InDefault)
  {
    if (!InDefault.IsValid())
    {
      return InValue;
    }
    const TSharedPtr<FJsonObject> *ValueObject = nullptr;
    const TSharedPtr<FJsonObject> *DefaultObject = nullptr;
    // Colours stay whole: a partial colour is easy to misread and must be
    // completed before it can be set.
    if (InValue->TryGetObject(ValueObject) && !IsColorObject(*ValueObject) &&
        InDefault->TryGetObject(DefaultObject))
    {
      TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
      for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
           (*ValueObject)->Values)
      {
        TSharedPtr<FJsonValue> Diff =
            DiffJson(Pair.Value, (*DefaultObject)->TryGetField(Pair.Key));
        if (Diff.IsValid())
        {
          Result->SetField(Pair.Key, Diff);
        }
      }
      if (Result->Values.IsEmpty())
      {
        return nullptr;
      }
      return MakeShared<FJsonValueObject>(Result);
    }
    return FJsonValue::CompareEqual(*InValue, *InDefault) ? nullptr : InValue;
  }


  // Numbers as Export writes them. UMG stores almost every number as a
  // float, and the property JSON prints it back as a double with 17
  // significant digits (0.1 becomes 0.10000000149011612): noise that costs
  // tokens on every read and hides the value that was typed. Whole numbers
  // are written exactly, since integer properties must not pass through a
  // float; other numbers in the shortest form that reads back as the same
  // float. Colour channels get 5 decimals: far below what anyone can see,
  // and still exact for FColor channels (n / 255), which the property JSON
  // also writes as 0-1 values.
  FString FormatExportNumber(double InValue, bool bInColorChannel)
  {
    if (bInColorChannel)
    {
      FString Text = FString::Printf(TEXT("%.5f"), InValue);
      while (Text.EndsWith(TEXT("0")))
      {
        Text.LeftChopInline(1);
      }
      if (Text.EndsWith(TEXT(".")))
      {
        Text.LeftChopInline(1);
      }
      return Text == TEXT("-0") ? FString(TEXT("0")) : Text;
    }
    if (InValue == 0.0)
    {
      return TEXT("0");
    }
    if (FMath::Abs(InValue) < 9007199254740992.0 &&
        InValue == FMath::FloorToDouble(InValue))
    {
      return FString::Printf(TEXT("%.0f"), InValue);
    }
    const float AsFloat = static_cast<float>(InValue);
    for (int32 Digits = 1; Digits < 9; ++Digits)
    {
      const FString Text = FString::Printf(TEXT("%.*g"), Digits, InValue);
      if (static_cast<float>(FCString::Atod(*Text)) == AsFloat)
      {
        return Text;
      }
    }
    return FString::Printf(TEXT("%.9g"), InValue);
  }

  // InValue with every number rewritten by FormatExportNumber. Objects are
  // changed in place; arrays, whose elements FJsonValue doesn't let us
  // replace, are rebuilt.
  TSharedPtr<FJsonValue> RoundExportNumbers(const TSharedPtr<FJsonValue> &InValue,
                                            bool bInColorChannel = false)
  {
    if (!InValue.IsValid())
    {
      return InValue;
    }
    switch (InValue->Type)
    {
    case EJson::Number:
    {
      double Number = 0.0;
      if (!InValue->TryGetNumber(Number) || !FMath::IsFinite(Number))
      {
        return InValue;
      }
      return MakeShared<FJsonValueNumberString>(
          FormatExportNumber(Number, bInColorChannel));
    }
    case EJson::Object:
    {
      const TSharedPtr<FJsonObject> Object = InValue->AsObject();
      const bool bColor = IsColorObject(Object);
      for (auto &Pair : Object->Values)
      {
        Pair.Value = RoundExportNumbers(Pair.Value, bColor);
      }
      return InValue;
    }
    case EJson::Array:
    {
      TArray<TSharedPtr<FJsonValue>> Elements = InValue->AsArray();
      for (TSharedPtr<FJsonValue> &Element : Elements)
      {
        Element = RoundExportNumbers(Element);
      }
      return MakeShared<FJsonValueArray>(Elements);
    }
    default:
      return InValue;
    }
  }

  // What Export needs per class, worked out once per Export instead of once
  // per widget or slot: the properties worth exporting, and the class
  // default's JSON for each, read the first time an object of the class
  // needs it. Class defaults don't change during an Export.
  struct FExportClassInfo
  {
    TArray<const FProperty *> Properties;
    TMap<FName, TSharedPtr<FJsonValue>> Defaults;
  };

  struct FExportContext
  {
    TMap<const UClass *, FExportClassInfo> Classes;
  };

  FExportClassInfo &GetExportClassInfo(FExportContext &Ctx, const UClass *InClass)
  {
    if (FExportClassInfo *Found = Ctx.Classes.Find(InClass))
    {
      return *Found;
    }
    FExportClassInfo &Info = Ctx.Classes.Add(InClass);
    for (TFieldIterator<FProperty> It(InClass); It; ++It)
    {
      const FProperty *Property = *It;
      if (!Property->HasAnyPropertyFlags(CPF_Edit) ||
          Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated |
                                        CPF_EditConst |
                                        CPF_InstancedReference |
                                        CPF_PersistentInstance) ||
          Property->IsA<FDelegateProperty>() ||
          Property->IsA<FMulticastDelegateProperty>() ||
          SkippedExportProperties().Contains(Property->GetFName()))
      {
        continue;
      }
      Info.Properties.Add(Property);
    }
    return Info;
  }

  // Editable properties of InObject that differ from its class default, with
  // struct values reduced to the differing fields and numbers as
  // FormatExportNumber writes them. Null when none differ.
  TSharedPtr<FJsonObject> ExportProperties(FExportContext &Ctx,
                                           const UObject *InObject)
  {
    if (!InObject)
    {
      return nullptr;
    }
    const UObject *Defaults = InObject->GetClass()->GetDefaultObject();
    FExportClassInfo &Info = GetExportClassInfo(Ctx, InObject->GetClass());
    TArray<FName> Names;
    for (const FProperty *Property : Info.Properties)
    {
      bool bIdentical = true;
      for (int32 Index = 0; Index < Property->ArrayDim && bIdentical; ++Index)
      {
        bIdentical = Property->Identical_InContainer(InObject, Defaults, Index);
      }
      if (!bIdentical)
      {
        Names.Add(Property->GetFName());
      }
    }
    if (Names.IsEmpty())
    {
      return nullptr;
    }

    const TSharedPtr<FJsonObject> Values =
        ParseObject(UToolsetLibrary::GetObjectProperties(InObject, Names));
    if (!Values.IsValid())
    {
      return nullptr;
    }
    TArray<FName> Missing;
    for (const FName Name : Names)
    {
      if (!Info.Defaults.Contains(Name))
      {
        Missing.Add(Name);
      }
    }
    if (!Missing.IsEmpty())
    {
      const TSharedPtr<FJsonObject> Read =
          ParseObject(UToolsetLibrary::GetObjectProperties(Defaults, Missing));
      for (const FName Name : Missing)
      {
        Info.Defaults.Add(Name, Read.IsValid() ? Read->TryGetField(Name.ToString())
                                               : TSharedPtr<FJsonValue>());
      }
    }
    TSharedRef<FJsonObject> DefaultValues = MakeShared<FJsonObject>();
    for (const FName Name : Names)
    {
      if (const TSharedPtr<FJsonValue> &Default = Info.Defaults.FindChecked(Name))
      {
        DefaultValues->SetField(Name.ToString(), Default);
      }
    }
    const TSharedPtr<FJsonValue> Diff =
        DiffJson(MakeShared<FJsonValueObject>(Values),
                 MakeShared<FJsonValueObject>(DefaultValues));
    return Diff.IsValid() ? RoundExportNumbers(Diff)->AsObject() : nullptr;
  }

  FString ExportClassName(const UClass *InClass)
  {
    const FString Path = InClass->GetPathName();
    return Path.StartsWith(UmgPackagePrefix) ? InClass->GetName() : Path;
  }

  TSharedRef<FJsonObject> ExportNode(FExportContext &Ctx, const UWidget *InWidget)
  {
    TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
    Node->SetStringField(TEXT("class"), ExportClassName(InWidget->GetClass()));
    Node->SetStringField(TEXT("name"), InWidget->GetName());
    Node->SetBoolField(TEXT("variable"), InWidget->bIsVariable);
    if (TSharedPtr<FJsonObject> Props = ExportProperties(Ctx, InWidget))
    {
      Node->SetObjectField(TEXT("props"), Props);
    }
    if (TSharedPtr<FJsonObject> Slot = ExportProperties(Ctx, InWidget->Slot))
    {
      Node->SetObjectField(TEXT("slot"), Slot);
    }
    if (const UPanelWidget *Panel = Cast<UPanelWidget>(InWidget))
    {
      TArray<TSharedPtr<FJsonValue>> Children;
      for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
      {
        if (const UWidget *Child = Panel->GetChildAt(Index))
        {
          Children.Add(MakeShared<FJsonValueObject>(ExportNode(Ctx, Child)));
        }
      }
      if (!Children.IsEmpty())
      {
        Node->SetArrayField(TEXT("children"), Children);
      }
    }
    return Node;
  }

  // Names of the widgets the spec creates must not clash with anything else
  // the blueprint names: its variables and those of every parent class
  // (Visibility, Padding...), its functions, graphs and components. One
  // validator checks them all, before anything is changed.
  // VerifyWidgetRename, used before, built a new validator and walked the
  // whole widget tree for every widget, so creating N widgets cost about
  // N^2, and it only ran mid-build, after the blueprint had been changed.
  // ParseNode has already rejected every character VerifyWidgetRename's
  // SanitizeWidgetName would replace, and the validator checks the length.
  void CheckNewNames(UWidgetBlueprint *InBlueprint,
                     const TMap<FName, const FSpecNode *> &InSpecByName,
                     const TMap<FName, UWidget *> &InKept,
                     const TArray<UWidget *> &InExisting,
                     TArray<FString> &OutErrors)
  {
    TSet<FName> ExistingNames;
    ExistingNames.Reserve(InExisting.Num());
    for (const UWidget *Widget : InExisting)
    {
      ExistingNames.Add(Widget->GetFName());
    }
    FKismetNameValidator Validator(InBlueprint);
    for (const TPair<FName, const FSpecNode *> &Pair : InSpecByName)
    {
      const FName Name = Pair.Key;
      if (InKept.Contains(Name))
      {
        continue;
      }
      // A widget of another class with the same name is being replaced. Its
      // variable stays in the skeleton class until the compile, so the
      // validator would call the name taken; the compile moves it over.
      if (ExistingNames.Contains(Name))
      {
        continue;
      }
      // A parent-class BindWidget property shares the widget's name on
      // purpose; Apply checks its class.
      if (InBlueprint->ParentClass)
      {
        const FProperty *Property =
            InBlueprint->ParentClass->FindPropertyByName(Name);
        if (Property && FWidgetBlueprintEditorUtils::IsBindWidgetProperty(Property))
        {
          continue;
        }
      }
      const FString NameString = Name.ToString();
      const EValidatorResult Result = Validator.IsValid(NameString);
      if (Result != EValidatorResult::Ok)
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: %s Choose another name."), *Pair.Value->Path(),
            *INameValidatorInterface::GetErrorText(NameString, Result).ToString()));
      }
    }
  }
}

bool UIWTWidgetSpec::Apply(UWidgetBlueprint *InBlueprint,
                           const FString &InSpecJson, FString &OutReport,
                           TArray<FString> &OutErrors)
{
  OutReport.Reset();
  OutErrors.Reset();
  const TSharedPtr<FJsonObject> Document = ParseObject(InSpecJson);
  if (!Document.IsValid())
  {
    OutErrors.Add(TEXT("the spec is not a JSON object"));
    return false;
  }
  return Apply(InBlueprint, Document.ToSharedRef(), OutReport, OutErrors);
}

bool UIWTWidgetSpec::Apply(UWidgetBlueprint *InBlueprint,
                           const TSharedRef<FJsonObject> &InSpec,
                           FString &OutReport, TArray<FString> &OutErrors)
{
  OutReport.Reset();
  OutErrors.Reset();
  if (!InBlueprint || !InBlueprint->WidgetTree)
  {
    OutErrors.Add(TEXT("no widget blueprint"));
    return false;
  }

  // Parse and validate everything before touching the blueprint.
  TSharedPtr<FJsonObject> RootObject = InSpec;
  if (InSpec->HasField(TEXT("root")))
  {
    const TSharedPtr<FJsonObject> *Root = nullptr;
    if (!InSpec->TryGetObjectField(TEXT("root"), Root))
    {
      OutErrors.Add(TEXT("\"root\" must be a widget node object"));
      return false;
    }
    RootObject = *Root;
  }

  FSpecNode Root;
  TSet<FName> Names;
  int32 Count = 0;
  ParseNode(RootObject, nullptr, true, 0, Root, Names, Count, OutErrors);
  if (!OutErrors.IsEmpty())
  {
    return false;
  }
  TMap<FName, const FSpecNode *> SpecByName;
  CollectNodes(Root, SpecByName);

  for (const TPair<FName, const FSpecNode *> &Pair : SpecByName)
  {
    if (Pair.Value->Class->ClassGeneratedBy == InBlueprint)
    {
      OutErrors.Add(FString::Printf(
          TEXT("%s: a widget blueprint cannot contain itself"),
          *Pair.Value->Path()));
    }
  }

  // Existing widgets whose name and class match are kept; the rest go.
  TArray<UWidget *> Existing;
  InBlueprint->WidgetTree->GetAllWidgets(Existing);
  TSet<UWidget *> InPanelTree;
  CollectPanelTree(InBlueprint->WidgetTree->RootWidget, InPanelTree);
  for (const UWidget *Widget : Existing)
  {
    if (!InPanelTree.Contains(Widget))
    {
      OutErrors.Add(FString::Printf(
          TEXT("widget '%s' is named-slot content, which a spec cannot "
               "describe; edit this blueprint with the UMGToolSet tools "
               "instead"),
          *Widget->GetName()));
    }
  }
  if (!OutErrors.IsEmpty())
  {
    return false;
  }
  FBuildContext Ctx;
  Ctx.Blueprint = InBlueprint;
  TSet<UWidget *> Removed;
  for (UWidget *Widget : Existing)
  {
    const FSpecNode *const *Node = SpecByName.Find(Widget->GetFName());
    if (Node && (*Node)->Class == Widget->GetClass())
    {
      Ctx.Kept.Add(Widget->GetFName(), Widget);
    }
    else
    {
      Removed.Add(Widget);
    }
  }

  // Refuse deletions that would silently break the blueprint. What the
  // animations and property bindings use is collected once instead of
  // scanned again for every removed widget.
  TMultiMap<FName, FString> AnimatedBy;
  TMultiMap<FName, FName> BoundProperties;
  if (!Removed.IsEmpty())
  {
    for (const UWidgetAnimation *Animation : InBlueprint->Animations)
    {
      if (!Animation)
      {
        continue;
      }
      for (const FWidgetAnimationBinding &Binding : Animation->GetBindings())
      {
        AnimatedBy.AddUnique(Binding.WidgetName, Animation->GetName());
      }
    }
    for (const FDelegateEditorBinding &Binding : InBlueprint->Bindings)
    {
      BoundProperties.Add(FName(*Binding.ObjectName), Binding.PropertyName);
    }
  }
  for (const UWidget *Widget : Removed)
  {
    const FName Name = Widget->GetFName();
    const FString Why = SpecByName.Contains(Name)
                            ? FString::Printf(TEXT("the spec changes its class from %s"),
                                              *ClassDisplayName(Widget->GetClass()))
                            : FString(TEXT("the spec leaves it out"));
    if (FBlueprintEditorUtils::IsVariableUsed(InBlueprint, Name))
    {
      OutErrors.Add(FString::Printf(
          TEXT("widget '%s' is used by the blueprint's graph, but %s; keep "
               "it with the same name and class"),
          *Name.ToString(), *Why));
    }
    TArray<FString> Animations;
    AnimatedBy.MultiFind(Name, Animations, true);
    for (const FString &Animation : Animations)
    {
      OutErrors.Add(FString::Printf(
          TEXT("widget '%s' is animated by '%s', but %s; keep it with "
               "the same name and class"),
          *Name.ToString(), *Animation, *Why));
    }
    TArray<FName> Properties;
    BoundProperties.MultiFind(Name, Properties, true);
    for (const FName Property : Properties)
    {
      OutErrors.Add(FString::Printf(
          TEXT("widget '%s' has a property binding on %s, but %s; keep it "
               "with the same name and class"),
          *Name.ToString(), *Property.ToString(), *Why));
    }
  }
  // Parent-class BindWidget properties: a required one needs a widget of
  // its name, and any widget with the name of one must be of its class. An
  // optional one may be left out; its property then stays null.
  if (InBlueprint->ParentClass)
  {
    for (TFieldIterator<FObjectProperty> It(InBlueprint->ParentClass); It; ++It)
    {
      bool bOptional = false;
      if (!FWidgetBlueprintEditorUtils::IsBindWidgetProperty(*It, bOptional))
      {
        continue;
      }
      const TCHAR *Kind = bOptional ? TEXT("BindWidgetOptional") : TEXT("BindWidget");
      const FSpecNode *const *Node = SpecByName.Find(It->GetFName());
      if (!Node)
      {
        if (!bOptional)
        {
          OutErrors.Add(FString::Printf(
              TEXT("the parent class requires a %s named '%s' (BindWidget)"),
              *ClassDisplayName(It->PropertyClass), *It->GetName()));
        }
      }
      else if (!(*Node)->Class->IsChildOf(It->PropertyClass))
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: the parent class binds it as a %s (%s), not a %s"),
            *(*Node)->Path(), *ClassDisplayName(It->PropertyClass), Kind,
            *ClassDisplayName((*Node)->Class)));
      }
    }
  }
  CheckNewNames(InBlueprint, SpecByName, Ctx.Kept, Existing, OutErrors);
  if (!OutErrors.IsEmpty())
  {
    return false;
  }

  // Build with raw tree operations and mark the blueprint structurally
  // modified once at the end: that mark compiles, and a compile moves any
  // widget that is detached at that moment (kept ones, mid re-parenting) to
  // the transient package. Kept widgets are detached first so removing an
  // old parent cannot take them along.
  InBlueprint->Modify();
  InBlueprint->WidgetTree->SetFlags(RF_Transactional);
  InBlueprint->WidgetTree->Modify();
  for (const TPair<FName, UWidget *> &Pair : Ctx.Kept)
  {
    UWidget *Widget = Pair.Value;
    Widget->SetFlags(RF_Transactional);
    Widget->Modify();
    if (UPanelWidget *Parent = Widget->GetParent())
    {
      Parent->SetFlags(RF_Transactional);
      Parent->Modify();
      Parent->RemoveChild(Widget);
    }
  }
  for (UWidget *Widget : Removed)
  {
    RemoveWidget(InBlueprint, Widget);
  }
  InBlueprint->WidgetTree->RootWidget = nullptr;

  BuildNode(Ctx, Root, nullptr);
  FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(InBlueprint);

  FCompilerResultsLog Results;
  FKismetEditorUtilities::CompileBlueprint(
      InBlueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);
  TArray<FString> CompileErrors;
  for (const TSharedRef<FTokenizedMessage> &Message : Results.Messages)
  {
    if (Message->GetSeverity() == EMessageSeverity::Error)
    {
      CompileErrors.Add(Message->ToText().ToString());
    }
  }

  OutErrors = MoveTemp(Ctx.Errors);
  OutReport = FString::Printf(
      TEXT("Applied the spec: %d widgets (%d created, %d kept, %d removed)."),
      Ctx.Created + Ctx.Reused, Ctx.Created, Ctx.Reused, Removed.Num());
  if (InBlueprint->Status == BS_Error)
  {
    OutReport += TEXT(" Compile FAILED:");
    for (const FString &Error : CompileErrors)
    {
      OutReport += TEXT("\n- ") + Error;
    }
  }
  else
  {
    OutReport += TEXT(" Compiled cleanly.");
  }
  return true;
}

bool UIWTWidgetSpec::Export(UWidgetBlueprint *InBlueprint, FString &OutJson,
                            FString &OutError)
{
  OutJson.Reset();
  if (!InBlueprint || !InBlueprint->WidgetTree)
  {
    OutError = TEXT("no widget blueprint");
    return false;
  }
  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
  if (const UWidget *Root = InBlueprint->WidgetTree->RootWidget)
  {
    FExportContext Ctx;
    Document->SetObjectField(TEXT("root"), ExportNode(Ctx, Root));
  }
  TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
      TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
  return FJsonSerializer::Serialize(Document, Writer);
}

namespace
{
  void CollectSpecNames(const TSharedPtr<FJsonObject> &InNode, TSet<FString> &OutNames)
  {
    FString Name;
    if (!InNode.IsValid() || !InNode->TryGetStringField(TEXT("name"), Name))
    {
      return;
    }
    OutNames.Add(Name);
    const TArray<TSharedPtr<FJsonValue>> *Children = nullptr;
    if (InNode->TryGetArrayField(TEXT("children"), Children))
    {
      for (const TSharedPtr<FJsonValue> &Child : *Children)
      {
        CollectSpecNames(Child->AsObject(), OutNames);
      }
    }
  }

  // The node named InName under InNode, with the node holding it in its
  // children (null for InNode itself) and its index there.
  bool FindSpecNode(const TSharedPtr<FJsonObject> &InNode, const FString &InName,
                    TSharedPtr<FJsonObject> &OutNode, TSharedPtr<FJsonObject> &OutParent,
                    int32 &OutIndex)
  {
    FString Name;
    if (!InNode.IsValid() || !InNode->TryGetStringField(TEXT("name"), Name))
    {
      return false;
    }
    if (Name == InName)
    {
      OutNode = InNode;
      return true;
    }
    const TArray<TSharedPtr<FJsonValue>> *Children = nullptr;
    if (InNode->TryGetArrayField(TEXT("children"), Children))
    {
      for (int32 Index = 0; Index < Children->Num(); ++Index)
      {
        const TSharedPtr<FJsonObject> Child = (*Children)[Index]->AsObject();
        if (FindSpecNode(Child, InName, OutNode, OutParent, OutIndex))
        {
          if (!OutParent.IsValid())
          {
            OutParent = InNode;
            OutIndex = Index;
          }
          return true;
        }
      }
    }
    return false;
  }
}

bool UIWTWidgetSpec::ExportSubtree(UWidgetBlueprint *InBlueprint, const FString &InWidgetName,
                                   FString &OutJson, FString &OutError)
{
  OutJson.Reset();
  if (!InBlueprint || !InBlueprint->WidgetTree)
  {
    OutError = TEXT("no widget blueprint");
    return false;
  }
  const UWidget *Widget = InBlueprint->WidgetTree->FindWidget(FName(*InWidgetName));
  if (!Widget)
  {
    OutError = FString::Printf(TEXT("the blueprint has no widget named %s"), *InWidgetName);
    return false;
  }
  FExportContext Ctx;
  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
  Document->SetObjectField(TEXT("root"), ExportNode(Ctx, Widget));
  TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
      TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
  return FJsonSerializer::Serialize(Document, Writer);
}

// The subtree is spliced into the whole tree's export and applied with
// Apply, so its rules (and refusals) are exactly Apply's; the widgets
// outside get back the values Export gave for them.
bool UIWTWidgetSpec::ApplySubtree(UWidgetBlueprint *InBlueprint, const FString &InWidgetName,
                                  const FString &InSpecJson, FString &OutReport,
                                  TArray<FString> &OutErrors)
{
  OutReport.Reset();
  OutErrors.Reset();
  if (!InBlueprint || !InBlueprint->WidgetTree || !InBlueprint->WidgetTree->RootWidget)
  {
    OutErrors.Add(TEXT("the blueprint has no widget tree; use ApplyWidgetSpec"));
    return false;
  }
  const TSharedPtr<FJsonObject> Document = ParseObject(InSpecJson);
  if (!Document.IsValid())
  {
    OutErrors.Add(TEXT("the spec is not a JSON object"));
    return false;
  }
  TSharedPtr<FJsonObject> NewRoot = Document;
  if (Document->HasField(TEXT("root")))
  {
    const TSharedPtr<FJsonObject> *Root = nullptr;
    if (!Document->TryGetObjectField(TEXT("root"), Root))
    {
      OutErrors.Add(TEXT("\"root\" must be a widget node"));
      return false;
    }
    NewRoot = *Root;
  }

  FExportContext Ctx;
  const TSharedRef<FJsonObject> Whole = ExportNode(Ctx, InBlueprint->WidgetTree->RootWidget);
  TSharedPtr<FJsonObject> OldNode;
  TSharedPtr<FJsonObject> Parent;
  int32 Index = INDEX_NONE;
  if (!FindSpecNode(Whole, InWidgetName, OldNode, Parent, Index))
  {
    OutErrors.Add(FString::Printf(TEXT("the blueprint has no widget named %s"), *InWidgetName));
    return false;
  }

  // Names outside the subtree stay taken.
  TSet<FString> Outside;
  CollectSpecNames(Whole, Outside);
  TSet<FString> Inside;
  CollectSpecNames(OldNode, Inside);
  Outside = Outside.Difference(Inside);
  TSet<FString> NewNames;
  CollectSpecNames(NewRoot, NewNames);
  for (const FString &Name : NewNames)
  {
    if (Outside.Contains(Name))
    {
      OutErrors.Add(FString::Printf(
          TEXT("%s is a widget outside %s: a subtree spec can't use its name or move it "
               "in; apply the common parent's subtree instead"),
          *Name, *InWidgetName));
    }
  }
  if (!OutErrors.IsEmpty())
  {
    return false;
  }

  // A copy, so the caller's spec isn't changed.
  const TSharedRef<FJsonObject> Spliced = MakeShared<FJsonObject>(*NewRoot);
  const TSharedPtr<FJsonObject> *OldSlot = nullptr;
  if (!Spliced->HasField(TEXT("slot")) && OldNode->TryGetObjectField(TEXT("slot"), OldSlot))
  {
    Spliced->SetObjectField(TEXT("slot"), *OldSlot);
  }
  TSharedPtr<FJsonObject> WholeRoot = Whole;
  if (Parent.IsValid())
  {
    TArray<TSharedPtr<FJsonValue>> Children = Parent->GetArrayField(TEXT("children"));
    Children[Index] = MakeShared<FJsonValueObject>(Spliced);
    Parent->SetArrayField(TEXT("children"), Children);
  }
  else
  {
    WholeRoot = Spliced;
  }
  TSharedRef<FJsonObject> Spec = MakeShared<FJsonObject>();
  Spec->SetObjectField(TEXT("root"), WholeRoot);
  return Apply(InBlueprint, Spec, OutReport, OutErrors);
}

TSharedPtr<FJsonValue> UIWTWidgetSpec::NormalizeValue(const TSharedPtr<FJsonValue> &InValue)
{
  // RoundExportNumbers changes objects in place, so it gets a deep copy.
  return InValue.IsValid() ? RoundExportNumbers(FJsonValue::Duplicate(InValue)) : InValue;
}

// ---------------------------------------------------------------------------
// FClassInfo

struct UIWTWidgetSpec::FClassInfo::FImpl
{
  FExportContext Export;
  TMap<FString, UClass *> Classes;
  TMap<const UClass *, UClass *> Slots;
  TMap<const UClass *, TSet<FString>> Exported;
  TMap<TPair<const UClass *, FString>, TSharedPtr<FJsonValue>> Defaults;
};

UIWTWidgetSpec::FClassInfo::FClassInfo() : Impl(MakeUnique<FImpl>()) {}

UIWTWidgetSpec::FClassInfo::~FClassInfo() = default;

UClass *UIWTWidgetSpec::FClassInfo::ResolveClass(const FString &InSpecClass)
{
  if (UClass *const *Found = Impl->Classes.Find(InSpecClass))
  {
    return *Found;
  }
  FString Error;
  UClass *Class = ResolveWidgetClass(InSpecClass, Error);
  Impl->Classes.Add(InSpecClass, Class);
  return Class;
}

UClass *UIWTWidgetSpec::FClassInfo::SlotClass(UClass *InPanelClass)
{
  if (!IsPanel(InPanelClass))
  {
    return nullptr;
  }
  if (UClass *const *Found = Impl->Slots.Find(InPanelClass))
  {
    return *Found;
  }
  // UPanelWidget::GetSlotClass is protected: ask a throwaway panel instead.
  UPanelWidget *Panel = NewObject<UPanelWidget>(GetTransientPackage(), InPanelClass, NAME_None,
                                                RF_Transient);
  UWidget *Child = NewObject<USpacer>(GetTransientPackage(), NAME_None, RF_Transient);
  const UPanelSlot *Slot = Panel->AddChild(Child);
  UClass *SlotClass = Slot ? Slot->GetClass() : nullptr;
  Panel->ClearChildren();
  Panel->MarkAsGarbage();
  Child->MarkAsGarbage();
  Impl->Slots.Add(InPanelClass, SlotClass);
  return SlotClass;
}

bool UIWTWidgetSpec::FClassInfo::IsPanel(const UClass *InClass) const
{
  return InClass && InClass->IsChildOf(UPanelWidget::StaticClass()) &&
         !InClass->HasAnyClassFlags(CLASS_Abstract);
}

bool UIWTWidgetSpec::FClassInfo::CanHaveMultipleChildren(const UClass *InClass) const
{
  const UPanelWidget *Panel =
      IsPanel(InClass) ? Cast<UPanelWidget>(InClass->GetDefaultObject()) : nullptr;
  return Panel && Panel->CanHaveMultipleChildren();
}

bool UIWTWidgetSpec::FClassInfo::HasProperty(const UClass *InClass,
                                             const FString &InProperty) const
{
  return InClass && FindFProperty<FProperty>(InClass, FName(*InProperty));
}

bool UIWTWidgetSpec::FClassInfo::IsExported(const UClass *InClass, const FString &InProperty)
{
  if (!InClass)
  {
    return false;
  }
  TSet<FString> *Names = Impl->Exported.Find(InClass);
  if (!Names)
  {
    Names = &Impl->Exported.Add(InClass);
    for (const FProperty *Property : GetExportClassInfo(Impl->Export, InClass).Properties)
    {
      Names->Add(Property->GetName());
    }
  }
  return Names->Contains(InProperty);
}

TSharedPtr<FJsonValue> UIWTWidgetSpec::FClassInfo::Default(const UClass *InClass,
                                                           const FString &InProperty)
{
  const TPair<const UClass *, FString> Key(InClass, InProperty);
  if (const TSharedPtr<FJsonValue> *Found = Impl->Defaults.Find(Key))
  {
    return *Found;
  }
  TSharedPtr<FJsonValue> Value;
  if (IsExported(InClass, InProperty))
  {
    const TSharedPtr<FJsonObject> Read = ParseObject(UToolsetLibrary::GetObjectProperties(
        InClass->GetDefaultObject(), {FName(*InProperty)}));
    Value = NormalizeValue(Read.IsValid() ? Read->TryGetField(InProperty) : nullptr);
  }
  Impl->Defaults.Add(Key, Value);
  return Value;
}

TArray<UIWTWidgetSpec::FProtectedWidget>
UIWTWidgetSpec::FindProtectedWidgets(UWidgetBlueprint *InBlueprint)
{
  TArray<FProtectedWidget> Result;
  if (!InBlueprint || !InBlueprint->WidgetTree)
  {
    return Result;
  }

  // Everything below is keyed by name; collect it once, then walk the tree.
  TMap<FName, TArray<FString>> AnimationsByWidget;
  for (const UWidgetAnimation *Animation : InBlueprint->Animations)
  {
    if (!Animation)
    {
      continue;
    }
    for (const FWidgetAnimationBinding &Binding : Animation->GetBindings())
    {
      AnimationsByWidget.FindOrAdd(Binding.WidgetName)
          .AddUnique(FString::Printf(TEXT("animation %s"),
                                     *Animation->GetName()));
    }
  }
  TMap<FName, TArray<FString>> PropertyBindingsByWidget;
  for (const FDelegateEditorBinding &Binding : InBlueprint->Bindings)
  {
    PropertyBindingsByWidget.FindOrAdd(FName(*Binding.ObjectName))
        .AddUnique(FString::Printf(TEXT("property binding %s"),
                                   *Binding.PropertyName.ToString()));
  }
  TMap<FName, FString> BindWidgets;
  if (InBlueprint->ParentClass)
  {
    for (TFieldIterator<FObjectProperty> It(InBlueprint->ParentClass); It; ++It)
    {
      bool bOptional = false;
      if (FWidgetBlueprintEditorUtils::IsBindWidgetProperty(*It, bOptional))
      {
        BindWidgets.Add(It->GetFName(), bOptional ? TEXT("BindWidgetOptional")
                                                  : TEXT("BindWidget"));
      }
    }
  }

  TArray<UWidget *> Widgets;
  InBlueprint->WidgetTree->GetAllWidgets(Widgets);
  for (const UWidget *Widget : Widgets)
  {
    const FName Name = Widget->GetFName();
    TArray<FString> Reasons;
    if (FBlueprintEditorUtils::IsVariableUsed(InBlueprint, Name))
    {
      Reasons.Add(TEXT("graph"));
    }
    if (const TArray<FString> *Found = AnimationsByWidget.Find(Name))
    {
      Reasons.Append(*Found);
    }
    if (const TArray<FString> *Found = PropertyBindingsByWidget.Find(Name))
    {
      Reasons.Append(*Found);
    }
    if (const FString *Found = BindWidgets.Find(Name))
    {
      Reasons.Add(*Found);
    }
    if (!Reasons.IsEmpty())
    {
      Result.Add({Name, Widget->GetClass(), MoveTemp(Reasons)});
    }
  }
  return Result;
}
