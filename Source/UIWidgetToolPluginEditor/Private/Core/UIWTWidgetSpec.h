#pragma once

#include "CoreMinimal.h"

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
  // spec is invalid or would delete a widget that the graph, an animation or
  // a required BindWidget still needs. Widget properties not named in the
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

  // Writes InBlueprint's tree as a spec: every widget with its class, name,
  // variable flag, and the editable widget and slot properties that differ
  // from their class defaults. Apply(Export()) rebuilds an equivalent tree.
  // Empty object for an empty tree.
  bool Export(UWidgetBlueprint *InBlueprint, FString &OutJson,
              FString &OutError);

}
