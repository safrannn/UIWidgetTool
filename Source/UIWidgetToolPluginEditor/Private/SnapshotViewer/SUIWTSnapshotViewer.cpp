#include "SUIWTSnapshotViewer.h"

#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/FileManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Rendering/SlateRenderer.h"
#include "SUIWTSnapshotImage.h"
#include "SUIWTSnapshotTreeRow.h"
#include "Styling/AppStyle.h"
#include "Blueprint/WidgetTree.h"
#include "Components/NamedSlotInterface.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "WidgetBlueprint.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Views/SExpanderArrow.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UIWidgetToolSnapshotViewer"

namespace
{

  const TCHAR *ColumnConfigSection = TEXT("UIWidgetTool.SnapshotViewer");
  const TCHAR *ColumnConfigKey = TEXT("HiddenColumns");
  constexpr float HeaderLabelAllowance = 16.f;

}

void SUIWTSnapshotViewer::Construct(const FArguments &)
{
  ChildSlot
      [SNew(SVerticalBox) +
       SVerticalBox::Slot().FillHeight(1.f)
           [SNew(SSplitter).Orientation(Orient_Vertical) +
            SSplitter::Slot().Value(0.4f)
                [SNew(SVerticalBox) +
                 SVerticalBox::Slot().AutoHeight()
                     [SNew(SBorder)
                          .BorderImage(
                              FAppStyle::GetBrush("ToolPalette.DockingWell"))
                          .Padding(FMargin(2.f, 2.f, 2.f, 0.f))
                              [SNew(SHorizontalBox) +
                               SHorizontalBox::Slot().AutoWidth()
                                   [MakePaneTab(
                                       EPane::Runtime,
                                       LOCTEXT("PaneRuntime", "Runtime"),
                                       LOCTEXT("PaneRuntimeTip",
                                               "The Slate hierarchy the "
                                               "snapshot captured while "
                                               "playing."))] +
                               SHorizontalBox::Slot().AutoWidth().Padding(
                                   FMargin(2.f, 0.f, 0.f, 0.f))
                                   [MakePaneTab(
                                       EPane::Blueprint,
                                       LOCTEXT("PaneBlueprint", "Blueprint"),
                                       LOCTEXT("PaneBlueprintTip",
                                               "The design-time widget tree "
                                               "of the manager's selected "
                                               "blueprint - what the LLM "
                                               "edits."))]]] +
                 SVerticalBox::Slot().FillHeight(1.f)
                     [SNew(SWidgetSwitcher)
                          .WidgetIndex(this, &SUIWTSnapshotViewer::GetActivePaneIndex) +
                      SWidgetSwitcher::Slot()[BuildTreePanel()] +
                      SWidgetSwitcher::Slot()[BuildDesignPanel()]]] +
            SSplitter::Slot().Value(0.6f)
                [SNew(SOverlay) +
                 SOverlay::Slot()
                     [SAssignNew(ImagePanel, SUIWTSnapshotImage)
                          .OnNodesHovered(
                              FUIWTOnNodesPicked::CreateSP(
                                  this, &SUIWTSnapshotViewer::HandleNodesHovered))
                          .OnNodesCommitted(FUIWTOnNodesPicked::CreateSP(
                              this, &SUIWTSnapshotViewer::HandleNodesCommitted))] +
                 SOverlay::Slot()
                     .HAlign(HAlign_Center)
                     .VAlign(VAlign_Center)
                         [SNew(STextBlock)
                              .Visibility(this,
                                          &SUIWTSnapshotViewer::GetEmptyStateVisibility)
                              .Justification(ETextJustify::Center)
                              .AutoWrapText(true)
                              .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                              .Text(LOCTEXT(
                                  "EmptyState",
                                  "Load a .widgetsnapshot, or select a "
                                  "checkpoint in the manager and press Load "
                                  "Snapshot."))]]] +
       SVerticalBox::Slot().AutoHeight().Padding(FMargin(4.f, 2.f))
           [SNew(STextBlock)
                .Text(this, &SUIWTSnapshotViewer::GetStatusText)
                .AutoWrapText(true)
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())]];

  Canvas = ImagePanel->GetCanvas();
}

