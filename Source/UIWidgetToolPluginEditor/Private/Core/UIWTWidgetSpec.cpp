#include "UIWTWidgetSpec.h"

#include "Animation/WidgetAnimation.h"
#include "AssetRegistry/AssetData.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
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
    // "Root/Panel/Btn_Play", for messages.
    FString Path;
    FName Name;
    UClass *Class = nullptr;
    TOptional<bool> bVariable;
    TSharedPtr<FJsonObject> Props;
    TSharedPtr<FJsonObject> Slot;
    TArray<FSpecNode> Children;
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

  // Top-level keys of InProps must be properties of InClass.
  void CheckPropertyNames(const TSharedPtr<FJsonObject> &InProps,
                          const UClass *InClass, const FString &InWhere,
                          TArray<FString> &OutErrors)
  {
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : InProps->Values)
    {
      if (!FindFProperty<FProperty>(InClass, FName(*Pair.Key)))
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: '%s' is not a property of %s (ListWidgetProperties "
                 "shows the names)"),
            *InWhere, *Pair.Key, *ClassDisplayName(InClass)));
      }
    }
  }

  bool ParseNode(const TSharedPtr<FJsonObject> &InObject,
                 const FString &InParentPath, bool bInIsRoot, int32 InDepth,
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
    const FString ShownName = Name.IsEmpty() ? FString(TEXT("(unnamed)")) : Name;
    OutNode.Path = InParentPath.IsEmpty() ? ShownName
                                          : InParentPath + TEXT("/") + ShownName;
    const FString &Where = OutNode.Path;

    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : InObject->Values)
    {
      if (!AllowedKeys.Contains(Pair.Key))
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: unknown key '%s' (allowed: class, name, variable, "
                 "props, slot, children)"),
            *Where, *Pair.Key));
      }
    }

    FText NameError;
    if (Name.IsEmpty())
    {
      OutErrors.Add(FString::Printf(TEXT("%s: \"name\" is required"), *Where));
    }
    else if (!FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS,
                                  &NameError))
    {
      OutErrors.Add(FString::Printf(TEXT("%s: invalid name: %s"), *Where,
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
            TEXT("%s: the name is used by more than one widget"), *Where));
      }
    }

    FString ClassName;
    if (!InObject->TryGetStringField(TEXT("class"), ClassName))
    {
      OutErrors.Add(FString::Printf(TEXT("%s: \"class\" is required"), *Where));
    }
    else
    {
      FString ClassError;
      OutNode.Class = ResolveWidgetClass(ClassName, ClassError);
      if (!OutNode.Class)
      {
        OutErrors.Add(FString::Printf(TEXT("%s: %s"), *Where, *ClassError));
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
            TEXT("%s: \"variable\" must be true or false"), *Where));
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
          CheckPropertyNames(OutNode.Props, OutNode.Class, Where, OutErrors);
        }
      }
      else
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: \"props\" must be an object"), *Where));
      }
    }

    const TSharedPtr<FJsonObject> *Slot = nullptr;
    if (InObject->HasField(TEXT("slot")))
    {
      if (bInIsRoot)
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: the root widget has no slot; remove \"slot\""), *Where));
      }
      else if (InObject->TryGetObjectField(TEXT("slot"), Slot))
      {
        OutNode.Slot = *Slot;
      }
      else
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: \"slot\" must be an object"), *Where));
      }
    }

    const TArray<TSharedPtr<FJsonValue>> *Children = nullptr;
    if (InObject->HasField(TEXT("children")))
    {
      if (!InObject->TryGetArrayField(TEXT("children"), Children))
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: \"children\" must be an array"), *Where));
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
              *Where, *ClassDisplayName(OutNode.Class)));
        }
        else if (!Panel->CanHaveMultipleChildren() && Children->Num() > 1)
        {
          OutErrors.Add(FString::Printf(
              TEXT("%s: %s holds one child but %d are given; wrap them in "
                   "a panel such as Overlay or VerticalBox"),
              *Where, *ClassDisplayName(OutNode.Class), Children->Num()));
        }
      }
    }

    if (Children)
    {
      for (int32 Index = 0; Index < Children->Num(); ++Index)
      {
        const TSharedPtr<FJsonObject> *ChildObject = nullptr;
        if (!(*Children)[Index]->TryGetObject(ChildObject))
        {
          OutErrors.Add(FString::Printf(
              TEXT("%s: children[%d] must be an object"), *Where, Index));
          continue;
        }
        FSpecNode &Child = OutNode.Children.AddDefaulted_GetRef();
        if (!ParseNode(*ChildObject, OutNode.Path, false, InDepth + 1, Child,
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

  TSharedPtr<FJsonObject> ParseObject(const FString &InJson)
  {
    TSharedPtr<FJsonObject> Object;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InJson);
    FJsonSerializer::Deserialize(Reader, Object);
    return Object;
  }

  // Sets the spec's properties on InObject, skipping names the object's
  // class does not have (reported, since slot classes are only known here).
  void ApplyProperties(UObject *InObject, const TSharedPtr<FJsonObject> &InProps,
                       const FString &InWhere, TArray<FString> &OutErrors)
  {
    if (!InObject || !InProps.IsValid() || InProps->Values.IsEmpty())
    {
      return;
    }
    TSharedRef<FJsonObject> Known = MakeShared<FJsonObject>();
    TArray<FName> KnownNames;
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : InProps->Values)
    {
      if (FindFProperty<FProperty>(InObject->GetClass(), FName(*Pair.Key)))
      {
        Known->SetField(Pair.Key, Pair.Value);
        KnownNames.Add(FName(*Pair.Key));
      }
      else
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: '%s' is not a property of %s"), *InWhere, *Pair.Key,
            *ClassDisplayName(InObject->GetClass())));
      }
    }
    if (Known->Values.IsEmpty())
    {
      return;
    }
    const TSharedPtr<FJsonObject> Current =
        ParseObject(UToolsetLibrary::GetObjectProperties(InObject, KnownNames));
    CompleteColors(MakeShared<FJsonValueObject>(Known),
                   Current.IsValid() ? MakeShared<FJsonValueObject>(Current)
                                     : TSharedPtr<FJsonValue>());

    FString Json;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
    FJsonSerializer::Serialize(Known, Writer);
    InObject->Modify();
    if (!UToolsetLibrary::SetObjectProperties(InObject, Json))
    {
      OutErrors.Add(FString::Printf(
          TEXT("%s: some properties could not be set (see the property "
               "errors above)"),
          *InWhere));
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
                                       *InNode.Path,
                                       *ClassDisplayName(InNode.Class),
                                       *Error.ToString()));
        return;
      }
      if (!FWidgetBlueprintOperationUtils::VerifyWidgetRename(
              Blueprint, Widget, FText::FromName(InNode.Name), Error))
      {
        FWidgetBlueprintOperationUtils::RemoveTransientWidgetFromTree(
            Blueprint, Widget);
        Ctx.Errors.Add(FString::Printf(TEXT("%s: %s"), *InNode.Path,
                                       *Error.ToString()));
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
    ApplyProperties(Widget->Slot, InNode.Slot, InNode.Path + TEXT(" slot"),
                    Ctx.Errors);
    ApplyProperties(Widget, InNode.Props, InNode.Path, Ctx.Errors);

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


  // Editable properties of InObject that differ from its class default, with
  // struct values reduced to the differing fields. Null when none differ.
  TSharedPtr<FJsonObject> ExportProperties(const UObject *InObject)
  {
    if (!InObject)
    {
      return nullptr;
    }
    const UObject *Defaults = InObject->GetClass()->GetDefaultObject();
    TArray<FName> Names;
    for (TFieldIterator<FProperty> It(InObject->GetClass()); It; ++It)
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
    const TSharedPtr<FJsonObject> DefaultValues =
        ParseObject(UToolsetLibrary::GetObjectProperties(Defaults, Names));
    if (!Values.IsValid())
    {
      return nullptr;
    }
    const TSharedPtr<FJsonValue> Diff = DiffJson(
        MakeShared<FJsonValueObject>(Values),
        DefaultValues.IsValid() ? MakeShared<FJsonValueObject>(DefaultValues)
                                : TSharedPtr<FJsonValue>());
    return Diff.IsValid() ? Diff->AsObject() : nullptr;
  }

  FString ExportClassName(const UClass *InClass)
  {
    const FString Path = InClass->GetPathName();
    return Path.StartsWith(UmgPackagePrefix) ? InClass->GetName() : Path;
  }

  TSharedRef<FJsonObject> ExportNode(const UWidget *InWidget)
  {
    TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
    Node->SetStringField(TEXT("class"), ExportClassName(InWidget->GetClass()));
    Node->SetStringField(TEXT("name"), InWidget->GetName());
    Node->SetBoolField(TEXT("variable"), InWidget->bIsVariable);
    if (TSharedPtr<FJsonObject> Props = ExportProperties(InWidget))
    {
      Node->SetObjectField(TEXT("props"), Props);
    }
    if (TSharedPtr<FJsonObject> Slot = ExportProperties(InWidget->Slot))
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
          Children.Add(MakeShared<FJsonValueObject>(ExportNode(Child)));
        }
      }
      if (!Children.IsEmpty())
      {
        Node->SetArrayField(TEXT("children"), Children);
      }
    }
    return Node;
  }
}

