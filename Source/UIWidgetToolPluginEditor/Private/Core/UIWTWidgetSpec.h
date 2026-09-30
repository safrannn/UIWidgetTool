#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UWidgetBlueprint;

// A whole widget tree as one JSON document, so a tree can be written, read
// back and rebuilt in a single call instead of one tool call per widget and
// property. A node is
//
//   { "class": "Button", "name": "Btn_Play", "variable": true,
//     "props": { <widget properties> }, "slot": { <slot properties> },
//     "children": [ <nodes> ] }
//
// and the document is { "root": <node> } or the root node itself. "class" is
// a UMG class name (Button), a native class path (/Script/UMG.Button) or a
// Widget Blueprint path (/Game/UI/WBP_Item). Property values use the same
// JSON as UToolsetLibrary::Get/SetObjectProperties.
namespace UIWTWidgetSpec
{

  // Makes InBlueprint's tree match the spec. Widgets whose name and class
  // match a spec node are kept (same object, so graph references, bindings
  // and animations stay valid) and re-parented; the rest are created, and
  // widgets absent from the spec are deleted. Nothing is changed when the
  // spec is invalid, would delete a widget that the graph, an animation, a
  // property binding or a required BindWidget still needs, gives a
  // BindWidget name a widget of the wrong class, or names a new widget like
  // something the blueprint already names (a variable of it or a parent
  // class, a function, a graph). Widget properties not named in the
  // spec keep their current values on kept widgets; slots are always
  // recreated, so slot properties come from the spec alone. Blueprints with
  // named-slot content are refused, since a spec only describes panel
  // children. Compiles afterwards.
  //
  // Returns false with OutErrors set when nothing was changed. Otherwise
  // returns true; OutErrors then lists problems applying individual
  // properties (the tree is built regardless), and OutReport summarises the
  // result, including compile errors.
  bool Apply(UWidgetBlueprint *InBlueprint, const FString &InSpecJson,
             FString &OutReport, TArray<FString> &OutErrors);

  // The same with the spec already parsed, so a caller that builds the spec
  // as JSON objects doesn't write it to a string for Apply to parse again.
  // InSpec is not modified.
  bool Apply(UWidgetBlueprint *InBlueprint, const TSharedRef<FJsonObject> &InSpec,
             FString &OutReport, TArray<FString> &OutErrors);

  // Writes InBlueprint's tree as a spec: every widget with its class, name,
  // variable flag, and the editable widget and slot properties that differ
  // from their class defaults. Numbers are written in their shortest form at
  // float precision (whole numbers exactly), and colour channels with 5
  // decimals. Apply(Export()) rebuilds an equivalent tree.
  // Empty object for an empty tree.
  bool Export(UWidgetBlueprint *InBlueprint, FString &OutJson,
              FString &OutError);

  // Export of one widget and its descendants, as {"root": node}; the node
  // keeps its slot in its parent. For editing part of a large tree.
  bool ExportSubtree(UWidgetBlueprint *InBlueprint, const FString &InWidgetName,
                     FString &OutJson, FString &OutError);

  // Makes the subtree rooted at InWidgetName match the spec ({"root": node}
  // or the node), with Apply's rules inside it: widgets kept by name and
  // class, the rest created, widgets left out removed unless something
  // still needs them. Widgets outside the subtree stay as Export gives them.
  // Their names count toward uniqueness, so a spec that uses one (moving an
  // outside widget in) is refused. The subtree root keeps its slot unless
  // the spec gives one. Compiles once. Same results as Apply.
  bool ApplySubtree(UWidgetBlueprint *InBlueprint, const FString &InWidgetName,
                    const FString &InSpecJson, FString &OutReport,
                    TArray<FString> &OutErrors);

  // A copy of InValue with every number written as Export writes numbers,
  // so a value from a spec compares equal to what Export gives back for it
  // (the design import's sidecar stores values this way).
  TSharedPtr<FJsonValue> NormalizeValue(const TSharedPtr<FJsonValue> &InValue);

  // What a spec's classes mean, the way Apply and Export see them: for
  // comparing specs (the design import's merge). Caches per class for the
  // object's life, so make one per comparison.
  class FClassInfo
  {
  public:
    FClassInfo();
    ~FClassInfo();

    // The class a spec's "class" names; null when unknown.
    UClass *ResolveClass(const FString &InSpecClass);
    // The slot class a panel gives its children; null for non-panels.
    UClass *SlotClass(UClass *InPanelClass);
    bool IsPanel(const UClass *InClass) const;
    bool CanHaveMultipleChildren(const UClass *InClass) const;
    // Whether the class has a property of this name at all.
    bool HasProperty(const UClass *InClass, const FString &InProperty) const;
    // Whether Export writes the property when it differs from the default
    // (editable, not transient, not editor bookkeeping).
    bool IsExported(const UClass *InClass, const FString &InProperty);
    // The class default's value, numbers normalized as Export writes them;
    // null when Export doesn't write the property.
    TSharedPtr<FJsonValue> Default(const UClass *InClass, const FString &InProperty);

  private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
  };

  // A widget something outside the tree depends on by name, so an edit must
  // keep it with the same name and class.
  struct FProtectedWidget
  {
    FName Name;
    UClass *Class = nullptr;
    // Why, one entry per dependency: "graph", "animation <name>",
    // "property binding <property>", "BindWidget" or "BindWidgetOptional".
    TArray<FString> Reasons;
  };

  // Every widget in InBlueprint's tree that the graph uses, an animation
  // animates, a property binding targets, or the parent class binds
  // (BindWidget), in tree order. Apply refuses to delete or change the class
  // of any of these, except that an optionally bound widget may be left out
  // (its property then ends up null).
  TArray<FProtectedWidget> FindProtectedWidgets(UWidgetBlueprint *InBlueprint);

}
