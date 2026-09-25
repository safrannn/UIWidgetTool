#pragma once

#include "CoreMinimal.h"
#include "ImageCore.h"

class UObject;
class UTexture2D;
class UWidgetBlueprint;

// Image work for a run: zooming into the reference image, cutting textures
// out of it, and rendering a Widget Blueprint to compare against it. Every
// FImage here is BGRA8 sRGB, and every rectangle is in the image's pixels.
namespace UIWTRunImages
{

  // Longest edge of an image Claude looks at before it is downscaled.
  constexpr int32 MaxViewEdge = 1568;

  enum class ECropMode : uint8
  {
    // The pixels as they are.
    Copy,
    // Alpha from brightness above the background (nearly all of the crop's
    // border pixels) plus the threshold, colours kept: art on a dark
    // background.
    KeyDark,
    // Like KeyDark, but white, so a widget tint gives it its colour.
    Mask
  };

  enum class ECropShape : uint8
  {
    Rect,
    // An ellipse filling the output, with an anti-aliased edge.
    Circle
  };

  bool ParseCropMode(const FString &InText, ECropMode &OutMode);
  bool ParseCropShape(const FString &InText, ECropShape &OutShape);

  bool LoadImageFile(const FString &InPath, FImage &OutImage,
                     FString &OutError);
  bool SavePng(const FImage &InImage, const FString &InPath,
               FString &OutError);

  // Fails when InRect is empty or not inside InImage.
  bool CheckRect(const FImage &InImage, const FIntRect &InRect,
                 const TCHAR *InWhat, FString &OutError);

  // InRect magnified by up to InScale (nearest neighbour), lowered so the
  // longer edge stays within MaxViewEdge. OutScale is the factor used.
  bool Zoom(const FImage &InImage, const FIntRect &InRect, int32 InScale,
            FImage &OutImage, double &OutScale, FString &OutError);

  // Left and right next to each other on a dark gap; right is resized to
  // left's size when they differ.
  void SideBySide(const FImage &InLeft, const FImage &InRight,
                  FImage &OutImage);

  // Mean absolute RGB difference in percent, with B resized to A's size.
  double MeanDifference(const FImage &InA, const FImage &InB);

  // Cuts InRect out of InSource for a texture: keyed per InMode, resized to
  // InOutputSize (the crop's size when zero) and shaped per InShape.
  bool CropForTexture(const FImage &InSource, const FIntRect &InRect,
                      ECropMode InMode, ECropShape InShape, int32 InThreshold,
                      FIntPoint InOutputSize, FImage &OutImage,
                      FString &OutError);

  // Creates InFolder/InName, or replaces the pixels of the texture already
  // there, as a UI texture: uncompressed, sRGB, UI group, no mips, never
  // streamed. Not saved. OutCreated says whether the asset is new.
  UTexture2D *WriteTexture(const FString &InFolder, const FString &InName,
                           const FImage &InPixels, bool &OutCreated,
                           FString &OutError);

  // Saves an asset's package where it lives.
  bool SaveAsset(UObject *InAsset, FString &OutError);

  // Renders the blueprint at InSize in design mode (construct scripts that
  // need a game do not run). Compiles first when needed; fails on compile
  // errors. Needs a real RHI.
  bool RenderWidget(UWidgetBlueprint *InBlueprint, FIntPoint InSize,
                    FImage &OutImage, FString &OutError);

}
