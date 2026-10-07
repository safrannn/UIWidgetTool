#pragma once

#include "Core/UIWTDesignMerge.h"
#include "Core/UIWTDesignTree.h"
#include "CoreMinimal.h"

class FJsonObject;
class UWidgetBlueprint;

// The shared design import (import-figma.md steps 4 and 6, import-tree.md):
// design.json → converter → textures → a new Widget Blueprint through
// UIWTWidgetSpec::Apply → design-time size, reference image and sidecar.
// Every design source (Figma, Photoshop) ends here.
//
// Import creates a new blueprint and refuses an existing one. Re-importing
// into a blueprint an import made goes through the three-way merge
// (PlanReimport → ApplyReimport → AcceptReimport or DiscardReimport), which
// keeps what was changed in UE. Instances without overrides become shared
// child WBPs in the components folder.
namespace UIWTDesignImport
{
  struct FRequest
  {
    // Absolute path of design.json. Its image paths are relative to its
    // folder and must stay inside it.
    FString DesignFile;
    // Content folder that gets the blueprint's own folder, for example
    // /Game/UI. Empty uses UUIWTDesignSettings' default import folder.
    FString TargetFolder;
    // Asset name. Empty uses WBP_<root node name>.
    FString BlueprintName;
    // The blueprint's parent class, a UUserWidget subclass. Null means
    // UUserWidget.
    UClass *ParentClass = nullptr;
    // Overrides the document's rootMode when not Auto.
    UIWTDesignTree::ERootMode RootMode = UIWTDesignTree::ERootMode::Auto;
    // The rendered design to copy into the blueprint's folder as
    // reference.png. Empty uses reference.png next to design.json, if any.
    FString ReferenceImage;
    // Copy that reference image at all (child WBPs don't: it shows the
    // screen, not the component).
    bool bCopyReference = true;
    // Content folder of the child WBPs made from components, shared by
    // every screen. Empty uses UUIWTDesignSettings' components folder.
    FString ComponentsFolder;
    // Set when the tree is a component's (a child WBP): its root gets no
    // size role, and the sidecar records the key.
    FString ComponentKey;
    // Saves the textures and the blueprint. Tests import unsaved into /Temp.
    bool bSave = true;
    // Lets the blueprint go into the generated-blueprint mount, which
    // imports otherwise refuse. Only OverwriteImport sets it, for an entry's
    // blueprint that is already there.
    bool bAllowGenerated = false;
  };

  struct FResult
  {
    UWidgetBlueprint *Blueprint = nullptr;
    // Package name, e.g. /Game/UI/WBP_Shop/WBP_Shop
    FString BlueprintPath;
    FString SidecarFile;
    // Empty when there was no reference image to copy.
    FString ReferenceFile;
    UIWTDesignTree::ERootMode RootMode = UIWTDesignTree::ERootMode::Screen;
    // The design-time size set on the blueprint; zero for desired size.
    FIntPoint DesignSize = FIntPoint::ZeroValue;
    // Converter and reader findings, plus textureFailed entries.
    TArray<UIWTDesignTree::FReportEntry> Report;
    // Apply's summary, including compile errors.
    FString ApplyReport;
    // Properties Apply couldn't set; the tree was built regardless.
    TArray<FString> ApplyErrors;
    int32 WidgetCount = 0;
    int32 MaxDepth = 0;
    int32 TexturesWritten = 0;
    bool bSaved = false;
    // What happened to each child WBP, e.g. "btn: created /Game/.../WBP_Button".
    TArray<FString> Components;
    // The assets this import created (textures, the blueprint).
    TArray<TWeakObjectPtr<UObject>> CreatedAssets;
  };

  // Imports a design tree as a new Widget Blueprint. Returns false with
  // OutError set when nothing was imported: an unreadable tree, a spec over
  // Apply's limits (OutResult.Report says which parts are largest), an
  // existing blueprint at the target, or Apply refusing the spec. Textures
  // that fail to load are reported and left out; the import goes on.
  //
  // Instances without overrides become child WBPs in the components folder
  // (import-tree.md → Assets → Child WBPs), one per component.key per
  // project: created when the components index doesn't have the key,
  // updated through the re-import merge when its hash changed, reused
  // otherwise, deepest first and each once. They are saved with the screen.
  bool Import(const FRequest &InRequest, FResult &OutResult, FString &OutError);

