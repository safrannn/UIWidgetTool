#pragma once

#include "Core/UIWTDesignTree.h"
#include "CoreMinimal.h"

class FJsonObject;

// Raw Figma REST JSON → the shared design tree (import-figma.md step 2).
// Pure: it reads the /v1/files/{key}/nodes response, decides what to
// rasterize and which image fills to download, and fills in image sizes once
// the client has the pixels. No HTTP here.
namespace UIWTFigmaNormalize
{
  struct FOptions
  {
    FString FileKey;
    // The frame's node id, e.g. 12:34
    FString NodeId;
    FString FileName;
    FString Version;
    FString LastModified;
    // The design tree's referenceSize (UUIWTDesignSettings).
    FIntPoint ReferenceSize = FIntPoint(1920, 1080);
    // /v1/images scale for rasterized nodes.
    double RenderScale = 2.0;
    // Image fills with more texture pixels per design pixel than this are
    // downscaled when written.
    double MaxImageScale = 2.0;
    // Set when normalizing a main component from another file (a library):
    // its renders go under render/<file key>/, since node ids only mean
    // something within one file.
    FString RenderFileKey;
  };

  // One image in the tree that still needs its pixels: a node the reader
  // rasterizes (/v1/images) or an image fill (/v1/files/{key}/images).
  struct FImageJob
  {
    // The file to render or fetch the image from.
    FString FileKey;
    // Rendered node: its id. Image fill: empty.
    FString RenderId;
    // Rendered with use_absolute_bounds (the layer's box, for nodes an auto
    // layout places) instead of the render bounds.
    bool bAbsoluteBounds = false;
    // Image fill: Figma's imageRef.
    FString ImageRef;
    // FILL, FIT, TILE or STRETCH (Figma's CROP).
    FString ScaleMode;
    // STRETCH: imageTransform as [a, b, tx, c, d, ty], node space (0-1) to
    // image space (0-1).
    TArray<double> Transform;
    // The node's box size in design pixels.
    FVector2D BoxSize = FVector2D::ZeroVector;
    FString NodeId;
    // The tree's image block, completed by FinishImages. Shared with the
    // node, so completing it completes the tree.
    TSharedPtr<UIWTDesignTree::FImageRef> Image;
  };

  struct FResult
  {
    UIWTDesignTree::FDocument Document;
    TArray<FImageJob> Images;
    int32 NodeCount = 0;
    // component.key → the instances' componentId in this file: the main
    // component's node when the key turns out to be local (unpublished).
    TMap<FString, FString> ComponentNodeIds;
  };

  // Normalizes the /nodes response for InOptions.NodeId. Fails when the
  // response has no such node, or the node is a page or the document.
  bool Normalize(const TSharedRef<FJsonObject> &InNodesResponse, const FOptions &InOptions,
                 FResult &OutResult, FString &OutError);

  // Normalizes a main component from a /nodes response of the file that
  // holds it (InOptions.FileKey, NodeId): its root is at (0, 0). Its image
  // jobs, notes and instance component ids go into InOutResult.
  bool NormalizeComponent(const TSharedRef<FJsonObject> &InNodesResponse, const FOptions &InOptions,
                          FResult &InOutResult, UIWTDesignTree::FNode &OutRoot, FString &OutError);

  // The component.keys of the instances under InRoot that become child
  // WBPs (no overrides), in tree order, without descending into them: their
  // own contents come from their main components.
  void CollectComponentKeys(const UIWTDesignTree::FNode &InRoot, TArray<FString> &OutKeys,
                            TMap<FString, FString> *OutNames = nullptr);

  // A main component root built from an instance of InKey under InRoot (the
  // first without overrides that has layers), for a main component Figma
  // only returns as an empty stub: a library component the file uses but the
  // token can't open. The instance's own placement (position, rotation,
  // opacity, visibility, constraints) stays on the instance. False when there
  // is no such instance.
  bool ComponentFromInstance(const UIWTDesignTree::FNode &InRoot, const FString &InKey,
                             UIWTDesignTree::FNode &OutRoot);

  // The components entry's hash: the node as design.json writes it, plus the
  // pixels of every image under it (files relative to InDirectory), so a
  // re-rendered vector counts as a change too.
  FString HashComponent(const UIWTDesignTree::FNode &InRoot, const FString &InDirectory);

  // Where the client saves downloads, relative to design.json: rendered
  // nodes, image fills as downloaded, and the frame rendered at RenderScale.
  // InFileKey is set for nodes of another file (see FOptions::RenderFileKey).
  FString RenderFile(const FString &InNodeId, const FString &InFileKey = FString());
  FString SourceImageFile(const FString &InImageRef);
  FString ReferenceRenderFile();

  // Writes every job's texture PNG (cropping FILL and STRETCH images to the
  // box, downscaling oversized fills) from the downloads under InDirectory,
  // and sets each image's size and scale. Jobs whose download is missing or
  // can't be decoded get a note. Renders are only measured, from their PNG
  // header.
  void FinishImages(FResult &InOutResult, const FString &InDirectory, const FOptions &InOptions);

  // Writes reference.png at 1x from the frame's RenderScale render
  // (ReferenceRenderFile), when there is one.
  void WriteReference(const FString &InDirectory, const FOptions &InOptions);

  // "12:34;5:6" → "12-34_5-6": node ids as file names.
  FString FileSafeId(const FString &InNodeId);
}