bool UIWTWidgetSpec::Apply(UWidgetBlueprint *InBlueprint,
                           const FString &InSpecJson, FString &OutReport,
                           TArray<FString> &OutErrors)
{
  OutReport.Reset();
  OutErrors.Reset();
  if (!InBlueprint || !InBlueprint->WidgetTree)
  {
    OutErrors.Add(TEXT("no widget blueprint"));
    return false;
  }

  // Parse and validate everything before touching the blueprint.
  const TSharedPtr<FJsonObject> Document = ParseObject(InSpecJson);
  if (!Document.IsValid())
  {
    OutErrors.Add(TEXT("the spec is not a JSON object"));
    return false;
  }
  TSharedPtr<FJsonObject> RootObject = Document;
  if (Document->HasField(TEXT("root")))
  {
    const TSharedPtr<FJsonObject> *Root = nullptr;
    if (!Document->TryGetObjectField(TEXT("root"), Root))
    {
      OutErrors.Add(TEXT("\"root\" must be a widget node object"));
      return false;
    }
    RootObject = *Root;
  }

  FSpecNode Root;
  TSet<FName> Names;
  int32 Count = 0;
  ParseNode(RootObject, FString(), true, 0, Root, Names, Count, OutErrors);
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
          *Pair.Value->Path));
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

  // Refuse deletions that would silently break the blueprint.
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
    for (const UWidgetAnimation *Animation : InBlueprint->Animations)
    {
      if (!Animation)
      {
        continue;
      }
      for (const FWidgetAnimationBinding &Binding : Animation->GetBindings())
      {
        if (Binding.WidgetName == Name)
        {
          OutErrors.Add(FString::Printf(
              TEXT("widget '%s' is animated by '%s', but %s; keep it with "
                   "the same name and class"),
              *Name.ToString(), *Animation->GetName(), *Why));
          break;
        }
      }
    }
  }
  if (InBlueprint->ParentClass)
  {
    for (TFieldIterator<FObjectProperty> It(InBlueprint->ParentClass); It; ++It)
    {
      bool bOptional = false;
      if (!FWidgetBlueprintEditorUtils::IsBindWidgetProperty(*It, bOptional) ||
          bOptional)
      {
        continue;
      }
      const FSpecNode *const *Node = SpecByName.Find(It->GetFName());
      if (!Node)
      {
        OutErrors.Add(FString::Printf(
            TEXT("the parent class requires a %s named '%s' (BindWidget)"),
            *ClassDisplayName(It->PropertyClass), *It->GetName()));
      }
      else if (!(*Node)->Class->IsChildOf(It->PropertyClass))
      {
        OutErrors.Add(FString::Printf(
            TEXT("%s: the parent class binds it as a %s (BindWidget), not a "
                 "%s"),
            *(*Node)->Path, *ClassDisplayName(It->PropertyClass),
            *ClassDisplayName((*Node)->Class)));
      }
    }
  }
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
    Document->SetObjectField(TEXT("root"), ExportNode(Root));
  }
  TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
      TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
  return FJsonSerializer::Serialize(Document, Writer);
}