  // The components index next to a components folder: component.key → the
  // child WBP and the main component's hash it was built from. Entries whose
  // asset is gone are left out, so their component is created again.
  TMap<FString, UIWTDesignTree::FComponentIndexEntry>
  ReadComponentIndex(const FString &InComponentsFolder);
  FString GetComponentIndexPath(const FString &InComponentsFolder);

  // The result as a JSON object text, for tools and logs.
  FString ResultToJson(const FResult &InResult);

  // Names a new widget can't take in a blueprint with this parent class:
  // every property and function of the class and its parents, and
  // EventGraph (import-tree.md → Names). BindWidget properties are left out:
  // a widget is meant to take that name, and Apply checks its class.
  TSet<FString> ReservedNamesForClass(const UClass *InParentClass);

  // The same for re-importing into InBlueprint: its parent class's names,
  // its own variables, functions, graphs, components and animations, and its
  // widgets the last import didn't make (InOwned), since the merge keeps
  // those. Protected widgets' names are left out: they never change.
  TSet<FString> ReservedNamesForBlueprint(const UWidgetBlueprint *InBlueprint,
                                          const UIWTDesignTree::FOwnedMap &InOwned,
                                          const TSet<FString> &InProtected);

  // The converter options of a first import into InFolder/InName/InName:
  // reserved names from the parent class, the settings' components folder,
  // its components index and font map, no sidecar names or textures, and
  // the asset names already in those folders.
  UIWTDesignTree::FConvertOptions FirstImportOptions(const FString &InFolder,
                                                     const FString &InName,
                                                     const UClass *InParentClass);

  // Fills InOutOptions' ResolveFont and NaturalLineHeight from
  // UUIWTDesignSettings' font map, else the font library
  // (UIWTDesignFonts::FindLibraryFont), and Slate's font measuring. Font
  // assets are loaded once per set of options.
  void SetFontResolver(UIWTDesignTree::FConvertOptions &InOutOptions);

  // WBP_<sanitized root name>.
  FString DefaultBlueprintName(const UIWTDesignTree::FDocument &InDocument);

  // <blueprint folder on disk>/<name>.design.json
  FString GetSidecarPath(const FString &InBlueprintPackage);

  // The package Import would create for InRequest, e.g.
  // /Game/UI/WBP_Shop/WBP_Shop. Empty when the design tree can't be read.
  FString GetImportPackage(const FRequest &InRequest);

  // The full replace, the merge's alternative: deletes InBlueprint (an
  // import's, in its own folder) and the textures its import wrote, then
  // imports InRequest's design fresh under the same folder and name, and the
  // same parent class unless InRequest gives one. Changes made in UE are
  // lost. Refuses a child WBP and a blueprint other assets use; false with
  // OutError set when nothing was replaced, or when the new import failed
  // after the delete (OutError says so).
  bool ReplaceImport(UWidgetBlueprint *InBlueprint, const FRequest &InRequest,
                     FResult &OutResult, FString &OutError);

  // The same for a blueprint no import made (it has no sidecar), such as an
  // entry's empty one in the generated-blueprint mount: deletes it and
  // imports InRequest's design in its place, under the same folder and name
  // and with the same parent class unless InRequest gives one. Everything
  // else in its folder stays. Refuses a blueprint that isn't in its own
  // folder and one other assets use; OutError as for ReplaceImport.
  bool OverwriteImport(UWidgetBlueprint *InBlueprint, const FRequest &InRequest,
                       FResult &OutResult, FString &OutError);

  // ---------------------------------------------------------------------
  // Re-import (import-figma.md step 6)

  // What the last import wrote next to a blueprint.
  struct FSidecar
  {
    FString Source;
    TSharedPtr<FJsonObject> SourceRef;
    // Absolute.
    FString DesignFile;
    FString ParentClass;
    // The components folder its child WBPs are in.
    FString ComponentsFolder;
    // Set for a child WBP: the component.key it was made from.
    FString ComponentKey;
    UIWTDesignTree::FOwnedMap Owned;
    TMap<FString, FString> Textures;
    TMap<FString, UIWTDesignMerge::FBaseWidget> Widgets;
    // The converter's report from the last import or re-import.
    TArray<UIWTDesignTree::FReportEntry> Report;
    // The design nodes the last accepted re-import added or changed: the
    // scope of an AI pass on the changed parts. Cleared once one succeeds.
    TArray<FString> ChangedNodes;
  };