TSharedRef<SWidget> SUIWTSnapshotViewer::BuildTreePanel()
{
  return SNew(SVerticalBox) +
         SVerticalBox::Slot().AutoHeight()
             [SNew(SBox).HeightOverride(28.f).Padding(FMargin(2.f))
                  [SNew(SHorizontalBox) +
                   SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 2.f, 0.f))[SNew(SComboButton).ComboButtonStyle(FAppStyle::Get(), "SimpleComboButton").ToolTipText(LOCTEXT("ColumnsTip", "Choose which columns the hierarchy shows.")).OnGetMenuContent(this, &SUIWTSnapshotViewer::BuildColumnMenu).ButtonContent()[SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 4.f, 0.f))[SNew(SImage).Image(FAppStyle::Get().GetBrush("Icons.Filter")).ColorAndOpacity(FSlateColor::UseForeground())] + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(STextBlock).Text(LOCTEXT("ColumnsBtn", "Columns"))]]] +
                   SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 2.f, 0.f))[SAssignNew(SearchBox, SSearchBox).HintText(LOCTEXT("SearchHint", "Search widgets")).OnTextChanged(this, &SUIWTSnapshotViewer::OnSearchTextChanged)] +
                   SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                       [SNew(SComboBox<TSharedPtr<int32>>)
                            .Visibility(
                                this,
                                &SUIWTSnapshotViewer::GetRootComboVisibility)
                            .OptionsSource(&RootOptions)
                            .OnGenerateWidget(
                                this,
                                &SUIWTSnapshotViewer::OnGenerateRootWidget)
                            .OnSelectionChanged(
                                this,
                                &SUIWTSnapshotViewer::OnRootSelectionChanged)
                                [SNew(STextBlock)
                                     .Text(this,
                                           &SUIWTSnapshotViewer::
                                               GetRootComboText)]]]] +
         SVerticalBox::Slot().FillHeight(1.f)
             [SAssignNew(TreeView, STreeView<TSharedRef<FUIWTSnapshotNode>>)
                  .Visibility(this, &SUIWTSnapshotViewer::GetTreeVisibility)
                  .TreeItemsSource(&TreeSource)
                  .SelectionMode(ESelectionMode::Multi)
                  .OnGenerateRow(this, &SUIWTSnapshotViewer::OnGenerateRow)
                  .OnGetChildren(this, &SUIWTSnapshotViewer::OnGetChildren)
                  .OnSelectionChanged(
                      this, &SUIWTSnapshotViewer::OnTreeSelectionChanged)
                  .OnMouseButtonDoubleClick(
                      this, &SUIWTSnapshotViewer::OnTreeDoubleClick)
                  .HeaderRow(BuildHeaderRow())];
}

TSharedRef<SHeaderRow> SUIWTSnapshotViewer::BuildHeaderRow()
{
  SAssignNew(HeaderRow, SHeaderRow)
      .CanSelectGeneratedColumn(true)
      .HiddenColumnsList(LoadHiddenColumns())
      .OnHiddenColumnsListChanged(FSimpleDelegate::CreateSP(
          this, &SUIWTSnapshotViewer::SaveHiddenColumns));

  const FSlateFontInfo HeaderFont =
      FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText").Font;
  const TSharedRef<FSlateFontMeasure> FontMeasure =
      FSlateApplication::Get().GetRenderer()->GetFontMeasureService();

  for (const UIWTSnapshotColumns::FColumnInfo &Column :
       UIWTSnapshotColumns::GetAll())
  {
    SHeaderRow::FColumn::FArguments Args =
        SHeaderRow::Column(Column.Id).DefaultLabel(Column.Label);

    if (Column.FillWidth > 0.f)
    {
      Args.FillWidth(Column.FillWidth);
    }
    else
    {
      const float LabelWidth =
          FontMeasure->Measure(Column.Label, HeaderFont).X +
          HeaderLabelAllowance;
      Args.FixedWidth(FMath::Max(Column.FixedWidth, LabelWidth));
    }

    if (!CanToggleColumn(Column.Id))
    {
      Args.ShouldGenerateWidget(true);
    }

    HeaderRow->AddColumn(Args);
  }

  return HeaderRow.ToSharedRef();
}

TSharedRef<SWidget> SUIWTSnapshotViewer::BuildColumnMenu()
{
  FMenuBuilder MenuBuilder(false, nullptr);

  MenuBuilder.BeginSection("SnapshotColumns",
                           LOCTEXT("ColumnsSection", "Select Columns"));

  for (const UIWTSnapshotColumns::FColumnInfo &Column :
       UIWTSnapshotColumns::GetAll())
  {
    const FText ToolTip =
        CanToggleColumn(Column.Id)
            ? FText::GetEmpty()
            : LOCTEXT("ColumnPinnedTip",
                      "Always shown: this column carries the tree's "
                      "expander arrow.");

    MenuBuilder.AddMenuEntry(
        Column.Label, ToolTip, FSlateIcon(),
        FUIAction(FExecuteAction::CreateSP(this,
                                           &SUIWTSnapshotViewer::ToggleColumn,
                                           Column.Id),
                  FCanExecuteAction::CreateSP(
                      this, &SUIWTSnapshotViewer::CanToggleColumn, Column.Id),
                  FIsActionChecked::CreateSP(
                      this, &SUIWTSnapshotViewer::IsColumnVisible, Column.Id)),
        NAME_None, EUserInterfaceActionType::ToggleButton);
  }

  MenuBuilder.EndSection();
  return MenuBuilder.MakeWidget();
}

