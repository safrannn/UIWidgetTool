#include "UIWTDesignRefine.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Core/UIWTDesignImport.h"
#include "Core/UIWTJson.h"
#include "Core/UIWTLocalSettings.h"
#include "Core/UIWTRunImages.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

namespace
{
  using namespace UIWTDesignTree;

  // import-tree.md → Cost: with and without the Claude pass. Tokens.
  constexpr int64 FixedContext = 25000;
  constexpr int64 ImageTokens = 3000;
  constexpr int64 OutlinePerNode = 19;
  constexpr int64 SpecPerNode = 110;
  constexpr int64 PatchTokens = 5000;
  constexpr int64 ThinkingTokens = 5000;
  // Each round is one apply and two follow-up calls (render, zoom or
  // GetDesignNode), each with a short reply, and some tool results.
  constexpr int64 CallsPerRound = 3;
  constexpr int64 FollowUpTokens = 1000;
  constexpr int64 ToolResultTokens = 2000;
  // A reading run's tree (import-image.md, section 0: the spike measured
  // 73-78 tokens per node; with the hints and runs Claude adds, about 110),
  // and the expected density of an image: design pixels per node (the
  // spike's screens had 10-41 nodes at 1920 x 1080).
  constexpr int64 TreeTokensPerNode = 110;
  constexpr double DesignAreaPerNode = 60000.0;
  // Dollars per million tokens.
  constexpr double CacheReadPrice = 0.20;

  // Report entries shown in the request; the rest are counted.
  constexpr int32 MaxReportEntries = 150;

  const TCHAR *KindName(EKind InKind)
  {
    switch (InKind)
    {
    case EKind::Group:
      return TEXT("group");
    case EKind::Instance:
      return TEXT("instance");
    case EKind::Text:
      return TEXT("text");
    case EKind::Shape:
      return TEXT("shape");
    case EKind::Image:
      return TEXT("image");
    default:
      return TEXT("frame");
    }
  }

  // Rounded to a tenth and written short: the JSON writer would print 10.1
  // as 10.099999999999999, which Claude reads as tokens.
  TSharedRef<FJsonValue> Short(double InValue)
  {
    return MakeShared<FJsonValueNumberString>(
        FString::SanitizeFloat(FMath::RoundToDouble(InValue * 10.0) / 10.0, 0));
  }

