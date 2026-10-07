#pragma once

#include "Core/UIWTDesignTree.h"
#include "CoreMinimal.h"

class UWidgetBlueprint;
enum class EUIWTClaudeModel : uint8;

namespace UIWTDesignImport
{
  struct FSidecar;
}

// The optional AI pass after a design import (import-tree.md → Claude pass,
// Phase 7): what Claude is told, what it's estimated to cost, the design
// outline and nodes it reads, and the renames written back into the
// sidecar afterwards. The run itself is an ordinary Claude run of
// FUIWTClaudeService on the blueprint's manager entry.
namespace UIWTDesignRefine
{
  // The models import-tree.md has prices for.
  enum class EPrices : uint8
  {
    Unknown,
    Sonnet,
    Opus
  };

  EPrices PricesFor(EUIWTClaudeModel InModel, const FString &InCustomModel);

  struct FEstimate
  {
    int32 Rounds = 0;
    // Read at the input price (the first read and each round's new
    // context), read back from the prompt cache, and written.
    int64 InputTokens = 0;
    int64 CachedTokens = 0;
    int64 OutputTokens = 0;
    // At API rates; negative when the model's prices aren't known.
    double Dollars = -1.0;
  };

  // import-tree.md → Cost: with and without the Claude pass, with subtree
  // specs: an estimate from the scope's node count, not a measurement.
  FEstimate Estimate(int32 InNodes, int32 InRounds, EPrices InPrices);

  // An image-reading run (import-image.md, section 5): each round writes the
  // whole tree, and with bInAIPass a pass's rounds follow in the session.
  FEstimate EstimateReading(int32 InNodes, int32 InRounds, EPrices InPrices, bool bInAIPass);

  // The node count to expect for an image of this size in design pixels.
  int32 ExpectedNodes(const FVector2D &InDesignSize);

  // What a pass may change (import-tree.md → When it runs → Scope).
  struct FScope
  {
    // Design nodes whose subtrees the pass may change; empty for the whole
    // blueprint.
    TArray<FString> Nodes;
    // The nodes are the sidecar's changedNodes ("Refine changed parts").
    bool bChangedParts = false;
  };

  // The design node a widget of the blueprint was made from: the node that
  // owns it, or the node owning its nearest ancestor. Empty when none does.
  FString NodeForWidget(const UWidgetBlueprint *InBlueprint,
                        const UIWTDesignTree::FOwnedMap &InOwned, const FString &InWidgetName);

  // What a pass on one imported blueprint needs.
  struct FPass
  {
    // "figma" or "psd".
    FString Source;
    // The scope's design nodes (the whole design for the whole blueprint).
    int32 NodeCount = 0;
    // The request: the pass's instructions, the scope, the outline and the
    // report.
    FString Request;
    // reference.png in the blueprint's folder; empty without one.
    FString ReferenceImage;
    // The image to attach: the reference, or its crop to the scope, written
    // under Saved/UIWidgetTool/AIPass. Empty without a reference.
    FString AttachImage;
    // The crop in reference pixels; empty when the whole reference is sent.
    FIntRect Crop;
    // The outermost widget of each scope part; empty for the whole
    // blueprint.
    TArray<FString> ScopeWidgets;
    // For the chat: "the whole blueprint", "Plate", "3 parts".
    FString ScopeLabel;
  };

  // The sidecar of a blueprint a design import made, and the design
  // (design.json) it points at.
  bool LoadDesign(const UWidgetBlueprint *InBlueprint, UIWTDesignImport::FSidecar &OutSidecar,
                  UIWTDesignTree::FDocument &OutDocument, FString &OutError);

  // Reads the blueprint's sidecar and design and writes the request.
  // InReport is the import's report; empty uses the sidecar's. Refuses a
  // child WBP, a blueprint with a re-import waiting for Accept or Discard,
  // and a scope none of whose nodes still has a widget.
  bool Prepare(UWidgetBlueprint *InBlueprint, int32 InRounds, const FScope &InScope,
               const TArray<UIWTDesignTree::FReportEntry> &InReport, FPass &OutPass,
               FString &OutError);

  // The compact outline (import-tree.md → Claude's context): per node its
  // id, n(ame), k(ind), b(ox) [x, y, w, h] relative to its parent, w(idget)
  // it became, h(ints) and c(hildren), as JSON text.
  FString MakeOutline(const UIWTDesignTree::FNode &InRoot,
                      const UIWTDesignTree::FOwnedMap &InOwned);

  // The node with this id in the document (its root or a component's), as
  // design.json writes it, with InDepth levels of children; deeper children
  // are left out and counted in "hiddenChildren". Null when there's none.
  const UIWTDesignTree::FNode *FindNode(const UIWTDesignTree::FDocument &InDocument,
                                        const FString &InId);
  FString NodeJson(const UIWTDesignTree::FNode &InNode, int32 InDepth);

  // What a pass does, by source ("figma"; "psd" and "image" share the
  // layout-less list), as the request's "Do" section.
  FString PassTasks(const FString &InSource);

  // Widget GUID → name: widgets keep their GUID through a rename.
  TMap<FGuid, FName> SnapshotWidgets(const UWidgetBlueprint *InBlueprint);

  // Writes the renames made since InBefore into the blueprint's sidecar
  // (UIWTDesignImport::RenameInSidecar). OutRenamed is their number.
  bool SyncRenames(UWidgetBlueprint *InBlueprint, const TMap<FGuid, FName> &InBefore,
                   int32 &OutRenamed, FString &OutError);
}