bool SUIWTSnapshotViewer::IsColumnVisible(FName InColumnId) const
{
  return HeaderRow.IsValid() && HeaderRow->ShouldGeneratedColumn(InColumnId);
}

// The name column carries the expander arrow, so it can never be hidden.
bool SUIWTSnapshotViewer::CanToggleColumn(FName InColumnId) const
{
  return InColumnId != UIWTSnapshotColumns::WidgetName;
}

void SUIWTSnapshotViewer::ToggleColumn(FName InColumnId)
{
  if (!HeaderRow.IsValid())
  {
    return;
  }
  HeaderRow->SetShowGeneratedColumn(InColumnId,
                                    !HeaderRow->ShouldGeneratedColumn(InColumnId));
  SaveHiddenColumns();
}

TArray<FName> SUIWTSnapshotViewer::LoadHiddenColumns() const
{
  TArray<FString> Stored;
  if (!GConfig || !GConfig->GetArray(ColumnConfigSection, ColumnConfigKey,
                                     Stored, GEditorPerProjectIni))
  {
    return UIWTSnapshotColumns::GetDefaultHiddenColumns();
  }

  TArray<FName> Hidden;
  for (const FString &Entry : Stored)
  {
    const FName Id(*Entry);
    if (UIWTSnapshotColumns::Find(Id) && CanToggleColumn(Id))
    {
      Hidden.Add(Id);
    }
  }
  return Hidden;
}

void SUIWTSnapshotViewer::SaveHiddenColumns()
{
  if (!HeaderRow.IsValid() || !GConfig)
  {
    return;
  }

  TArray<FString> Stored;
  for (const FName &Id : HeaderRow->GetHiddenColumnIds())
  {
    Stored.Add(Id.ToString());
  }

  GConfig->SetArray(ColumnConfigSection, ColumnConfigKey, Stored,
                    GEditorPerProjectIni);
  GConfig->Flush(false, GEditorPerProjectIni);
}

bool SUIWTSnapshotViewer::LoadSnapshot(const FString &InFilePath,
                                       FText &OutError)
{
  TSharedPtr<FUIWTSnapshotDocument> NewDocument =
      MakeShared<FUIWTSnapshotDocument>();
  if (!UIWTSnapshotReader::LoadSnapshotDocument(InFilePath, *NewDocument,
                                                OutError))
  {
    return false;
  }

  ApplyDocument(NewDocument, InFilePath);
  return true;
}

void SUIWTSnapshotViewer::ClearSnapshot()
{
  if (Document.IsValid())
  {
    ApplyDocument(nullptr, FString());
  }
}

bool SUIWTSnapshotViewer::IsShowingSnapshot(const FString &InFilePath) const
{
  return !LoadedSnapshotPath.IsEmpty() && LoadedSnapshotPath == InFilePath &&
         IFileManager::Get().GetTimeStamp(*InFilePath) == LoadedSnapshotTimestamp;
}

void SUIWTSnapshotViewer::ApplyDocument(
    const TSharedPtr<FUIWTSnapshotDocument> &InDocument,
    const FString &InFilePath)
{
  // Selection and expansion are keyed on the old nodes; drop them before
  // the nodes go, or the tree keeps stale entries it can never show.
  SetSelection({}, false);
  TreeView->ClearExpandedItems();

  Document = InDocument;
  LoadedSnapshotPath = InFilePath;
  LoadedSnapshotTimestamp = InFilePath.IsEmpty()
                                ? FDateTime::MinValue()
                                : IFileManager::Get().GetTimeStamp(*InFilePath);
  RootIndex = Document.IsValid() && Document->Roots.Num() > 0 ? 0 : INDEX_NONE;

  SearchText.Reset();
  if (SearchBox.IsValid())
  {
    SearchBox->SetText(FText::GetEmpty());
  }

  RootOptions.Reset();
  if (Document.IsValid())
  {
    for (int32 Index = 0; Index < Document->Roots.Num(); ++Index)
    {
      RootOptions.Add(MakeShared<int32>(Index));
    }
  }

  BuildParentMap();
  RebuildTreeSource();

  // SetDocument lands the canvas on the same first root the viewer chose.
  Canvas->SetDocument(Document);
}

void SUIWTSnapshotViewer::BuildParentMap()
{
  ParentMap.Reset();
  if (!Document.IsValid())
  {
    return;
  }
  for (const FUIWTSnapshotRoot &Root : Document->Roots)
  {
    if (Root.Node.IsValid())
    {
      BuildParentMapRecursive(Root.Node.ToSharedRef());
    }
  }
}