  TSharedRef<FJsonObject> OutlineNode(const FNode &InNode, const FOwnedMap &InOwned)
  {
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("id"), InNode.Id);
    Object->SetStringField(TEXT("n"), InNode.Name);
    Object->SetStringField(TEXT("k"), KindName(InNode.Kind));
    TArray<TSharedPtr<FJsonValue>> Box;
    for (const double Value : {InNode.Box.X, InNode.Box.Y, InNode.Box.W, InNode.Box.H})
    {
      Box.Add(Short(Value));
    }
    Object->SetArrayField(TEXT("b"), Box);
    if (const FRoleNames *Roles = InOwned.Find(InNode.Id))
    {
      if (const FString *Main = Roles->Find(TEXT("main")))
      {
        Object->SetStringField(TEXT("w"), *Main);
      }
    }
    if (!InNode.bVisible)
    {
      Object->SetBoolField(TEXT("v"), false);
    }
    if (!InNode.Hints.IsEmpty())
    {
      TArray<TSharedPtr<FJsonValue>> Hints;
      for (const FString &Hint : InNode.Hints)
      {
        Hints.Add(MakeShared<FJsonValueString>(Hint));
      }
      Object->SetArrayField(TEXT("h"), Hints);
    }
    if (!InNode.Children.IsEmpty())
    {
      TArray<TSharedPtr<FJsonValue>> Children;
      for (const FNode &Child : InNode.Children)
      {
        Children.Add(MakeShared<FJsonValueObject>(OutlineNode(Child, InOwned)));
      }
      Object->SetArrayField(TEXT("c"), Children);
    }
    return Object;
  }

  int32 CountNodes(const FNode &InNode)
  {
    int32 Count = 1;
    for (const FNode &Child : InNode.Children)
    {
      Count += CountNodes(Child);
    }
    return Count;
  }

  const FNode *FindIn(const FNode &InNode, const FString &InId)
  {
    if (InNode.Id == InId)
    {
      return &InNode;
    }
    for (const FNode &Child : InNode.Children)
    {
      if (const FNode *Found = FindIn(Child, InId))
      {
        return Found;
      }
    }
    return nullptr;
  }

  // A part of the scope: a scoped node with no scoped ancestor, its box in
  // root coordinates and its outermost widget.
  struct FScopeRoot
  {
    const FNode *Node = nullptr;
    FRect Abs;
    FString Widget;
  };

  // Design pixels around the scope's parts in the cropped reference, and the
  // share of the reference above which it's sent whole.
  constexpr double CropMargin = 8.0;
  constexpr double MaxCropShare = 0.8;

  void CollectScope(const FNode &InNode, const TSet<FString> &InScope, const FVector2D &InOrigin,
                    bool bInInside, TArray<FScopeRoot> &OutRoots)
  {
    const FVector2D Position = InOrigin + FVector2D(InNode.Box.X, InNode.Box.Y);
    bool bInside = bInInside;
    if (!bInside && InScope.Contains(InNode.Id))
    {
      bInside = true;
      FScopeRoot &Root = OutRoots.AddDefaulted_GetRef();
      Root.Node = &InNode;
      Root.Abs = {Position.X, Position.Y, InNode.Box.W, InNode.Box.H};
    }
    for (const FNode &Child : InNode.Children)
    {
      CollectScope(Child, InScope, Position, bInside, OutRoots);
    }
  }

  void CollectIds(const FNode &InNode, TSet<FString> &OutIds)
  {
    OutIds.Add(InNode.Id);
    for (const FNode &Child : InNode.Children)
    {
      CollectIds(Child, OutIds);
    }
  }

  // The widget of the node's chain that holds the others: the one nearest
  // the tree's root. Empty when none of its widgets is left.
  FString OutermostWidget(const UWidgetBlueprint *InBlueprint, const FOwnedMap &InOwned,
                          const FString &InNode)
  {
    const FRoleNames *Roles = InOwned.Find(InNode);
    FString Best;
    if (!Roles)
    {
      return Best;
    }
    int32 BestDepth = MAX_int32;
    for (const TPair<FString, FString> &Role : *Roles)
    {
      const UWidget *Widget = InBlueprint->WidgetTree->FindWidget(FName(*Role.Value));
      if (!Widget)
      {
        continue;
      }
      int32 Depth = 0;
      for (const UWidget *Parent = Widget->GetParent(); Parent; Parent = Parent->GetParent())
      {
        ++Depth;
      }
      if (Depth < BestDepth)
      {
        BestDepth = Depth;
        Best = Role.Value;
      }
    }
    return Best;
  }

  void Truncate(FNode &InOutNode, int32 InDepth, TMap<FString, int32> &OutHidden)
  {
    if (InDepth <= 0)
    {
      if (!InOutNode.Children.IsEmpty())
      {
        OutHidden.Add(InOutNode.Id, InOutNode.Children.Num());
        InOutNode.Children.Empty();
      }
      return;
    }
    for (FNode &Child : InOutNode.Children)
    {
      Truncate(Child, InDepth - 1, OutHidden);
    }
  }

  void MarkHidden(const TSharedPtr<FJsonObject> &InNode, const TMap<FString, int32> &InHidden)
  {
    if (!InNode.IsValid())
    {
      return;
    }
    FString Id;
    if (InNode->TryGetStringField(TEXT("id"), Id))
    {
      if (const int32 *Count = InHidden.Find(Id))
      {
        InNode->SetNumberField(TEXT("hiddenChildren"), *Count);
      }
    }
    const TArray<TSharedPtr<FJsonValue>> *Children = nullptr;
    if (InNode->TryGetArrayField(TEXT("children"), Children))
    {
      for (const TSharedPtr<FJsonValue> &Child : *Children)
      {
        MarkHidden(Child->AsObject(), InHidden);
      }
    }
  }

  // What the pass does, by source (import-figma.md step 5, import-ps.md
  // step 4).
  const TCHAR *FigmaTasks = TEXT(
      "- Buttons: frames, instances and groups that are clearly clickable (named like a "
      "button, hint prefix:btn, a label on a filled plate) become Buttons: the plate's look "
      "becomes the Button style (normal; hovered and pressed from the design's variant layers "
      "when it has them, otherwise the normal look slightly lighter and darker), and its "
      "content becomes the Button's child. The content widgets keep their names.\n"
      "- Lists and scroll areas: a column or row of repeated items that overflows its frame "
      "or is named like a list goes into a ScrollBox.\n"
      "- Collapse wrappers that do nothing: a SizeBox or panel with one child and no size, "
      "padding or look of its own.\n"
      "- Names: clean names (Btn_Play, Txt_Title, Img_Avatar, List_Items), and \"variable\": "
      "true on the widgets the graph will need (buttons, inputs, texts and images that change "
      "at runtime).\n"
      "- Auto layout already made the rows, columns, sizing and anchors: keep them.\n");

  const TCHAR *PsdTasks = TEXT(
      "- Layout: a Photoshop draft is absolute CanvasPanels with top-left anchors. Where a "
      "canvas's children are clearly a row or a column, replace it with a HorizontalBox or "
      "VerticalBox, with the measured gaps as slot padding. Give what stays on a canvas the "
      "anchors it belongs to (the screen edge or centre it sits at).\n"
      "- Buttons: layer groups that are buttons (named btn_ or button_, hint prefix:btn, a "
      "label over a plate, or normal / hover / pressed layers) become Buttons. Merge the state "
      "layers into the Button style: the normal layer's image or look is the normal brush, "
      "the hover and pressed layers' the hovered and pressed brushes; those layers' own "
      "widgets are then left out. The label keeps its name.\n"
      "- Lists and scroll areas: a column or row of repeated items that overflows its area or "
      "is named like a list (list_) goes into a ScrollBox.\n"
      "- Names: clean names (Btn_Play, Txt_Title, Img_Avatar, List_Items), and \"variable\": "
      "true on the widgets the graph will need (buttons, inputs, texts and images that change "
      "at runtime).\n");
}

