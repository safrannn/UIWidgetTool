#pragma once

#include "Core/UIWTDesignTree.h"
#include "CoreMinimal.h"

class FJsonObject;

namespace UIWTWidgetSpec
{
  class FClassInfo;
}

// The re-import's three-way merge (import-figma.md step 6, import-tree.md →
// Owned widgets). For every widget and value there are three versions:
//   base      what the last import wrote (the sidecar),
//   source    the design converted again, and
//   blueprint the blueprint as it is now (UIWTWidgetSpec::Export).
// Only the source changed: the source's. Only the blueprint changed: the
// blueprint's. Both: the source's, reported as a conflict. Widgets added in
// UE are kept where they are, and protected widgets (FindProtectedWidgets)
// keep their name and class and are never removed. Nothing is changed here:
// the result is a spec for Apply, the preview and the report.
namespace UIWTDesignMerge
{
  // Export's values for one widget, normalized.
  struct FExported
  {
    TSharedPtr<FJsonObject> Props;
    TSharedPtr<FJsonObject> Slot;
  };

  // One widget as the last import wrote it, values normalized.
  struct FBaseWidget
  {
    FString Class;
    // "" for the root.
    FString Parent;
    // What the converter wrote: the design's side of the base. Design
    // changes are found by comparing the new conversion with these.
    TSharedPtr<FJsonObject> Props;
    TSharedPtr<FJsonObject> Slot;
    // What Export gave for the widget after the last import: the
    // blueprint's side of the base. Changes made in UE are found by
    // comparing Export with this, not with Props and Slot, because Apply
    // derives values the converter never wrote (a Border's slot copies its
    // Padding) and those must not count as changes. Unset in sidecars
    // written before it existed; Props and Slot stand in then.
    TOptional<FExported> Exported;
  };

  // What the merge kept from the blueprint for one widget because the
  // design didn't change it. The next base keeps its old exported values
  // there, so the change made in UE still counts as one next time.
  struct FKeptValues
  {
    // The whole widget (its class was changed in UE).
    bool bWhole = false;
    // The whole slot (it was moved in UE).
    bool bSlotWhole = false;
    TSet<FString> Props;
    TSet<FString> Slot;
  };

  struct FInput
  {
    // The last import: node → role → widget, and each widget's values.
    UIWTDesignTree::FOwnedMap BaseOwned;
    TMap<FString, FBaseWidget> BaseWidgets;
    // The new conversion: {"root": ...}, and its owned map.
    TSharedPtr<FJsonObject> Source;
    UIWTDesignTree::FOwnedMap SourceOwned;
    // Export's output: {"root": ...}, or {} for an empty tree.
    TSharedPtr<FJsonObject> Blueprint;
    // Protected widget name → the reasons FindProtectedWidgets gives.
    TMap<FString, TArray<FString>> Protected;
  };

  enum class EChange : uint8
  {
    Added,
    Removed,
    Changed
  };

  // A design node the source changed since the last import: the preview.
  struct FChange
  {
    FString Node;
    EChange Change = EChange::Changed;
    // One line per widget, e.g. "Card_Bg: props Background, Padding".
    TArray<FString> Details;
  };

  struct FResult
  {
    // The spec to Apply; null when nothing is left to build (no root).
    TSharedPtr<FJsonObject> Spec;
    // Source changes since the last import, by node, in id order.
    TArray<FChange> Changes;
    // conflict, keptUEChange, protectedClass, protectedKept, mergeMoved.
    TArray<UIWTDesignTree::FReportEntry> Report;
    int32 Conflicts = 0;
    int32 KeptUEChanges = 0;
    // Widgets the design doesn't know (added in UE), kept where they are.
    int32 UEWidgets = 0;
    // By widget name; see FKeptValues.
    TMap<FString, FKeptValues> KeptFromBlueprint;
  };

  FResult Merge(const FInput &InInput, UIWTWidgetSpec::FClassInfo &InClasses);

  // The sidecar's "widgets" object as base widgets.
  void ReadBaseWidgets(const TSharedPtr<FJsonObject> &InWidgets,
                       TMap<FString, FBaseWidget> &OutWidgets);

  // Export's output ({"root": ...}) as widget name → normalized values.
  TMap<FString, FExported> ExportedWidgets(const TSharedPtr<FJsonObject> &InExportSpec);

  // The exported values the next sidecar stores: Export after this merge's
  // Apply, except where the merge kept the blueprint's value, which keeps
  // the last base's exported value (see FKeptValues).
  TMap<FString, FExported> NextExported(const TMap<FString, FExported> &InAfterApply,
                                        const TMap<FString, FBaseWidget> &InBase,
                                        const TMap<FString, FKeptValues> &InKept);
}