void SUIWTSnapshotViewer::BuildParentMapRecursive(
    const TSharedRef<FUIWTSnapshotNode> &InNode)
{
  for (const TSharedRef<FUIWTSnapshotNode> &Child : InNode->ChildNodes)
  {
    ParentMap.Add(&Child.Get(), InNode);
    BuildParentMapRecursive(Child);
  }
}

void SUIWTSnapshotViewer::RebuildTreeSource()
{
  TreeSource.Reset();

  const FUIWTSnapshotRoot *Root =
      Document.IsValid() ? Document->GetRoot(RootIndex) : nullptr;
  if (Root && Root->Node.IsValid())
  {
    TreeSource.Add(Root->Node.ToSharedRef());
  }

  TreeView->RequestTreeRefresh();
  for (const TSharedRef<FUIWTSnapshotNode> &Node : TreeSource)
  {
    TreeView->SetItemExpansion(Node, true);
  }
}

bool SUIWTSnapshotViewer::SubtreeMatchesSearch(
    const TSharedRef<FUIWTSnapshotNode> &InNode) const
{
  if (SearchText.IsEmpty() ||
      InNode->WidgetTypeAndShortName.ToString().Contains(SearchText) ||
      InNode->WidgetReadableLocation.ToString().Contains(SearchText))
  {
    return true;
  }
  return InNode->ChildNodes.ContainsByPredicate(
      [this](const TSharedRef<FUIWTSnapshotNode> &Child)
      { return SubtreeMatchesSearch(Child); });
}

void SUIWTSnapshotViewer::OnSearchTextChanged(const FText &InText)
{
  SearchText = InText.ToString().TrimStartAndEnd();
  TreeView->RequestTreeRefresh();
}

TSharedRef<ITableRow>
SUIWTSnapshotViewer::OnGenerateRow(TSharedRef<FUIWTSnapshotNode> Item,
                                   const TSharedRef<STableViewBase> &Owner)
{
  return SNew(SUIWTSnapshotTreeRow, Owner).Node(Item);
}

void SUIWTSnapshotViewer::OnGetChildren(
    TSharedRef<FUIWTSnapshotNode> Item,
    TArray<TSharedRef<FUIWTSnapshotNode>> &OutChildren)
{
  for (const TSharedRef<FUIWTSnapshotNode> &Child : Item->ChildNodes)
  {
    if (SubtreeMatchesSearch(Child))
    {
      OutChildren.Add(Child);
    }
  }
}

void SUIWTSnapshotViewer::OnTreeSelectionChanged(
    TSharedPtr<FUIWTSnapshotNode>, ESelectInfo::Type)
{
  if (!bSuppressSelectionCallback)
  {
    SetSelection(TreeView->GetSelectedItems(), true);
  }
}

void SUIWTSnapshotViewer::OnTreeDoubleClick(TSharedRef<FUIWTSnapshotNode> Item)
{
  Canvas->FocusOnNode(Item);
}

void SUIWTSnapshotViewer::SetSelection(
    const TArray<TSharedRef<FUIWTSnapshotNode>> &InNodes, bool bFromTree)
{
  Canvas->SetSelection(InNodes);

  if (bFromTree)
  {
    return;
  }

  TGuardValue<bool> Guard(bSuppressSelectionCallback, true);
  TreeView->ClearSelection();
  if (InNodes.Num() > 0)
  {
    const TSharedRef<FUIWTSnapshotNode> Leaf = InNodes.Last();
    ExpandToNode(Leaf);
    TreeView->SetItemSelection(Leaf, true, ESelectInfo::Direct);
    TreeView->RequestScrollIntoView(Leaf);
  }
}

void SUIWTSnapshotViewer::ExpandToNode(
    const TSharedRef<FUIWTSnapshotNode> &InNode)
{
  TArray<TSharedRef<FUIWTSnapshotNode>> Chain;
  const FUIWTSnapshotNode *Current = &InNode.Get();
  while (const TSharedRef<FUIWTSnapshotNode> *Parent = ParentMap.Find(Current))
  {
    Chain.Add(*Parent);
    Current = &Parent->Get();
  }

  for (int32 Index = Chain.Num() - 1; Index >= 0; --Index)
  {
    TreeView->SetItemExpansion(Chain[Index], true);
  }
}

void SUIWTSnapshotViewer::HandleNodesHovered(
    const TArray<TSharedRef<FUIWTSnapshotNode>> &InNodes)
{
  if (InNodes.Num() == 0)
  {
    return;
  }
  const TSharedRef<FUIWTSnapshotNode> Leaf = InNodes.Last();
  ExpandToNode(Leaf);
  TreeView->RequestScrollIntoView(Leaf);
}

void SUIWTSnapshotViewer::HandleNodesCommitted(
    const TArray<TSharedRef<FUIWTSnapshotNode>> &InNodes)
{
  SetSelection(InNodes, false);
  SelectDesignNode(ResolvePickedDesignNode(InNodes), true);
}