UIWTDesignRefine::EPrices UIWTDesignRefine::PricesFor(EUIWTClaudeModel InModel,
                                                      const FString &InCustomModel)
{
  switch (InModel)
  {
  case EUIWTClaudeModel::Opus:
    return EPrices::Opus;
  case EUIWTClaudeModel::Sonnet:
    return EPrices::Sonnet;
  case EUIWTClaudeModel::Custom:
    return InCustomModel.Contains(TEXT("opus"))     ? EPrices::Opus
           : InCustomModel.Contains(TEXT("sonnet")) ? EPrices::Sonnet
                                                    : EPrices::Unknown;
  default:
    return EPrices::Unknown;
  }
}

namespace
{
  // One run: InWrittenPerApply is what each round's apply writes (a patch, or
  // a whole tree), and bInReadsSpec whether the run reads an existing outline
  // and spec first (a pass does; a reading run has none yet).
  UIWTDesignRefine::FEstimate EstimateRun(int32 InNodes, int32 InRounds,
                                          UIWTDesignRefine::EPrices InPrices,
                                          int64 InWrittenPerApply, bool bInReadsSpec)
  {
    UIWTDesignRefine::FEstimate Result;
    const int64 Nodes = FMath::Max(InNodes, 1);
    const int64 Rounds = FMath::Max(InRounds, 1);
    Result.Rounds = static_cast<int32>(Rounds);
    // The first read: fixed context, the outline and the scope's spec when
    // there are any, the reference image. Each round adds what it writes,
    // thinking, replies, a render and tool results, and every call reads the
    // context so far from the cache.
    const int64 First =
        FixedContext + (bInReadsSpec ? (OutlinePerNode + SpecPerNode) * Nodes : 0) + ImageTokens;
    const int64 WrittenPerRound = InWrittenPerApply + ThinkingTokens + 2 * FollowUpTokens;
    const int64 NewPerRound = WrittenPerRound + ImageTokens + ToolResultTokens;
    Result.InputTokens = First + Rounds * NewPerRound;
    Result.OutputTokens = Rounds * WrittenPerRound;
    for (int64 Round = 0; Round < Rounds; ++Round)
    {
      Result.CachedTokens += CallsPerRound * (First + Round * NewPerRound);
    }
    double InputPrice = 0.0;
    double OutputPrice = 0.0;
    switch (InPrices)
    {
    case UIWTDesignRefine::EPrices::Sonnet:
      InputPrice = 2.0;
      OutputPrice = 10.0;
      break;
    case UIWTDesignRefine::EPrices::Opus:
      InputPrice = 4.0;
      OutputPrice = 20.0;
      break;
    default:
      return Result;
    }
    Result.Dollars = (Result.InputTokens * InputPrice + Result.CachedTokens * CacheReadPrice +
                      Result.OutputTokens * OutputPrice) /
                     1e6;
    return Result;
  }
}

