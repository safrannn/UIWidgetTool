#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolCallAsyncResultString.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "UIWTDesignToolset.generated.h"

class UWidgetBlueprint;

/**
 * Design imports: Figma frames, Photoshop exports (and design trees from any
 * source) become Widget Blueprints. An import is deterministic and uses no
 * tokens: the frame is read through Figma's REST API, or the export folder
 * Photoshop's Export for Unreal wrote is read from disk, normalized into a
 * design tree (design.json), converted to a widget spec and applied to a new
 * blueprint, with textures, the design-time size, reference.png and a
 * sidecar. Refine the result afterwards with the usual tools if the user
 * asks. Figma needs a token (FIGMA_TOKEN, or the local plugin settings).
 * Imports never overwrite an existing blueprint: updating one from its
 * design is ReimportDesign, which merges and keeps what was changed in UE.
 */
UCLASS(BlueprintType, Hidden)
class UUIWTDesignToolset : public UToolsetDefinition
{
  GENERATED_BODY()

public:
  /**
   * Downloads a Figma frame into the design-tree cache
   * (Saved/UIWidgetTool/Figma/<file>/<node>): design.json, the rendered
   * layers, the image fills and reference.png. Reuses the cache when the file
   * hasn't changed since.
   * @param Url A Figma link to the frame, with node-id (right-click the frame
   *   → Copy link to selection).
   * @param bRefresh Fetch again even when the cache has the current version.
   * @return JSON: designFile, referenceImage, fromCache, version, node and
   *   image counts, and the reader's notes (what was simplified).
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Design")
  static UToolCallAsyncResultString *FetchFigmaNode(const FString &Url, bool bRefresh);

  /**
   * Fetches a Figma frame (cached) and converts it to a widget spec without
   * creating anything: a preview of what an import would build.
   * @param Url A Figma link to the frame, with node-id.
   * @return JSON: designFile, spec (UIWTToolset.ApplyWidgetSpec's format),
   *   report (what the conversion approximated or dropped), widgets and
   *   depth.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Design")
  static UToolCallAsyncResultString *ConvertFigmaToSpec(const FString &Url);

  /**
   * Imports a Figma frame as a new Widget Blueprint: fetch (cached),
   * convert, textures, blueprint, design-time size, reference.png and the
   * sidecar used by later re-imports. Refuses a blueprint that already
   * exists.
   * @param Url A Figma link to the frame, with node-id.
   * @param TargetFolder Content folder for the blueprint's own folder, for
   *   example /Game/UI. Empty uses the project's default import folder.
   * @param BlueprintName Asset name, for example WBP_Shop. Empty uses
   *   WBP_<frame name>.
   * @return JSON: blueprint path, sidecar, reference, widget count, Apply's
   *   report and the import report.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Design")
  static UToolCallAsyncResultString *ImportFigmaFrame(const FString &Url,
                                                      const FString &TargetFolder,
                                                      const FString &BlueprintName);

  /**
   * Imports a design tree file (design.json from any design source) as a new
   * Widget Blueprint. Its image paths are relative to its folder.
   * @param DesignFile Absolute path of design.json, inside the project folder.
   * @param TargetFolder Content folder for the blueprint's own folder. Empty
   *   uses the project's default import folder.
   * @param BlueprintName Asset name. Empty uses WBP_<root name>.
   * @return JSON: blueprint path, sidecar, reference, widget count, Apply's
   *   report and the import report.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Design")
  static FString ImportDesignTree(const FString &DesignFile, const FString &TargetFolder,
                                  const FString &BlueprintName);

  /**
   * Reads a Photoshop export (the folder Export for Unreal writes, one per
   * artboard) and converts it to a widget spec without creating anything: a
   * preview of what an import would build. The export is copied into
   * Saved/UIWidgetTool/Psd first.
   * @param ManifestFile Absolute path of the export's manifest.json.
   * @return JSON: designFile, referenceImage (the export's composite),
   *   exportedAt, nodes, spec (UIWTToolset.ApplyWidgetSpec's format), report
   *   (what the conversion approximated or dropped, and the reader's notes),
   *   widgets and depth.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Design")
  static FString ConvertPsdManifestToSpec(const FString &ManifestFile);

  /**
   * One node of the design a blueprint was imported from, as design.json
   * writes it: box, fills, strokes, radii, text runs, image, effects, hints
   * and component. For details the outline leaves out.
   * @param WidgetBlueprint A blueprint made by a design import.
   * @param NodeId The node's id, as in the outline.
   * @param Depth Levels of children to include; deeper ones are counted in
   *   "hiddenChildren". 0 gives the node alone.
   * @return The node as JSON text.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Design")
  static FString GetDesignNode(UWidgetBlueprint *WidgetBlueprint, const FString &NodeId,
                               int32 Depth);

  /**
   * The compact outline of the design a blueprint was imported from: per
   * node its id, n(ame), k(ind), b(ox) [x, y, w, h] relative to its parent
   * in design pixels, the w(idget) it became, v false when hidden, h(ints)
   * and c(hildren).
   * @param WidgetBlueprint A blueprint made by a design import.
   * @param NodeId The subtree's root node; empty for the whole design.
   * @return The outline as JSON text.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Design")
  static FString GetDesignOutline(UWidgetBlueprint *WidgetBlueprint, const FString &NodeId);

  /**
   * Imports a Photoshop export as a new Widget Blueprint: textures from the
   * layer PNGs, blueprint, design-time size, the composite as reference.png
   * and the sidecar used by later re-imports. Refuses a blueprint that
   * already exists.
   * @param ManifestFile Absolute path of the export's manifest.json.
   * @param TargetFolder Content folder for the blueprint's own folder. Empty
   *   uses the project's default import folder.
   * @param BlueprintName Asset name. Empty uses WBP_<artboard or document
   *   name>.
   * @return JSON: blueprint path, sidecar, reference, widget count, Apply's
   *   report and the import report.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Design")
  static FString ImportPsdManifest(const FString &ManifestFile, const FString &TargetFolder,
                                   const FString &BlueprintName);

  /**
   * Updates a blueprint an import made from its design as it is now (a
   * Figma frame is fetched again; a Photoshop export is read again from its
   * folder, so export from Photoshop first), keeping what was changed in UE:
   * a value changed only in UE stays, one changed only in the design is
   * taken, one changed in both takes the design's and is reported as a
   * conflict.
   * Widgets added in UE stay where they are. Widgets the graph, an
   * animation, a property binding or BindWidget uses keep their name and
   * class and are never removed. Show the user the preview before applying.
   * @param WidgetBlueprint A blueprint made by a design import (it has a
   *   <name>.design.json sidecar next to it).
   * @param bApply False: only the preview, nothing changes. True: apply the
   *   merge, unsaved; then AcceptReimport saves it, DiscardReimport undoes it.
   * @param bForce Re-import even when the Figma file or the Photoshop export
   *   hasn't changed since the last import (to bring back widgets deleted in
   *   UE, for example).
   * @return JSON: the preview (changes by design node, conflicts, kept UE
   *   changes, widgets added in UE, protected widgets, report), and after
   *   applying, Apply's report and a render compared with the reference.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Design")
  static UToolCallAsyncResultString *ReimportDesign(UWidgetBlueprint *WidgetBlueprint, bool bApply,
                                                    bool bForce);

  /**
   * Keeps an applied re-import: saves the blueprint and its textures, copies
   * the new reference image, and updates the sidecar the next re-import
   * compares against.
   * @param WidgetBlueprint The blueprint ReimportDesign applied to.
   * @return What was saved.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Design")
  static FString AcceptReimport(UWidgetBlueprint *WidgetBlueprint);

  /**
   * Undoes an applied re-import: reloads the blueprint and changed textures
   * from disk and deletes the new textures. The sidecar was never changed.
   * @param WidgetBlueprint The blueprint ReimportDesign applied to.
   * @return What was restored.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Design")
  static FString DiscardReimport(UWidgetBlueprint *WidgetBlueprint);
};