void SUIWTSnapshotViewer::OnRootSelectionChanged(TSharedPtr<int32> InIndex,
                                                 ESelectInfo::Type)
{
  if (!InIndex.IsValid() || *InIndex == RootIndex)
  {
    return;
  }

  RootIndex = *InIndex;
  SetSelection({}, false);
  RebuildTreeSource();
  Canvas->SetRootIndex(RootIndex);
}

TSharedRef<SWidget>
SUIWTSnapshotViewer::OnGenerateRootWidget(TSharedPtr<int32> InIndex) const
{
  return SNew(STextBlock)
      .Text(GetRootDisplayName(InIndex.IsValid() ? *InIndex : INDEX_NONE));
}

FText SUIWTSnapshotViewer::GetRootDisplayName(int32 InIndex) const
{
  const FUIWTSnapshotRoot *Root =
      Document.IsValid() ? Document->GetRoot(InIndex) : nullptr;
  return Root ? FText::FromString(Root->DisplayName) : FText::GetEmpty();
}

FText SUIWTSnapshotViewer::GetRootComboText() const
{
  return GetRootDisplayName(RootIndex);
}

EVisibility SUIWTSnapshotViewer::GetRootComboVisibility() const
{
  return (Document.IsValid() && Document->Roots.Num() > 1)
             ? EVisibility::Visible
             : EVisibility::Collapsed;
}

EVisibility SUIWTSnapshotViewer::GetEmptyStateVisibility() const
{
  return Document.IsValid() ? EVisibility::Collapsed
                            : EVisibility::HitTestInvisible;
}

EVisibility SUIWTSnapshotViewer::GetTreeVisibility() const
{
  return Document.IsValid() ? EVisibility::Visible : EVisibility::Hidden;
}

FText SUIWTSnapshotViewer::GetStatusText() const
{
  if (!Document.IsValid())
  {
    return LOCTEXT("StatusNoSnapshot", "No snapshot loaded.");
  }

  FString Text = FString::Printf(
      TEXT("%s  -  %d root(s), %d widget(s)"),
      *FPaths::GetCleanFilename(Document->FilePath), Document->Roots.Num(),
      Document->TotalWidgetCount());

  if (!Document->Warning.IsEmpty())
  {
    Text.Append(TEXT("  -  "));
    Text.Append(Document->Warning);
  }
  if (!DesignBlueprintName.IsEmpty() && !SnapshotMatchesDesign())
  {
    Text.Append(TEXT("  -  snapshot predates this blueprint (play and "
                     "capture again to pick from it)"));
  }

  return FText::FromString(Text);
}

// ---------------------------------------------------------------------------
// Blueprint tab: the design-time tree the LLM edits.

namespace
{
  const FName DesignColName("Name");
  const FName DesignColSlot("Slot");

  class SUIWTDesignRow : public SMultiColumnTableRow<TSharedRef<FUIWTDesignNode>>
  {
  public:
    SLATE_BEGIN_ARGS(SUIWTDesignRow) : _IsPicked(false) {}
    SLATE_ARGUMENT(TSharedPtr<FUIWTDesignNode>, Node)
    // True while this row is the widget the last image pick landed on. The
    // row marks it independently of selection, which any click moves.
    SLATE_ATTRIBUTE(bool, IsPicked)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs,
                   const TSharedRef<STableViewBase> &InOwner)
    {
      Node = InArgs._Node;
      IsPicked = InArgs._IsPicked;
      SMultiColumnTableRow<TSharedRef<FUIWTDesignNode>>::Construct(
          FSuperRowType::FArguments(), InOwner);
    }

