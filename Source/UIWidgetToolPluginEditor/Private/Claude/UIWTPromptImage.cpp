#include "UIWTPromptImage.h"

#include "Brushes/SlateDynamicImageBrush.h"
#include "HAL/FileManager.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <shellapi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#define LOCTEXT_NAMESPACE "UIWidgetTool"

namespace
{
  // Claude downscales anything with a longer edge than this before looking
  // at it, so sending more only costs upload time.
  constexpr int32 MaxEdgePixels = 1568;
  // The API's per-image limit, applied to the base64 text to be safe.
  constexpr int64 MaxBase64Bytes = 5 * 1024 * 1024;
  // Refuse to even read files bigger than this.
  constexpr int64 MaxFileBytes = 64 * 1024 * 1024;
  constexpr int32 ThumbnailEdgePixels = 128;
  constexpr int32 JpegQuality = 85;

  int32 GThumbnailSerial = 0;

  bool FitsApiLimit(const TArray64<uint8> &InEncoded)
  {
    return InEncoded.Num() <= MaxBase64Bytes &&
           static_cast<int64>(FBase64::GetEncodedDataSize(
               static_cast<uint32>(InEncoded.Num()))) <= MaxBase64Bytes;
  }

  // Resizes InImage so its longer edge is at most InMaxEdge; smaller images
  // are copied as they are. Output is always BGRA8 sRGB.
  void FitWithin(const FImage &InImage, int32 InMaxEdge, FImage &OutImage)
  {
    const int32 LongEdge = FMath::Max(InImage.SizeX, InImage.SizeY);
    if (LongEdge <= InMaxEdge)
    {
      InImage.CopyTo(OutImage, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
      return;
    }
    const double Scale = static_cast<double>(InMaxEdge) / LongEdge;
    const int32 Width =
        FMath::Max(1, FMath::RoundToInt32(InImage.SizeX * Scale));
    const int32 Height =
        FMath::Max(1, FMath::RoundToInt32(InImage.SizeY * Scale));
    FImageCore::ResizeImageAllocDest(InImage, OutImage, Width, Height,
                                     ERawImageFormat::BGRA8, EGammaSpace::sRGB);
  }

  TSharedPtr<FSlateDynamicImageBrush> MakeThumbnail(const FImage &InImage)
  {
    FImage Thumb;
    FitWithin(InImage, ThumbnailEdgePixels, Thumb);
    // BGRA8 pixels, which is what FSlateDynamicImageBrush takes.
    const TArray<uint8> Pixels(Thumb.RawData.GetData(),
                               static_cast<int32>(Thumb.RawData.Num()));
    const FName BrushName(
        *FString::Printf(TEXT("UIWTPromptImage_%d"), GThumbnailSerial++));
    return FSlateDynamicImageBrush::CreateWithImageData(
        BrushName,
        FVector2D(static_cast<float>(Thumb.SizeX),
                  static_cast<float>(Thumb.SizeY)),
        Pixels);
  }
}

FString UIWTPromptImage::GetFileTypes()
{
  return TEXT("Image files (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp");
}

TSharedPtr<const FUIWTPromptImage>
UIWTPromptImage::LoadFromFile(const FString &InPath, FText &OutError)
{
  const FText FileName = FText::FromString(FPaths::GetCleanFilename(InPath));

  const int64 FileSize = IFileManager::Get().FileSize(*InPath);
  if (FileSize < 0)
  {
    OutError = FText::Format(
        LOCTEXT("PromptImageMissing", "Could not read {0}."), FileName);
    return nullptr;
  }
  if (FileSize > MaxFileBytes)
  {
    OutError = FText::Format(
        LOCTEXT("PromptImageFileTooLarge", "{0} is too large to attach."),
        FileName);
    return nullptr;
  }

  TArray<uint8> FileData;
  if (!FFileHelper::LoadFileToArray(FileData, *InPath))
  {
    OutError = FText::Format(
        LOCTEXT("PromptImageReadFailed", "Could not read {0}."), FileName);
    return nullptr;
  }
  return LoadFromMemory(FileData, FileName.ToString(), OutError);
}

TSharedPtr<const FUIWTPromptImage>
UIWTPromptImage::LoadFromMemory(TConstArrayView<uint8> InData,
                                const FString &InDisplayName, FText &OutError)
{
  const FText FileName = FText::FromString(InDisplayName);

  IImageWrapperModule &ImageWrapper =
      FModuleManager::LoadModuleChecked<IImageWrapperModule>(
          TEXT("ImageWrapper"));
  const EImageFormat SourceFormat =
      ImageWrapper.DetectImageFormat(InData.GetData(), InData.Num());
  if (SourceFormat != EImageFormat::PNG && SourceFormat != EImageFormat::JPEG &&
      SourceFormat != EImageFormat::GrayscaleJPEG &&
      SourceFormat != EImageFormat::BMP)
  {
    OutError = FText::Format(
        LOCTEXT("PromptImageBadFormat",
                "{0} is not a PNG, JPEG or BMP image."),
        FileName);
    return nullptr;
  }

  FImage Decoded;
  if (!ImageWrapper.DecompressImage(InData.GetData(), InData.Num(), Decoded) ||
      Decoded.SizeX <= 0 || Decoded.SizeY <= 0)
  {
    OutError = FText::Format(
        LOCTEXT("PromptImageDecodeFailed", "{0} could not be decoded."),
        FileName);
    return nullptr;
  }

  FImage Fitted;
  FitWithin(Decoded, MaxEdgePixels, Fitted);

  // PNG keeps UI text crisp; photos that do not fit as PNG go as JPEG.
  TArray64<uint8> Encoded;
  FString MediaType = TEXT("image/png");
  if (!ImageWrapper.CompressImage(Encoded, EImageFormat::PNG, Fitted) ||
      !FitsApiLimit(Encoded))
  {
    Encoded.Reset();
    MediaType = TEXT("image/jpeg");
    if (!ImageWrapper.CompressImage(Encoded, EImageFormat::JPEG, Fitted,
                                    JpegQuality))
    {
      OutError = FText::Format(
          LOCTEXT("PromptImageEncodeFailed", "{0} could not be re-encoded."),
          FileName);
      return nullptr;
    }
  }
  if (!FitsApiLimit(Encoded))
  {
    OutError = FText::Format(
        LOCTEXT("PromptImageTooLarge",
                "{0} is still larger than 5 MB after shrinking it."),
        FileName);
    return nullptr;
  }

  TSharedRef<FUIWTPromptImage> Image = MakeShared<FUIWTPromptImage>();
  Image->FileName = FileName.ToString();
  Image->MediaType = MediaType;
  Image->Base64Data = FBase64::Encode(Encoded.GetData(),
                                      static_cast<uint32>(Encoded.Num()));
  Image->Thumbnail = MakeThumbnail(Fitted);
  return Image;
}

TSharedPtr<const FUIWTPromptImage>
UIWTPromptImage::LoadFromDib(TConstArrayView<uint8> InDib,
                             const FString &InDisplayName, FText &OutError)
{
  // A DIB is a BMP file without its 14-byte file header. Put one back, with
  // the pixel offset the header, masks and palette imply, and let the BMP
  // decoder handle the many header versions and pixel layouts.
  constexpr int32 FileHeaderBytes = 14;
  constexpr int32 InfoHeaderBytes = 40;
  constexpr uint32 BiBitfields = 3;
  constexpr uint32 BiAlphaBitfields = 6;
  auto ReadU32 = [&InDib](int32 InOffset)
  {
    uint32 Value = 0;
    FMemory::Memcpy(&Value, InDib.GetData() + InOffset, sizeof(Value));
    return Value;
  };

  const FText Name = FText::FromString(InDisplayName);
  if (InDib.Num() < InfoHeaderBytes)
  {
    OutError = FText::Format(
        LOCTEXT("PromptImageDibTooSmall", "{0} could not be decoded."), Name);
    return nullptr;
  }
  const uint32 HeaderSize = ReadU32(0);
  const uint32 BitCount = ReadU32(14) & 0xFFFF;
  const uint32 Compression = ReadU32(16);
  const uint32 ColorsUsed = ReadU32(32);

  uint32 MaskBytes = 0;
  if (HeaderSize == InfoHeaderBytes)
  {
    // Larger headers carry the masks inside them.
    MaskBytes = Compression == BiBitfields        ? 12
                : Compression == BiAlphaBitfields ? 16
                                                  : 0;
  }
  const uint64 PaletteEntries =
      ColorsUsed != 0 ? ColorsUsed : (BitCount <= 8 ? 1ull << BitCount : 0);
  const uint64 PixelOffset =
      FileHeaderBytes + HeaderSize + MaskBytes + PaletteEntries * 4;
  if (HeaderSize < InfoHeaderBytes ||
      PixelOffset > static_cast<uint64>(FileHeaderBytes + InDib.Num()))
  {
    OutError = FText::Format(
        LOCTEXT("PromptImageDibBad", "{0} could not be decoded."), Name);
    return nullptr;
  }

  TArray<uint8> Bmp;
  Bmp.Reserve(FileHeaderBytes + InDib.Num());
  auto AppendU32 = [&Bmp](uint32 InValue)
  { Bmp.Append(reinterpret_cast<const uint8 *>(&InValue), sizeof(InValue)); };
  Bmp.Add('B');
  Bmp.Add('M');
  AppendU32(static_cast<uint32>(FileHeaderBytes + InDib.Num()));
  AppendU32(0); // reserved
  AppendU32(static_cast<uint32>(PixelOffset));
  Bmp.Append(InDib.GetData(), InDib.Num());
  return LoadFromMemory(Bmp, InDisplayName, OutError);
}

bool UIWTPromptImage::PasteFromClipboard(
    TSharedPtr<const FUIWTPromptImage> &OutImage, FText &OutError)
{
  OutImage.Reset();
#if PLATFORM_WINDOWS
  // Text wins: Excel and some browsers put a bitmap next to the text the
  // user meant to paste.
  if (::IsClipboardFormatAvailable(CF_UNICODETEXT))
  {
    return false;
  }

  // Encoded PNG keeps alpha; CF_DIB is what screenshots and "Copy image"
  // always offer (Windows synthesizes it from the other bitmap formats); a
  // copied file in Explorer arrives as CF_HDROP.
  const UINT PngFormat = ::RegisterClipboardFormatW(L"PNG");
  UINT Format = 0;
  if (PngFormat != 0 && ::IsClipboardFormatAvailable(PngFormat))
  {
    Format = PngFormat;
  }
  else if (::IsClipboardFormatAvailable(CF_DIB))
  {
    Format = CF_DIB;
  }
  else if (::IsClipboardFormatAvailable(CF_HDROP))
  {
    Format = CF_HDROP;
  }
  else
  {
    return false;
  }

  if (!::OpenClipboard(nullptr))
  {
    OutError = LOCTEXT("PromptImageClipboardBusy",
                       "The clipboard is in use by another application. "
                       "Try pasting again.");
    return true;
  }
  // Copy out and close right away; decoding happens afterwards.
  TArray<uint8> Data;
  FString FilePath;
  if (HANDLE Handle = ::GetClipboardData(Format))
  {
    if (Format == CF_HDROP)
    {
      const HDROP Drop = static_cast<HDROP>(Handle);
      const UINT Count = ::DragQueryFileW(Drop, 0xFFFFFFFF, nullptr, 0);
      for (UINT Index = 0; Index < Count && FilePath.IsEmpty(); ++Index)
      {
        TArray<TCHAR> Buffer;
        Buffer.SetNumZeroed(::DragQueryFileW(Drop, Index, nullptr, 0) + 1);
        ::DragQueryFileW(Drop, Index, Buffer.GetData(),
                         static_cast<UINT>(Buffer.Num()));
        const FString Path(Buffer.GetData());
        const FString Extension = FPaths::GetExtension(Path).ToLower();
        if (Extension == TEXT("png") || Extension == TEXT("jpg") ||
            Extension == TEXT("jpeg") || Extension == TEXT("bmp"))
        {
          FilePath = Path;
        }
      }
    }
    else if (const void *Locked = ::GlobalLock(Handle))
    {
      const SIZE_T Size = ::GlobalSize(Handle);
      if (Size > 0 && Size <= static_cast<SIZE_T>(MaxFileBytes))
      {
        Data.Append(static_cast<const uint8 *>(Locked),
                    static_cast<int32>(Size));
      }
      ::GlobalUnlock(Handle);
    }
  }
  ::CloseClipboard();

  if (Format == CF_HDROP)
  {
    if (FilePath.IsEmpty())
    {
      // Copied files, none of them an image: nothing to paste here.
      return false;
    }
    OutImage = LoadFromFile(FilePath, OutError);
    return true;
  }
  if (Data.Num() == 0)
  {
    OutError = LOCTEXT("PromptImageClipboardRead",
                       "The image on the clipboard could not be read.");
    return true;
  }
  const FString PastedName = TEXT("Pasted image");
  OutImage = Format == CF_DIB ? LoadFromDib(Data, PastedName, OutError)
                              : LoadFromMemory(Data, PastedName, OutError);
  return true;
#else
  return false;
#endif
}

#undef LOCTEXT_NAMESPACE