  // False with OutError set when the blueprint has no readable sidecar (it
  // wasn't made by an import).
  bool ReadSidecar(const FString &InBlueprintPackage, FSidecar &OutSidecar, FString &OutError);

  // After an AI pass (import-tree.md → Claude pass): writes the widget
  // renames it made (old name → new name) into the sidecar's owned map and
  // base widgets, so the next re-import keeps the new names. InRemoved lists
  // widgets the pass deleted; one whose name a renamed widget took is
  // dropped from the sidecar, since the name now means the other widget.
  bool RenameInSidecar(const FString &InBlueprintPackage, const TMap<FString, FString> &InRenames,
                       const TSet<FString> &InRemoved, FString &OutError);

  // Forgets the last re-import's changed nodes, after an AI pass covered
  // them.
  bool ClearChangedNodes(const FString &InBlueprintPackage, FString &OutError);

  struct FReimportRequest
  {
    UWidgetBlueprint *Blueprint = nullptr;
    // The design as it is now. Empty uses the sidecar's designFile.
    FString DesignFile;
    // Empty uses reference.png next to the design file, if any.
    FString ReferenceImage;
    // Use a reference image at all (child WBPs don't).
    bool bUseReference = true;
  };

  struct FReimportState;

  // The preview (step 4): what the design changed since the last import,
  // what the merge will do about changes made in UE, and the merged spec.
  struct FReimportPlan
  {
    FString BlueprintPath;
    TArray<UIWTDesignMerge::FChange> Changes;
    // Converter and reader entries, then the merge's (conflict,
    // keptUEChange, protectedClass, protectedKept, mergeMoved).
    TArray<UIWTDesignTree::FReportEntry> Report;
    int32 Conflicts = 0;
    int32 KeptUEChanges = 0;
    int32 UEWidgets = 0;
    int32 WidgetCount = 0;
    // "Name (reasons)" for every protected widget.
    TArray<FString> Protected;
    // What applying will do to each child WBP the design uses directly,
    // e.g. "btn: update /Game/.../WBP_Button".
    TArray<FString> Components;
    // What ApplyReimport needs.
    TSharedPtr<FReimportState> State;
  };

  // Steps 2–4: exports the blueprint, finds its protected widgets, converts
  // the design with the sidecar's names and textures and merges. Nothing is
  // changed. Refuses a design from another file or frame than the sidecar's,
  // and a blueprint whose last re-import is still waiting for Accept or
  // Discard.
  bool PlanReimport(const FReimportRequest &InRequest, FReimportPlan &OutPlan, FString &OutError);

  struct FReimportResult
  {
    FString ApplyReport;
    TArray<FString> ApplyErrors;
    int32 TexturesWritten = 0;
    int32 TexturesCreated = 0;
    // The render compare (step 7), when the editor can render and there is
    // a reference image; MeanDifference is in percent, -1 without one.
    FString RenderFile;
    FString CompareFile;
    double MeanDifference = -1.0;
    // What happened to each child WBP, and their reports.
    TArray<FString> Components;
    TArray<UIWTDesignTree::FReportEntry> ComponentReport;
  };

  // Steps 5–7: creates or updates the child WBPs the design uses, writes the
  // textures and Applies the merged spec once. Everything stays unsaved and
  // the sidecars unchanged until AcceptReimport or DiscardReimport, which
  // accept or undo the child WBPs along with the blueprint.
  bool ApplyReimport(const FReimportPlan &InPlan, FReimportResult &OutResult, FString &OutError);

  bool HasPendingReimport(const UWidgetBlueprint *InBlueprint);

  // Step 8, accept: saves the blueprint and textures (when bInSave), copies
  // the new reference image and writes the sidecar for the next re-import.
  bool AcceptReimport(UWidgetBlueprint *InBlueprint, bool bInSave, FString &OutError);

  // Step 8, discard: reloads the blueprint and the changed textures from
  // disk and deletes the new ones. The sidecar was never touched.
  bool DiscardReimport(UWidgetBlueprint *InBlueprint, FString &OutError);

  FString PlanToJson(const FReimportPlan &InPlan);
  FString ReimportResultToJson(const FReimportResult &InResult);
}