UIWTDesignRefine::FEstimate UIWTDesignRefine::Estimate(int32 InNodes, int32 InRounds,
                                                       EPrices InPrices)
{
  return EstimateRun(InNodes, InRounds, InPrices, PatchTokens, true);
}

UIWTDesignRefine::FEstimate UIWTDesignRefine::EstimateReading(int32 InNodes, int32 InRounds,
                                                              EPrices InPrices, bool bInAIPass)
{
  // Each round writes the whole tree (TreeTokensPerNode per node).
  FEstimate Result = EstimateRun(InNodes, InRounds, InPrices,
                                 TreeTokensPerNode * FMath::Max(InNodes, 1), false);
  if (bInAIPass)
  {
    // The pass's rounds follow in the same session.
    const FEstimate Pass = Estimate(InNodes, InRounds, InPrices);
    Result.InputTokens += Pass.InputTokens;
    Result.CachedTokens += Pass.CachedTokens;
    Result.OutputTokens += Pass.OutputTokens;
    if (Result.Dollars >= 0.0 && Pass.Dollars >= 0.0)
    {
      Result.Dollars += Pass.Dollars;
    }
  }
  return Result;
}

int32 UIWTDesignRefine::ExpectedNodes(const FVector2D &InDesignSize)
{
  return FMath::Clamp(FMath::RoundToInt(InDesignSize.X * InDesignSize.Y / DesignAreaPerNode), 10,
                      400);
}

FString UIWTDesignRefine::MakeOutline(const FNode &InRoot, const FOwnedMap &InOwned)
{
  return UIWTJson::Condensed(OutlineNode(InRoot, InOwned));
}

const FNode *UIWTDesignRefine::FindNode(const FDocument &InDocument, const FString &InId)
{
  if (const FNode *Found = FindIn(InDocument.Root, InId))
  {
    return Found;
  }
  for (const TPair<FString, FComponentDef> &Component : InDocument.Components)
  {
    if (const FNode *Found = FindIn(Component.Value.Root, InId))
    {
      return Found;
    }
  }
  return nullptr;
}

FString UIWTDesignRefine::NodeJson(const FNode &InNode, int32 InDepth)
{
  FDocument Doc;
  Doc.Root = InNode;
  TMap<FString, int32> Hidden;
  Truncate(Doc.Root, InDepth, Hidden);
  const TSharedRef<FJsonObject> Written = WriteDocument(Doc);
  const TSharedPtr<FJsonObject> *Root = nullptr;
  if (!Written->TryGetObjectField(TEXT("root"), Root))
  {
    return FString();
  }
  MarkHidden(*Root, Hidden);
  return UIWTJson::Condensed(Root->ToSharedRef());
}

FString UIWTDesignRefine::NodeForWidget(const UWidgetBlueprint *InBlueprint,
                                        const FOwnedMap &InOwned, const FString &InWidgetName)
{
  if (!InBlueprint || !InBlueprint->WidgetTree || InWidgetName.IsEmpty())
  {
    return FString();
  }
  TMap<FString, FString> WidgetToNode;
  for (const TPair<FString, FRoleNames> &Node : InOwned)
  {
    for (const TPair<FString, FString> &Role : Node.Value)
    {
      WidgetToNode.Add(Role.Value, Node.Key);
    }
  }
  for (const UWidget *Widget = InBlueprint->WidgetTree->FindWidget(FName(*InWidgetName)); Widget;
       Widget = Widget->GetParent())
  {
    if (const FString *Node = WidgetToNode.Find(Widget->GetName()))
    {
      return *Node;
    }
  }
  return FString();
}