    virtual TSharedRef<SWidget>
    GenerateWidgetForColumn(const FName &InColumn) override
    {
      FText Text;
      FSlateColor Color = FSlateColor::UseForeground();
      if (InColumn == DesignColName)
      {
        Text = FText::FromString(Node->Name);
        return SNew(SHorizontalBox) +
               SHorizontalBox::Slot().AutoWidth()[SNew(SExpanderArrow, SharedThis(this))] +
               SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 4.f, 0.f))
                   [SNew(SImage)
                        .Image(FAppStyle::Get().GetBrush("Icons.EyeDropper"))
                        .ColorAndOpacity(PickedColor())
                        .ToolTipText(LOCTEXT("PickedMarkTip",
                                             "Picked on the snapshot image; the "
                                             "target of the next prompt."))
                        .Visibility(this, &SUIWTDesignRow::GetPickedMarkVisibility)] +
               SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
                   [SNew(STextBlock)
                        .Text(Text)
                        .ColorAndOpacity(this, &SUIWTDesignRow::GetNameColor)];
      }
      if (InColumn == DesignColSlot)
      {
        Text = FText::FromString(Node->SlotClassName);
        Color = FSlateColor::UseSubduedForeground();
      }
      return SNew(STextBlock).Text(Text).ColorAndOpacity(Color);
    }

  private:
    static FSlateColor PickedColor()
    {
      return FAppStyle::Get().GetSlateColor("Colors.AccentBlue");
    }

    EVisibility GetPickedMarkVisibility() const
    {
      return IsPicked.Get() ? EVisibility::Visible : EVisibility::Collapsed;
    }

    FSlateColor GetNameColor() const
    {
      return IsPicked.Get() ? PickedColor() : FSlateColor::UseForeground();
    }

    TSharedPtr<FUIWTDesignNode> Node;
    TAttribute<bool> IsPicked;
  };

  // The widget's own children, named-slot content first the way the
  // designer lists them. Not UWidgetTree::GetChildWidgets, which gathers every
  // descendant and would repeat each nested subtree under its ancestors.
  void GetDirectChildren(UWidget *InWidget, TArray<UWidget *> &OutChildren)
  {
    if (INamedSlotInterface *NamedSlotHost = Cast<INamedSlotInterface>(InWidget))
    {
      TArray<FName> SlotNames;
      NamedSlotHost->GetSlotNames(SlotNames);
      for (const FName &SlotName : SlotNames)
      {
        if (UWidget *Content = NamedSlotHost->GetContentForSlot(SlotName))
        {
          OutChildren.Add(Content);
        }
      }
    }
    if (UPanelWidget *Panel = Cast<UPanelWidget>(InWidget))
    {
      for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
      {
        if (UWidget *Child = Panel->GetChildAt(Index))
        {
          OutChildren.Add(Child);
        }
      }
    }
  }

  TSharedRef<FUIWTDesignNode>
  BuildDesignNode(UWidget *InWidget,
                  TArray<TSharedRef<FUIWTDesignNode>> &OutAll)
  {
    TSharedRef<FUIWTDesignNode> Node = MakeShared<FUIWTDesignNode>();
    Node->Name = InWidget->GetName();
    if (InWidget->Slot)
    {
      Node->SlotClassName = InWidget->Slot->GetClass()->GetName();
    }
    OutAll.Add(Node);

    TArray<UWidget *> Children;
    GetDirectChildren(InWidget, Children);
    for (UWidget *Child : Children)
    {
      Node->Children.Add(BuildDesignNode(Child, OutAll));
    }
    return Node;
  }
}

TSharedRef<SWidget> SUIWTSnapshotViewer::BuildDesignPanel()
{
  return SNew(SVerticalBox) +
         SVerticalBox::Slot().AutoHeight()
             [SNew(SBox).HeightOverride(28.f).Padding(FMargin(2.f))
                  [SAssignNew(DesignSearchBox, SSearchBox)
                       .HintText(LOCTEXT("DesignSearchHint", "Search widgets"))
                       .OnTextChanged(
                           this, &SUIWTSnapshotViewer::OnDesignSearchTextChanged)]] +
         SVerticalBox::Slot().FillHeight(1.f)
             [SAssignNew(DesignTreeView, STreeView<TSharedRef<FUIWTDesignNode>>)
                  .TreeItemsSource(&DesignRoots)
                  .SelectionMode(ESelectionMode::Single)
                  .OnGenerateRow(this, &SUIWTSnapshotViewer::OnGenerateDesignRow)
                  .OnGetChildren(this, &SUIWTSnapshotViewer::OnGetDesignChildren)
                  .OnSelectionChanged(
                      this, &SUIWTSnapshotViewer::OnDesignSelectionChanged)
                  .HeaderRow(
                      SNew(SHeaderRow) +
                      SHeaderRow::Column(DesignColName)
                          .DefaultLabel(LOCTEXT("DesignHName", "Name"))
                          .FillWidth(0.6f) +
                      SHeaderRow::Column(DesignColSlot)
                          .DefaultLabel(LOCTEXT("DesignHSlot", "Slot"))
                          .FillWidth(0.35f))];
}

void SUIWTSnapshotViewer::OnDesignSearchTextChanged(const FText &InText)
{
  DesignSearchText = InText.ToString().TrimStartAndEnd();
  if (DesignTreeView.IsValid())
  {
    DesignTreeView->RequestTreeRefresh();
  }
}

bool SUIWTSnapshotViewer::DesignSubtreeMatchesSearch(
    const TSharedRef<FUIWTDesignNode> &InNode) const
{
  if (DesignSearchText.IsEmpty() || InNode->Name.Contains(DesignSearchText))
  {
    return true;
  }
  return InNode->Children.ContainsByPredicate(
      [this](const TSharedRef<FUIWTDesignNode> &Child)
      { return DesignSubtreeMatchesSearch(Child); });
}


