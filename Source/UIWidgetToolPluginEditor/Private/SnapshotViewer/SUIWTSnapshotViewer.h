#pragma once

#include "CoreMinimal.h"
#include "UIWTSnapshotReader.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

class SSearchBox;
class SUIWTSnapshotCanvas;
class SUIWTSnapshotImage;
class UWidgetBlueprint;

// One widget in a Widget Blueprint's design-time tree - what the LLM edits,
// as opposed to the runtime Slate tree a snapshot captures.
struct FUIWTDesignNode
{
  FString Name;
  FString SlotClassName;
  TArray<TSharedRef<FUIWTDesignNode>> Children;
};

class SUIWTSnapshotViewer : public SCompoundWidget
{
public:
  SLATE_BEGIN_ARGS(SUIWTSnapshotViewer) {}
  SLATE_END_ARGS()

  void Construct(const FArguments &InArgs);

  bool LoadSnapshot(const FString &InFilePath, FText &OutError);

  // Back to the empty state: no document, tree or image.
  void ClearSnapshot();

  // File the viewer is showing; empty until a load succeeds.
  const FString &GetLoadedSnapshotPath() const { return LoadedSnapshotPath; }

  // True when the viewer shows this file as it is on disk now. The owner
  // asks before loading, so re-selecting the same entry keeps the tree
  // state, while a file rewritten in place is loaded again.
  bool IsShowingSnapshot(const FString &InFilePath) const;

  // Shows the design-time tree of the manager's selected blueprint in the
  // Blueprint tab. Picks on the image resolve against InAcceptedAssetPaths
  // (the copy and its root original) so an older snapshot of the source still
  // maps onto the copy. Null clears the tab.
  void SetDesignBlueprint(UWidgetBlueprint *InBlueprint,
                          const TArray<FString> &InAcceptedAssetPaths);

  // Design-tree widget the user picked, by name; empty when nothing is
  // picked. Handed to the next run as its context. Dies with the viewer:
  // a pick against a tree the user can no longer see is a stale input.
  const FString &GetPickedWidget() const { return PickedWidget; }

private:
  FString PickedWidget;
  // True while PickedWidget came from a pick on the snapshot image rather
  // than a click in the Blueprint tree; the tree marks that row. A click
  // elsewhere in the tree moves the pick and drops the mark.
  bool bPickedFromImage = false;

  TSharedRef<SWidget> BuildTreePanel();
  TSharedRef<SWidget> BuildColumnMenu();
  TSharedRef<SHeaderRow> BuildHeaderRow();

  // Swaps in a document (null for none) and resets everything derived from
  // the previous one: selection, expansion, search, roots, parent map.
  void ApplyDocument(const TSharedPtr<FUIWTSnapshotDocument> &InDocument,
                     const FString &InFilePath);

  TSharedRef<ITableRow> OnGenerateRow(TSharedRef<FUIWTSnapshotNode> Item,
                                      const TSharedRef<STableViewBase> &Owner);
  void OnGetChildren(TSharedRef<FUIWTSnapshotNode> Item,
                     TArray<TSharedRef<FUIWTSnapshotNode>> &OutChildren);
  void OnTreeSelectionChanged(TSharedPtr<FUIWTSnapshotNode> Item,
                              ESelectInfo::Type SelectInfo);
  void OnTreeDoubleClick(TSharedRef<FUIWTSnapshotNode> Item);

  // Pushes a selection to the image panel and, unless it came from the tree
  // itself, mirrors it into the tree (expanded and scrolled into view).
  void SetSelection(const TArray<TSharedRef<FUIWTSnapshotNode>> &InNodes,
                    bool bFromTree);
  void HandleNodesHovered(const TArray<TSharedRef<FUIWTSnapshotNode>> &InNodes);
  void HandleNodesCommitted(const TArray<TSharedRef<FUIWTSnapshotNode>> &InNodes);
  void ExpandToNode(const TSharedRef<FUIWTSnapshotNode> &InNode);

  void OnSearchTextChanged(const FText &InText);
  void RebuildTreeSource();
  bool SubtreeMatchesSearch(const TSharedRef<FUIWTSnapshotNode> &InNode) const;

  bool IsColumnVisible(FName InColumnId) const;
  void ToggleColumn(FName InColumnId);
  bool CanToggleColumn(FName InColumnId) const;
  void SaveHiddenColumns();
  TArray<FName> LoadHiddenColumns() const;

  void OnRootSelectionChanged(TSharedPtr<int32> InIndex,
                              ESelectInfo::Type SelectInfo);
  TSharedRef<SWidget> OnGenerateRootWidget(TSharedPtr<int32> InIndex) const;
  FText GetRootComboText() const;
  FText GetRootDisplayName(int32 InIndex) const;
  EVisibility GetRootComboVisibility() const;