bool UIWTDesignRefine::LoadDesign(const UWidgetBlueprint *InBlueprint,
                                  UIWTDesignImport::FSidecar &OutSidecar, FDocument &OutDocument,
                                  FString &OutError)
{
  if (!InBlueprint)
  {
    OutError = TEXT("No Widget Blueprint given.");
    return false;
  }
  if (!UIWTDesignImport::ReadSidecar(InBlueprint->GetOutermost()->GetName(), OutSidecar,
                                     OutError))
  {
    return false;
  }
  FString Json;
  TArray<FString> Errors;
  if (!FFileHelper::LoadFileToString(Json, *OutSidecar.DesignFile) ||
      !ReadDocument(Json, OutDocument, Errors))
  {
    OutError = FString::Printf(TEXT("The design %s can't be read. %s"), *OutSidecar.DesignFile,
                               *FString::Join(Errors, TEXT("; ")));
    return false;
  }
  return true;
}

bool UIWTDesignRefine::Prepare(UWidgetBlueprint *InBlueprint, int32 InRounds,
                               const FScope &InScope, const TArray<FReportEntry> &InReport,
                               FPass &OutPass, FString &OutError)
{
  OutPass = FPass();
  if (!InBlueprint || !InBlueprint->WidgetTree || !InBlueprint->WidgetTree->RootWidget)
  {
    OutError = TEXT("The blueprint has no widget tree to refine.");
    return false;
  }
  if (UIWTDesignImport::HasPendingReimport(InBlueprint))
  {
    OutError = TEXT("The blueprint has a re-import waiting for Accept or Discard; finish that "
                    "first.");
    return false;
  }
  const FString Package = InBlueprint->GetOutermost()->GetName();
  UIWTDesignImport::FSidecar Sidecar;
  FDocument Doc;
  if (!LoadDesign(InBlueprint, Sidecar, Doc, OutError))
  {
    return false;
  }
  if (!Sidecar.ComponentKey.IsEmpty())
  {
    OutError = TEXT("This is a child WBP made from a component; refine the screens that use it.");
    return false;
  }
  OutPass.Source = Sidecar.Source;

  // The scope: the roots of its parts (nodes with no scoped ancestor) that
  // still have a widget, and every node inside them.
  const TSet<FString> Wanted(InScope.Nodes);
  const bool bWhole = Wanted.IsEmpty() || Wanted.Contains(Doc.Root.Id);
  TArray<FScopeRoot> Roots;
  TSet<FString> Inside;
  if (!bWhole)
  {
    CollectScope(Doc.Root, Wanted, FVector2D::ZeroVector, false, Roots);
    Roots.RemoveAll(
        [&](FScopeRoot &InRoot)
        {
          InRoot.Widget = OutermostWidget(InBlueprint, Sidecar.Owned, InRoot.Node->Id);
          return InRoot.Widget.IsEmpty();
        });
    if (Roots.IsEmpty())
    {
      OutError = TEXT("None of the scope's design nodes has a widget in the blueprint any more.");
      return false;
    }
    for (const FScopeRoot &Root : Roots)
    {
      CollectIds(*Root.Node, Inside);
      OutPass.NodeCount += CountNodes(*Root.Node);
      OutPass.ScopeWidgets.Add(Root.Widget);
    }
    OutPass.ScopeLabel = Roots.Num() == 1 ? Roots[0].Widget
                                          : FString::Printf(TEXT("%d parts"), Roots.Num());
  }
  else
  {
    OutPass.NodeCount = CountNodes(Doc.Root);
    OutPass.ScopeLabel = TEXT("the whole blueprint");
  }

  // The reference, cropped to the scope when that saves most of it. Its
  // pixels are decoded only for a crop; otherwise its size is enough.
  const FString Reference =
      FPaths::GetPath(UIWTDesignImport::GetSidecarPath(Package)) / TEXT("reference.png");
  FImage ReferencePixels;
  bool bDecoded = false;
  FString ImageError;
  FIntPoint ReferenceSize = FIntPoint::ZeroValue;
  bool bReference = FPaths::FileExists(Reference) &&
                    UIWTRunImages::ReadPngSize(Reference, ReferenceSize);
  if (!bReference && FPaths::FileExists(Reference) &&
      UIWTRunImages::LoadImageFile(Reference, ReferencePixels, ImageError))
  {
    // Not a PNG after all, but still an image.
    bReference = bDecoded = true;
    ReferenceSize = FIntPoint(ReferencePixels.SizeX, ReferencePixels.SizeY);
  }
  if (bReference)
  {
    OutPass.ReferenceImage = Reference;
    OutPass.AttachImage = Reference;
  }
  if (bReference && !bWhole && Doc.Root.Box.W > 0.0 && Doc.Root.Box.H > 0.0 &&
      (bDecoded || UIWTRunImages::LoadImageFile(Reference, ReferencePixels, ImageError)))
  {
    FBox2D Union(ForceInit);
    for (const FScopeRoot &Root : Roots)
    {
      Union += FVector2D(Root.Abs.X, Root.Abs.Y);
      Union += FVector2D(Root.Abs.X + Root.Abs.W, Root.Abs.Y + Root.Abs.H);
    }
    const double ScaleX = ReferencePixels.SizeX / Doc.Root.Box.W;
    const double ScaleY = ReferencePixels.SizeY / Doc.Root.Box.H;
    const FIntRect Rect(
        FMath::Clamp(FMath::FloorToInt((Union.Min.X - CropMargin) * ScaleX), 0,
                     ReferencePixels.SizeX),
        FMath::Clamp(FMath::FloorToInt((Union.Min.Y - CropMargin) * ScaleY), 0,
                     ReferencePixels.SizeY),
        FMath::Clamp(FMath::CeilToInt((Union.Max.X + CropMargin) * ScaleX), 0,
                     ReferencePixels.SizeX),
        FMath::Clamp(FMath::CeilToInt((Union.Max.Y + CropMargin) * ScaleY), 0,
                     ReferencePixels.SizeY));
    const int64 FullArea = int64(ReferencePixels.SizeX) * ReferencePixels.SizeY;
    FImage Cropped;
    if (Rect.Width() > 0 && Rect.Height() > 0 &&
        int64(Rect.Width()) * Rect.Height() < FullArea * MaxCropShare &&
        UIWTRunImages::CropForTexture(ReferencePixels, Rect, UIWTRunImages::ECropMode::Copy,
                                      UIWTRunImages::ECropShape::Rect, 0, FIntPoint::ZeroValue,
                                      Cropped, ImageError))
    {
      const FString CropFile = FPaths::ConvertRelativePathToFull(
          FPaths::ProjectSavedDir() / TEXT("UIWidgetTool/AIPass") /
          (InBlueprint->GetName() + TEXT("_scope.png")));
      IFileManager::Get().MakeDirectory(*FPaths::GetPath(CropFile), true);
      if (UIWTRunImages::SavePng(Cropped, CropFile, ImageError))
      {
        OutPass.AttachImage = CropFile;
        OutPass.Crop = Rect;
      }
    }
  }

  const bool bPsd = Sidecar.Source == TEXT("psd");
  const bool bImage = Sidecar.Source == TEXT("image");
  const FString RootWidget = InBlueprint->WidgetTree->RootWidget->GetName();
  FString &Request = OutPass.Request;
  Request = FString::Printf(
      TEXT("AI pass on a design import. This request replaces steps 2 to 4 of the procedure "
           "above; steps 1, 5, 6 and 7 and the rules still apply.\n\n"
           "The blueprint %s was built from a %s (\"%s\") by a design import, whose "
           "deterministic converter made every design node into widgets, with the design's "
           "layer names and no variables. Its look already matches the design. It may also "
           "have changes made in UE since (by the user or an earlier pass): keep them. Your job "
           "is its structure and names, not its look.\n\n"),
      *InBlueprint->GetPathName(),
      bPsd ? TEXT("Photoshop export") : bImage ? TEXT("image") : TEXT("Figma frame"),
      *Doc.Root.Name);
  if (bWhole)
  {
    Request += FString::Printf(
        TEXT("Scope: the whole blueprint (root widget %s), %d design nodes. "),
        *RootWidget, OutPass.NodeCount);
  }
  else
  {
    Request += FString::Printf(
        TEXT("Scope: %s of the blueprint, %d design nodes in all%s. Change only these widgets "
             "and what is inside them, and leave the rest of the blueprint as it is:\n"),
        Roots.Num() == 1 ? TEXT("one part") : *FString::Printf(TEXT("%d parts"), Roots.Num()),
        OutPass.NodeCount,
        InScope.bChangedParts ? TEXT(": the parts the last re-import of the design added or "
                                     "changed")
                              : TEXT(""));
    for (const FScopeRoot &Root : Roots)
    {
      Request += FString::Printf(
          TEXT("- %s: design node %s \"%s\", at (%g, %g), %g x %g design pixels in the root\n"),
          *Root.Widget, *Root.Node->Id, *Root.Node->Name, Root.Abs.X, Root.Abs.Y, Root.Abs.W,
          Root.Abs.H);
    }
    Request += TEXT("Use ExportWidgetSubtree and ApplyWidgetSubtree on these widgets or on "
                    "widgets inside them. When a part would need a new parent outside the scope "
                    "(a row made of two parts), don't make it: say so in your reply. ");
  }
  Request += FString::Printf(TEXT("The design root is %g x %g design pixels.\n"),
                             Doc.Root.Box.W, Doc.Root.Box.H);
  if (bReference && OutPass.Crop.Area() > 0)
  {
    Request += FString::Printf(
        TEXT("Reference: the attached image is the design as drawn, cropped to the scope: "
             "pixels (%d, %d) to (%d, %d) of %s (%d x %d). RenderWidgetBlueprint renders the "
             "whole blueprint and compares it with that whole reference, and ZoomImage takes "
             "its pixel coordinates: zoom into the scope's region to compare it.\n"),
        OutPass.Crop.Min.X, OutPass.Crop.Min.Y, OutPass.Crop.Max.X, OutPass.Crop.Max.Y,
        *Reference, ReferenceSize.X, ReferenceSize.Y);
  }
  else if (bReference)
  {
    Request += FString::Printf(
        TEXT("Reference: the attached image is the design as drawn, %d x %d pixels, saved as "
             "%s. RenderWidgetBlueprint compares against it, and ZoomImage takes its pixel "
             "coordinates.\n"),
        ReferenceSize.X, ReferenceSize.Y, *Reference);
  }
  const int32 Rounds = FMath::Max(InRounds, 1);
  Request += FString::Printf(
      TEXT("Rounds: at most %d rounds of apply, RenderWidgetBlueprint and compare. Stop "
           "earlier when the structure is done and the render still matches.\n\n"
           "Do%s:\n%s\n"),
      Rounds, bWhole ? TEXT("") : TEXT(", inside the scope only"),
      *PassTasks(Sidecar.Source));
  Request += TEXT(
      "How:\n"
      "- Read the outline below first. UIWTDesignToolset.GetDesignNode(WidgetBlueprint, "
      "NodeId, Depth) returns a node's full design data (text runs, fills, strokes, effects, "
      "hints) when you need it. The outline's \"w\" is the widget the node became.\n"
      "- Work one part at a time: UIWTToolset.ExportWidgetSubtree and ApplyWidgetSubtree on "
      "the part you restructure. Use ExportWidgetSpec / ApplyWidgetSpec on the whole tree "
      "only when it is small and the scope is the whole blueprint.\n"
      "- Keep every widget you keep with the same name and class, also when you wrap it in a "
      "Button or move it into a box: re-imports of the design update widgets by name, and "
      "keep the widgets you add and the classes and parents you change.\n"
      "- Rename only with UIWTToolset.RenameWidgets. Never rename by changing a name in a "
      "spec: that deletes the widget and makes a new one, and a re-import loses track of "
      "it.\n"
      "- Don't change texts, colours, fonts, images or textures, and don't delete widgets "
      "that show design content: the import took those from the design, and re-imports "
      "update them.\n"
      "- After each apply, RenderWidgetBlueprint (0 x 0) and check the side-by-side image; "
      "fix anything that moved.\n"
      "- Save with SaveWidgetBlueprint after a clean compile. Reply in fewer than three "
      "sentences: what you changed (buttons, lists, boxes, anchors, renames, variables) and "
      "what still differs from the reference, ending with the Widget Blueprint's path.\n\n");
  Request += TEXT("Design outline (id, n name, k kind, b box [x, y, w, h] relative to the "
                  "parent in design pixels, w widget, v false when hidden, h hints, c "
                  "children)");
  if (bWhole)
  {
    Request += TEXT(":\n") + MakeOutline(Doc.Root, Sidecar.Owned) + TEXT("\n");
  }
  else
  {
    Request += TEXT(", one line per part:\n");
    for (const FScopeRoot &Root : Roots)
    {
      Request += MakeOutline(*Root.Node, Sidecar.Owned) + TEXT("\n");
    }
  }

  const TArray<FReportEntry> &Report = InReport.IsEmpty() ? Sidecar.Report : InReport;
  TArray<const FReportEntry *> Entries;
  for (const FReportEntry &Entry : Report)
  {
    if (bWhole || Inside.Contains(Entry.Node))
    {
      Entries.Add(&Entry);
    }
  }
  if (!Entries.IsEmpty())
  {
    Request += FString::Printf(TEXT("\nImport report for the scope (%d entries%s): what the "
                                    "converter approximated or dropped.\n"),
                               Entries.Num(),
                               Entries.Num() > MaxReportEntries ? TEXT(", the first shown")
                                                                : TEXT(""));
    for (int32 Index = 0; Index < FMath::Min(Entries.Num(), MaxReportEntries); ++Index)
    {
      Request += FString::Printf(TEXT("- [%s] %s: %s\n"), *Entries[Index]->Category,
                                 *Entries[Index]->Node, *Entries[Index]->Detail);
    }
  }
  return true;
}