void SUIWTSnapshotViewer::SetDesignBlueprint(
    UWidgetBlueprint *InBlueprint, const TArray<FString> &InAcceptedAssetPaths)
{
  const FString NewPath = InBlueprint ? InBlueprint->GetPathName() : FString();
  const bool bSameBlueprint =
      !NewPath.IsEmpty() && NewPath == DesignBlueprintPath;

  // The old nodes are about to go; the tree must not keep selection or
  // expansion keyed on them.
  if (DesignTreeView.IsValid())
  {
    TGuardValue<bool> Guard(bSuppressDesignSelection, true);
    DesignTreeView->ClearSelection();
    DesignTreeView->ClearExpandedItems();
  }

  DesignRoots.Reset();
  AllDesignNodes.Reset();
  AcceptedAssetPaths = InAcceptedAssetPaths;
  DesignBlueprintName = InBlueprint ? InBlueprint->GetName() : FString();
  DesignBlueprintPath = NewPath;

  if (InBlueprint && InBlueprint->WidgetTree &&
      InBlueprint->WidgetTree->RootWidget)
  {
    DesignRoots.Add(
        BuildDesignNode(InBlueprint->WidgetTree->RootWidget, AllDesignNodes));
  }

  // A search is about one blueprint's names; it stays through that
  // blueprint's edits and goes with the blueprint.
  if (!bSameBlueprint)
  {
    DesignSearchText.Reset();
    if (DesignSearchBox.IsValid())
    {
      DesignSearchBox->SetText(FText::GetEmpty());
    }
  }

  if (DesignTreeView.IsValid())
  {
    DesignTreeView->RequestTreeRefresh();
    for (const TSharedRef<FUIWTDesignNode> &Node : AllDesignNodes)
    {
      DesignTreeView->SetItemExpansion(Node, true);
    }
  }

  // The same blueprint comes back after every edit: keep the pick, and its
  // image-pick mark, if the widget survived. Anything else is a new context,
  // so the pick is stale.
  SelectDesignNode(bSameBlueprint ? FindDesignNode(PickedWidget) : nullptr,
                   bPickedFromImage);
}

void SUIWTSnapshotViewer::SelectDesignNode(
    const TSharedPtr<FUIWTDesignNode> &InNode, bool bFromImage)
{
  PickedWidget = InNode.IsValid() ? InNode->Name : FString();
  bPickedFromImage = InNode.IsValid() && bFromImage;
  if (!DesignTreeView.IsValid())
  {
    return;
  }
  TGuardValue<bool> Guard(bSuppressDesignSelection, true);
  DesignTreeView->ClearSelection();
  if (InNode.IsValid())
  {
    ExpandAllDesignNodes();
    DesignTreeView->SetItemSelection(InNode.ToSharedRef(), true,
                                     ESelectInfo::Direct);
    DesignTreeView->RequestScrollIntoView(InNode.ToSharedRef());
  }
}

TSharedRef<ITableRow> SUIWTSnapshotViewer::OnGenerateDesignRow(
    TSharedRef<FUIWTDesignNode> Item, const TSharedRef<STableViewBase> &Owner)
{
  return SNew(SUIWTDesignRow, Owner)
      .Node(Item)
      .IsPicked(this, &SUIWTSnapshotViewer::IsDesignNodePicked, Item);
}

bool SUIWTSnapshotViewer::IsDesignNodePicked(
    TSharedRef<FUIWTDesignNode> InNode) const
{
  return bPickedFromImage && InNode->Name == PickedWidget;
}

void SUIWTSnapshotViewer::OnGetDesignChildren(
    TSharedRef<FUIWTDesignNode> Item,
    TArray<TSharedRef<FUIWTDesignNode>> &OutChildren)
{
  for (const TSharedRef<FUIWTDesignNode> &Child : Item->Children)
  {
    if (DesignSubtreeMatchesSearch(Child))
    {
      OutChildren.Add(Child);
    }
  }
}

// Selecting a design widget highlights the runtime nodes built for it and
// records it as the picked widget for the next run.
void SUIWTSnapshotViewer::OnDesignSelectionChanged(
    TSharedPtr<FUIWTDesignNode> Item, ESelectInfo::Type SelectInfo)
{
  if (bSuppressDesignSelection)
  {
    return;
  }
  PickedWidget = Item.IsValid() ? Item->Name : FString();
  // The pick now comes from the tree, so the image-pick mark would only
  // point at a widget that is no longer the target.
  bPickedFromImage = false;

  if (!Item.IsValid() || !Document.IsValid())
  {
    return;
  }
  TArray<TSharedRef<FUIWTSnapshotNode>> Matches;
  for (const FUIWTSnapshotRoot &Root : Document->Roots)
  {
    if (Root.Node.IsValid())
    {
      CollectRuntimeNodes(Root.Node.ToSharedRef(), Item->Name, Matches);
    }
  }
  if (Matches.Num() > 0)
  {
    // The topmost match is the UMG widget itself; deeper ones are its
    // internals.
    SetSelection({Matches[0]}, false);
  }
}

