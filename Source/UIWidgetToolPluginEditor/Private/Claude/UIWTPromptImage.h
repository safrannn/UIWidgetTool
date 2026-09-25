#pragma once

#include "CoreMinimal.h"

struct FSlateDynamicImageBrush;

// An image attached to one prompt: the bytes Claude receives, re-encoded
// and base64'd, plus a small thumbnail for the chat panel. Created and
// released on the game thread only, since the thumbnail is a Slate resource.
struct FUIWTPromptImage
{
  // Just the name, for display; the source file is not kept.
  FString FileName;
  // "image/png" or "image/jpeg".
  FString MediaType;
  FString Base64Data;
  // Pixel size of the image as sent, and of the file before shrinking.
  FIntPoint Size = FIntPoint::ZeroValue;
  FIntPoint SourceSize = FIntPoint::ZeroValue;
  // The image before shrinking, as PNG. A run saves it to its folder so the
  // image tools can crop and zoom at full resolution.
  TArray64<uint8> SourcePng;
  TSharedPtr<FSlateDynamicImageBrush> Thumbnail;
};

namespace UIWTPromptImage
{
  // Filter string for IDesktopPlatform::OpenFileDialog.
  FString GetFileTypes();

  // Decodes a PNG, JPEG or BMP file, shrinks it to the size Claude would
  // downscale it to anyway and re-encodes it within the API's per-image
  // limit. Null with OutError set when the file is unreadable, not one of
  // those formats, or still too large.
  TSharedPtr<const FUIWTPromptImage> LoadFromFile(const FString &InPath,
                                                  FText &OutError);

  // Same as LoadFromFile for an encoded image already in memory. The display
  // name stands in for the file name in the chat and in errors.
  TSharedPtr<const FUIWTPromptImage> LoadFromMemory(TConstArrayView<uint8> InData,
                                                    const FString &InDisplayName,
                                                    FText &OutError);

  // A Windows device-independent bitmap (the clipboard's CF_DIB: a BMP file
  // without its file header).
  TSharedPtr<const FUIWTPromptImage> LoadFromDib(TConstArrayView<uint8> InDib,
                                                 const FString &InDisplayName,
                                                 FText &OutError);

  // Takes an image off the system clipboard: a copied bitmap (a screenshot,
  // "Copy image") or a copied PNG, JPEG or BMP file. False when there is no
  // image to paste, including when the clipboard also holds text, so text
  // paste keeps working. True when there was one; OutImage is then null
  // with OutError set if it could not be loaded. Windows only.
  bool PasteFromClipboard(TSharedPtr<const FUIWTPromptImage> &OutImage,
                          FText &OutError);
}