TMap<FGuid, FName> UIWTDesignRefine::SnapshotWidgets(const UWidgetBlueprint *InBlueprint)
{
  TMap<FGuid, FName> Widgets;
  if (!InBlueprint || !InBlueprint->WidgetTree)
  {
    return Widgets;
  }
  for (const TPair<FName, FGuid> &Pair : InBlueprint->WidgetVariableNameToGuidMap)
  {
    // Animations share the map; only widgets matter here.
    if (InBlueprint->WidgetTree->FindWidget(Pair.Key))
    {
      Widgets.Add(Pair.Value, Pair.Key);
    }
  }
  return Widgets;
}

bool UIWTDesignRefine::SyncRenames(UWidgetBlueprint *InBlueprint,
                                   const TMap<FGuid, FName> &InBefore, int32 &OutRenamed,
                                   FString &OutError)
{
  OutRenamed = 0;
  const TMap<FGuid, FName> After = SnapshotWidgets(InBlueprint);
  TMap<FString, FString> Renames;
  TSet<FString> Removed;
  for (const TPair<FGuid, FName> &Before : InBefore)
  {
    const FName *Now = After.Find(Before.Key);
    if (!Now)
    {
      Removed.Add(Before.Value.ToString());
    }
    else if (!Now->IsEqual(Before.Value, ENameCase::CaseSensitive))
    {
      Renames.Add(Before.Value.ToString(), Now->ToString());
    }
  }
  if (Renames.IsEmpty())
  {
    return true;
  }
  if (!UIWTDesignImport::RenameInSidecar(InBlueprint->GetOutermost()->GetName(), Renames,
                                         Removed, OutError))
  {
    return false;
  }
  OutRenamed = Renames.Num();
  return true;
}

FString UIWTDesignRefine::PassTasks(const FString &InSource)
{
  return InSource == TEXT("figma") ? FigmaTasks : PsdTasks;
}
