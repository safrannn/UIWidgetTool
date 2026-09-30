#pragma once

#include "Core/UIWTDesignTree.h"
#include "CoreMinimal.h"

struct FUIWTActiveRun;

// Image → Widget Blueprint (import-image.md): Claude is the reader. In an
// image-reading run it cuts the art brushes can't draw (CutImageNode) and
// writes the design tree in the image's pixels (WriteDesignTree); each write
// is converted into design.json and imported, the first time, or merged into
// the blueprint, after that. Everything after design.json is the shared
// pipeline.
namespace UIWTImageReader
{
  enum class EScale : uint8
  {
    // One image pixel is one design pixel.
    One,
    // A 2x screenshot or mockup.
    Two,
    // The image's width is the reference resolution's.
    Fit
  };

  struct FSource
  {
    // The image as picked.
    FString SourceFile;
    // Saved/UIWidgetTool/Image/<name>_<crc>: reference.png, images/,
    // design.json.
    FString CacheDir;
    // CRC-32 of the file, 8 hex digits.
    FString Crc;
    FIntPoint ImageSize = FIntPoint::ZeroValue;
    // Image pixels per design pixel.
    double Scale = 1.0;
    FVector2D DesignSize = FVector2D::ZeroVector;
  };

  // Copies InFile (PNG, JPEG or BMP) into a fresh cache folder as
  // reference.png, with an empty images/.
  bool PrepareSource(const FString &InFile, EScale InScale, FSource &OutSource,
                     FString &OutError);

  // The reading run's request (after the general run instructions).
  FString BuildRequest(const FSource &InSource, int32 InRounds, bool bInAIPass);

  // The tree Claude wrote, boxes and lengths in absolute image pixels, as a
  // design document: boxes made relative to the parent and every length,
  // font size and line height divided by InScale; image nodes get their
  // texture's size and scale. OutErrors lists every problem.
  bool ToDocument(const FString &InJson, const FString &InCacheDir, double InScale,
                  const FString &InSourceFile, const FString &InCrc, FIntPoint InReferenceSize,
                  UIWTDesignTree::FDocument &OutDocument, TArray<FString> &OutErrors);

  // CutImageNode's work: the rectangle of the run's reference, keyed and
  // shaped, as images/<id>.png. A crop with the same pixels as an earlier
  // one returns that file instead. OutPath is relative to the cache folder.
  bool CutNode(FUIWTActiveRun &InOutRun, const FString &InId, const FIntRect &InRect,
               const FString &InMode, const FString &InShape, FString &OutPath,
               FString &OutError);

  // WriteDesignTree's work: design.json, then the import (first call: the
  // blueprint, its textures, the entry's widget) or the re-import merge
  // (later calls, renames made since synced into the sidecar first).
  // OutSummary is the reply for Claude.
  bool WriteTree(FUIWTActiveRun &InOutRun, const FString &InJson, FString &OutSummary,
                 TArray<FString> &OutErrors);
}