  FText GetStatusText() const;
  EVisibility GetEmptyStateVisibility() const;
  EVisibility GetTreeVisibility() const;

  void BuildParentMap();
  void BuildParentMapRecursive(const TSharedRef<FUIWTSnapshotNode> &InNode);

  // --- Blueprint tab.
  TSharedRef<SWidget> BuildDesignPanel();
  TSharedRef<ITableRow> OnGenerateDesignRow(TSharedRef<FUIWTDesignNode> Item,
                                            const TSharedRef<STableViewBase> &Owner);
  void OnGetDesignChildren(TSharedRef<FUIWTDesignNode> Item,
                           TArray<TSharedRef<FUIWTDesignNode>> &OutChildren);
  void OnDesignSelectionChanged(TSharedPtr<FUIWTDesignNode> Item,
                                ESelectInfo::Type SelectInfo);
  void ExpandAllDesignNodes();
  TSharedPtr<FUIWTDesignNode> FindDesignNode(const FString &InName) const;
  // Selects the node in the Blueprint tree (null clears) without treating
  // it as a user click, and records it as the picked widget. bFromImage
  // marks the row as picked on the snapshot image.
  void SelectDesignNode(const TSharedPtr<FUIWTDesignNode> &InNode,
                        bool bFromImage);
  // Whether the Blueprint tree marks this row as the image pick.
  bool IsDesignNodePicked(TSharedRef<FUIWTDesignNode> InNode) const;
  // The design widget a pick on the image lands on: the nearest snapshot
  // ancestor (leaf first) that is a UMG widget of one of the accepted assets
  // and exists in the design tree. Null when nothing on the chain is a child
  // widget of the blueprint: a pick outside the accepted assets, or on the
  // user widget itself, whose instance name is not in the tree.
  TSharedPtr<FUIWTDesignNode>
  ResolvePickedDesignNode(const TArray<TSharedRef<FUIWTSnapshotNode>> &InNodes) const;
  void CollectRuntimeNodes(const TSharedRef<FUIWTSnapshotNode> &InNode,
                           const FString &InUMGName,
                           TArray<TSharedRef<FUIWTSnapshotNode>> &OutNodes) const;
  bool SnapshotMatchesDesign() const;
  // The two trees above the image, in the switcher's slot order.
  enum class EPane : int32
  {
    Runtime,
    Blueprint
  };
  int32 GetActivePaneIndex() const { return static_cast<int32>(ActivePane); }
  // One tab of the Runtime / Blueprint strip above the trees.
  TSharedRef<SWidget> MakePaneTab(EPane InPane, const FText &InLabel,
                                  const FText &InToolTip);
  ECheckBoxState GetPaneCheckState(EPane InPane) const;
  void OnPaneTabChanged(ECheckBoxState InState, EPane InPane);
  void OnDesignSearchTextChanged(const FText &InText);
  bool DesignSubtreeMatchesSearch(const TSharedRef<FUIWTDesignNode> &InNode) const;

  TSharedPtr<STreeView<TSharedRef<FUIWTDesignNode>>> DesignTreeView;
  TSharedPtr<SSearchBox> DesignSearchBox;
  FString DesignSearchText;
  TArray<TSharedRef<FUIWTDesignNode>> DesignRoots;
  TArray<TSharedRef<FUIWTDesignNode>> AllDesignNodes;
  TArray<FString> AcceptedAssetPaths;
  FString DesignBlueprintName;
  // Identifies the blueprint across refreshes: the same one re-sent after an
  // edit keeps the pick, a different one drops it.
  FString DesignBlueprintPath;
  EPane ActivePane = EPane::Runtime;
  bool bSuppressDesignSelection = false;

  TSharedPtr<FUIWTSnapshotDocument> Document;
  FString LoadedSnapshotPath;
  FDateTime LoadedSnapshotTimestamp = FDateTime::MinValue();
  int32 RootIndex = INDEX_NONE;

  TSharedPtr<STreeView<TSharedRef<FUIWTSnapshotNode>>> TreeView;
  TSharedPtr<SHeaderRow> HeaderRow;
  TSharedPtr<SSearchBox> SearchBox;
  TSharedPtr<SUIWTSnapshotImage> ImagePanel;
  // The image panel's canvas: where the document, root and selection go.
  TSharedPtr<SUIWTSnapshotCanvas> Canvas;

  TArray<TSharedRef<FUIWTSnapshotNode>> TreeSource;
  TArray<TSharedPtr<int32>> RootOptions;

  TMap<const FUIWTSnapshotNode *, TSharedRef<FUIWTSnapshotNode>> ParentMap;

  bool bSuppressSelectionCallback = false;

  FString SearchText;
};