void SUIWTSnapshotViewer::CollectRuntimeNodes(
    const TSharedRef<FUIWTSnapshotNode> &InNode, const FString &InUMGName,
    TArray<TSharedRef<FUIWTSnapshotNode>> &OutNodes) const
{
  if (InNode->UMGName == InUMGName &&
      AcceptedAssetPaths.Contains(InNode->AssetPathString))
  {
    OutNodes.Add(InNode);
  }
  for (const TSharedRef<FUIWTSnapshotNode> &Child : InNode->ChildNodes)
  {
    CollectRuntimeNodes(Child, InUMGName, OutNodes);
  }
}

TSharedPtr<FUIWTDesignNode> SUIWTSnapshotViewer::ResolvePickedDesignNode(
    const TArray<TSharedRef<FUIWTSnapshotNode>> &InNodes) const
{
  if (InNodes.Num() == 0 || AcceptedAssetPaths.Num() == 0)
  {
    return nullptr;
  }
  const FUIWTSnapshotNode *Current = &InNodes.Last().Get();
  while (Current)
  {
    if (!Current->UMGName.IsEmpty() &&
        AcceptedAssetPaths.Contains(Current->AssetPathString))
    {
      if (TSharedPtr<FUIWTDesignNode> Design = FindDesignNode(Current->UMGName))
      {
        return Design;
      }
    }
    const TSharedRef<FUIWTSnapshotNode> *Parent = ParentMap.Find(Current);
    Current = Parent ? &Parent->Get() : nullptr;
  }
  // Inside the user widget but on none of its child widgets (its own
  // SObjectWidget carries the instance name, not a tree name): nothing to
  // mark. The root would be a guess, not what was picked.
  return nullptr;
}

TSharedPtr<FUIWTDesignNode>
SUIWTSnapshotViewer::FindDesignNode(const FString &InName) const
{
  if (InName.IsEmpty())
  {
    return nullptr;
  }
  for (const TSharedRef<FUIWTDesignNode> &Node : AllDesignNodes)
  {
    if (Node->Name == InName)
    {
      return Node;
    }
  }
  return nullptr;
}

void SUIWTSnapshotViewer::ExpandAllDesignNodes()
{
  // Design trees are small; expanding everything keeps the picked node
  // visible without a parent map.
  for (const TSharedRef<FUIWTDesignNode> &Node : AllDesignNodes)
  {
    DesignTreeView->SetItemExpansion(Node, true);
  }
}

// True when the snapshot contains widgets of one of the accepted assets -
// i.e. it was captured from this blueprint or its root.
bool SUIWTSnapshotViewer::SnapshotMatchesDesign() const
{
  if (!Document.IsValid() || AcceptedAssetPaths.Num() == 0)
  {
    return true;
  }
  TArray<const FUIWTSnapshotNode *> Stack;
  for (const FUIWTSnapshotRoot &Root : Document->Roots)
  {
    if (Root.Node.IsValid())
    {
      Stack.Add(Root.Node.Get());
    }
  }
  while (Stack.Num() > 0)
  {
    const FUIWTSnapshotNode *Node = Stack.Pop();
    if (AcceptedAssetPaths.Contains(Node->AssetPathString))
    {
      return true;
    }
    for (const TSharedRef<FUIWTSnapshotNode> &Child : Node->ChildNodes)
    {
      Stack.Add(&*Child);
    }
  }
  return false;
}

TSharedRef<SWidget> SUIWTSnapshotViewer::MakePaneTab(EPane InPane,
                                                     const FText &InLabel,
                                                     const FText &InToolTip)
{
  return SNew(SCheckBox)
      .Style(FAppStyle::Get(), "ToolPalette.DockingTab")
      .ToolTipText(InToolTip)
      .IsChecked(this, &SUIWTSnapshotViewer::GetPaneCheckState, InPane)
      .OnCheckStateChanged(this, &SUIWTSnapshotViewer::OnPaneTabChanged,
                           InPane)[SNew(STextBlock)
                                       .TextStyle(FAppStyle::Get(),
                                                  "ToolPalette.DockingLabel")
                                       .Text(InLabel)];
}

ECheckBoxState SUIWTSnapshotViewer::GetPaneCheckState(EPane InPane) const
{
  return ActivePane == InPane ? ECheckBoxState::Checked
                              : ECheckBoxState::Unchecked;
}

void SUIWTSnapshotViewer::OnPaneTabChanged(ECheckBoxState InState,
                                           EPane InPane)
{
  // Clicking the active tab unchecks it; it stays the active pane.
  if (InState == ECheckBoxState::Checked)
  {
    ActivePane = InPane;
  }
}

#undef LOCTEXT_NAMESPACE
