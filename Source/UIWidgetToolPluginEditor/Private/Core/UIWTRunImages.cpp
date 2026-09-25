#include "UIWTRunImages.h"

#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Editor.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "IImageWrapperModule.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "Slate/WidgetRenderer.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"

namespace UIWTRunImages
{

  namespace
  {
    constexpr int32 MaxTextureEdge = 2048;
    constexpr int32 MaxRenderEdge = 4096;
    constexpr int32 SideBySideGap = 8;
    const FColor GapColor(32, 32, 32, 255);

    IImageWrapperModule &GetImageWrapper()
    {
      return FModuleManager::LoadModuleChecked<IImageWrapperModule>(
          TEXT("ImageWrapper"));
    }

    void InitImage(FImage &OutImage, int32 InWidth, int32 InHeight)
    {
      OutImage.Init(InWidth, InHeight, ERawImageFormat::BGRA8,
                    EGammaSpace::sRGB);
    }

    void CopyRect(const FImage &InImage, const FIntRect &InRect,
                  FImage &OutImage)
    {
      InitImage(OutImage, InRect.Width(), InRect.Height());
      const TArrayView64<const FColor> Source = InImage.AsBGRA8();
      const TArrayView64<FColor> Dest = OutImage.AsBGRA8();
      for (int32 Y = 0; Y < InRect.Height(); ++Y)
      {
        FMemory::Memcpy(
            &Dest[static_cast<int64>(Y) * InRect.Width()],
            &Source[static_cast<int64>(InRect.Min.Y + Y) * InImage.SizeX +
                    InRect.Min.X],
            InRect.Width() * sizeof(FColor));
      }
    }

    void Resize(const FImage &InImage, int32 InWidth, int32 InHeight,
                FImage &OutImage)
    {
      if (InImage.SizeX == InWidth && InImage.SizeY == InHeight)
      {
        InImage.CopyTo(OutImage, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
        return;
      }
      FImageCore::ResizeImageAllocDest(InImage, OutImage, InWidth, InHeight,
                                       ERawImageFormat::BGRA8,
                                       EGammaSpace::sRGB);
    }

    uint8 Brightness(const FColor &InColor)
    {
      return FMath::Max3(InColor.R, InColor.G, InColor.B);
    }
  }

  bool ParseCropMode(const FString &InText, ECropMode &OutMode)
  {
    if (InText.IsEmpty() || InText.Equals(TEXT("Copy"), ESearchCase::IgnoreCase))
    {
      OutMode = ECropMode::Copy;
    }
    else if (InText.Equals(TEXT("KeyDark"), ESearchCase::IgnoreCase))
    {
      OutMode = ECropMode::KeyDark;
    }
    else if (InText.Equals(TEXT("Mask"), ESearchCase::IgnoreCase))
    {
      OutMode = ECropMode::Mask;
    }
    else
    {
      return false;
    }
    return true;
  }

  bool ParseCropShape(const FString &InText, ECropShape &OutShape)
  {
    if (InText.IsEmpty() || InText.Equals(TEXT("Rect"), ESearchCase::IgnoreCase))
    {
      OutShape = ECropShape::Rect;
    }
    else if (InText.Equals(TEXT("Circle"), ESearchCase::IgnoreCase))
    {
      OutShape = ECropShape::Circle;
    }
    else
    {
      return false;
    }
    return true;
  }

  bool LoadImageFile(const FString &InPath, FImage &OutImage,
                     FString &OutError)
  {
    TArray<uint8> Data;
    if (!FFileHelper::LoadFileToArray(Data, *InPath))
    {
      OutError = FString::Printf(TEXT("Could not read %s."), *InPath);
      return false;
    }
    FImage Decoded;
    if (!GetImageWrapper().DecompressImage(Data.GetData(), Data.Num(),
                                           Decoded) ||
        Decoded.SizeX <= 0 || Decoded.SizeY <= 0)
    {
      OutError = FString::Printf(
          TEXT("%s is not a PNG, JPEG or BMP image that can be decoded."),
          *InPath);
      return false;
    }
    Decoded.CopyTo(OutImage, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
    return true;
  }

  bool SavePng(const FImage &InImage, const FString &InPath,
               FString &OutError)
  {
    TArray64<uint8> Encoded;
    if (!GetImageWrapper().CompressImage(Encoded, EImageFormat::PNG, InImage))
    {
      OutError = TEXT("PNG encoding failed.");
      return false;
    }
    if (!FFileHelper::SaveArrayToFile(Encoded, *InPath))
    {
      OutError = FString::Printf(TEXT("Could not write %s."), *InPath);
      return false;
    }
    return true;
  }

  bool CheckRect(const FImage &InImage, const FIntRect &InRect,
                 const TCHAR *InWhat, FString &OutError)
  {
    if (InRect.Width() <= 0 || InRect.Height() <= 0 || InRect.Min.X < 0 ||
        InRect.Min.Y < 0 || InRect.Max.X > InImage.SizeX ||
        InRect.Max.Y > InImage.SizeY)
    {
      OutError = FString::Printf(
          TEXT("The rectangle x %d, y %d, width %d, height %d is not inside "
               "the %s (%d x %d)."),
          InRect.Min.X, InRect.Min.Y, InRect.Width(), InRect.Height(), InWhat,
          InImage.SizeX, InImage.SizeY);
      return false;
    }
    return true;
  }

  bool Zoom(const FImage &InImage, const FIntRect &InRect, int32 InScale,
            FImage &OutImage, double &OutScale, FString &OutError)
  {
    if (!CheckRect(InImage, InRect, TEXT("image"), OutError))
    {
      return false;
    }
    FImage Crop;
    CopyRect(InImage, InRect, Crop);

    const int32 LongEdge = FMath::Max(InRect.Width(), InRect.Height());
    const int32 Scale = FMath::Clamp(InScale, 1, 16);
    if (LongEdge * Scale <= MaxViewEdge)
    {
      // Nearest neighbour keeps pixel edges sharp for measuring.
      OutScale = Scale;
      InitImage(OutImage, InRect.Width() * Scale, InRect.Height() * Scale);
      const TArrayView64<const FColor> Source = Crop.AsBGRA8();
      const TArrayView64<FColor> Dest = OutImage.AsBGRA8();
      for (int32 Y = 0; Y < OutImage.SizeY; ++Y)
      {
        for (int32 X = 0; X < OutImage.SizeX; ++X)
        {
          Dest[static_cast<int64>(Y) * OutImage.SizeX + X] =
              Source[static_cast<int64>(Y / Scale) * Crop.SizeX + X / Scale];
        }
      }
      return true;
    }
    // Too big even at the requested factor: the largest integer factor that
    // fits, or a plain downscale.
    const int32 FittingScale = MaxViewEdge / LongEdge;
    if (FittingScale >= 1)
    {
      return Zoom(InImage, InRect, FittingScale, OutImage, OutScale, OutError);
    }
    OutScale = static_cast<double>(MaxViewEdge) / LongEdge;
    Resize(Crop, FMath::Max(1, FMath::RoundToInt32(Crop.SizeX * OutScale)),
           FMath::Max(1, FMath::RoundToInt32(Crop.SizeY * OutScale)),
           OutImage);
    return true;
  }

  void SideBySide(const FImage &InLeft, const FImage &InRight,
                  FImage &OutImage)
  {
    FImage Right;
    Resize(InRight, InLeft.SizeX, InLeft.SizeY, Right);

    InitImage(OutImage, InLeft.SizeX * 2 + SideBySideGap, InLeft.SizeY);
    const TArrayView64<FColor> Dest = OutImage.AsBGRA8();
    for (FColor &Pixel : Dest)
    {
      Pixel = GapColor;
    }
    const TArrayView64<const FColor> LeftPixels = InLeft.AsBGRA8();
    const TArrayView64<const FColor> RightPixels = Right.AsBGRA8();
    for (int32 Y = 0; Y < InLeft.SizeY; ++Y)
    {
      FColor *Row = &Dest[static_cast<int64>(Y) * OutImage.SizeX];
      FMemory::Memcpy(Row, &LeftPixels[static_cast<int64>(Y) * InLeft.SizeX],
                      InLeft.SizeX * sizeof(FColor));
      FMemory::Memcpy(Row + InLeft.SizeX + SideBySideGap,
                      &RightPixels[static_cast<int64>(Y) * InLeft.SizeX],
                      InLeft.SizeX * sizeof(FColor));
    }
  }

  double MeanDifference(const FImage &InA, const FImage &InB)
  {
    FImage B;
    Resize(InB, InA.SizeX, InA.SizeY, B);
    const TArrayView64<const FColor> APixels = InA.AsBGRA8();
    const TArrayView64<const FColor> BPixels = B.AsBGRA8();
    uint64 Sum = 0;
    for (int64 Index = 0; Index < APixels.Num(); ++Index)
    {
      const FColor &A = APixels[Index];
      const FColor &C = BPixels[Index];
      Sum += FMath::Abs(A.R - C.R) + FMath::Abs(A.G - C.G) +
             FMath::Abs(A.B - C.B);
    }
    return APixels.Num() == 0
               ? 0.0
               : 100.0 * static_cast<double>(Sum) /
                     (static_cast<double>(APixels.Num()) * 3.0 * 255.0);
  }

  bool CropForTexture(const FImage &InSource, const FIntRect &InRect,
                      ECropMode InMode, ECropShape InShape, int32 InThreshold,
                      FIntPoint InOutputSize, FImage &OutImage,
                      FString &OutError)
  {
    if (!CheckRect(InSource, InRect, TEXT("reference image"), OutError))
    {
      return false;
    }
    const FIntPoint OutputSize(
        InOutputSize.X > 0 ? InOutputSize.X : InRect.Width(),
        InOutputSize.Y > 0 ? InOutputSize.Y : InRect.Height());
    if (OutputSize.X > MaxTextureEdge || OutputSize.Y > MaxTextureEdge)
    {
      OutError = FString::Printf(
          TEXT("The texture would be %d x %d; the limit is %d on each edge."),
          OutputSize.X, OutputSize.Y, MaxTextureEdge);
      return false;
    }

    FImage Crop;
    CopyRect(InSource, InRect, Crop);
    const TArrayView64<FColor> Pixels = Crop.AsBGRA8();

    if (InMode != ECropMode::Copy)
    {
      // The border of a tight crop is background. Keying above nearly all of
      // it, rather than above the darkest pixel, also clears textured
      // backgrounds such as leather or stone.
      TArray<uint8> Border;
      for (int32 X = 0; X < Crop.SizeX; ++X)
      {
        Border.Add(Brightness(Pixels[X]));
        Border.Add(Brightness(
            Pixels[static_cast<int64>(Crop.SizeY - 1) * Crop.SizeX + X]));
      }
      for (int32 Y = 0; Y < Crop.SizeY; ++Y)
      {
        Border.Add(Brightness(Pixels[static_cast<int64>(Y) * Crop.SizeX]));
        Border.Add(Brightness(
            Pixels[static_cast<int64>(Y) * Crop.SizeX + Crop.SizeX - 1]));
      }
      Border.Sort();
      const uint8 Background = Border[(Border.Num() - 1) * 9 / 10];
      uint8 High = 0;
      for (const FColor &Pixel : Pixels)
      {
        High = FMath::Max(High, Brightness(Pixel));
      }
      const int32 Floor =
          FMath::Min<int32>(Background + FMath::Max(0, InThreshold), 254);
      const float Range = static_cast<float>(FMath::Max(1, High - Floor));
      for (FColor &Pixel : Pixels)
      {
        const float Alpha = FMath::Clamp(
            (Brightness(Pixel) - Floor) / Range, 0.f, 1.f);
        Pixel.A = static_cast<uint8>(FMath::RoundToInt32(Alpha * Pixel.A));
        if (InMode == ECropMode::Mask)
        {
          Pixel.R = Pixel.G = Pixel.B = 255;
        }
      }
    }

    Resize(Crop, OutputSize.X, OutputSize.Y, OutImage);

    if (InShape == ECropShape::Circle)
    {
      const TArrayView64<FColor> Out = OutImage.AsBGRA8();
      const double RadiusX = OutImage.SizeX * 0.5;
      const double RadiusY = OutImage.SizeY * 0.5;
      const double MinRadius = FMath::Min(RadiusX, RadiusY);
      for (int32 Y = 0; Y < OutImage.SizeY; ++Y)
      {
        for (int32 X = 0; X < OutImage.SizeX; ++X)
        {
          const double DX = (X + 0.5 - RadiusX) / RadiusX;
          const double DY = (Y + 0.5 - RadiusY) / RadiusY;
          // Distance to the edge in output pixels, positive inside.
          const double Inside =
              (1.0 - FMath::Sqrt(DX * DX + DY * DY)) * MinRadius;
          const double Coverage = FMath::Clamp(Inside + 0.5, 0.0, 1.0);
          FColor &Pixel = Out[static_cast<int64>(Y) * OutImage.SizeX + X];
          Pixel.A = static_cast<uint8>(FMath::RoundToInt32(Pixel.A * Coverage));
        }
      }
    }
    return true;
  }

  UTexture2D *WriteTexture(const FString &InFolder, const FString &InName,
                           const FImage &InPixels, bool &OutCreated,
                           FString &OutError)
  {
    OutCreated = false;
    FText Reason;
    if (InName.IsEmpty() ||
        !FName::IsValidXName(InName,
                             INVALID_OBJECTNAME_CHARACTERS
                                 INVALID_LONGPACKAGE_CHARACTERS,
                             &Reason))
    {
      OutError = FString::Printf(TEXT("\"%s\" is not a valid asset name. %s"),
                                 *InName, *Reason.ToString());
      return nullptr;
    }
    if (InPixels.SizeX <= 0 || InPixels.SizeY <= 0 ||
        InPixels.Format != ERawImageFormat::BGRA8)
    {
      OutError = TEXT("No pixels to write.");
      return nullptr;
    }

    const FString PackageName = InFolder / InName;
    const FString ObjectPath = PackageName + TEXT(".") + InName;
    const bool bOnDisk = FPackageName::DoesPackageExist(PackageName);
    UPackage *InMemory = FindPackage(nullptr, *PackageName);
    UObject *Existing = nullptr;
    if (bOnDisk || InMemory)
    {
      Existing = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath,
                                  nullptr, LOAD_NoWarn | LOAD_Quiet);
    }
    UTexture2D *Texture = Cast<UTexture2D>(Existing);
    // A failed lookup of the path can leave an empty package in memory; that
    // one is reused. A package holding anything else is not.
    if (!Texture && (Existing || bOnDisk ||
                     (InMemory && InMemory->FindAssetInPackage())))
    {
      OutError = FString::Printf(
          TEXT("%s already exists and is not a texture; pick another name."),
          *PackageName);
      return nullptr;
    }
    if (!Texture)
    {
      UPackage *Package = CreatePackage(*PackageName);
      Texture = NewObject<UTexture2D>(Package, FName(*InName),
                                      RF_Public | RF_Standalone);
      OutCreated = true;
    }

    Texture->PreEditChange(nullptr);
    Texture->Source.Init(InPixels);
    // Uncompressed and flagged as UI art: mostly-blue art is otherwise
    // auto-detected as a normal map, and gradients show compression blocks.
    Texture->CompressionSettings = TC_EditorIcon;
    Texture->SRGB = true;
    Texture->LODGroup = TEXTUREGROUP_UI;
    Texture->MipGenSettings = TMGS_NoMipmaps;
    Texture->NeverStream = true;
    Texture->PostEditChange();
    Texture->MarkPackageDirty();
    if (OutCreated)
    {
      FAssetRegistryModule::AssetCreated(Texture);
    }
    return Texture;
  }

  bool SaveAsset(UObject *InAsset, FString &OutError)
  {
    if (!InAsset)
    {
      OutError = TEXT("No asset to save.");
      return false;
    }
    UPackage *Package = InAsset->GetOutermost();
    const FString Filename = FPackageName::LongPackageNameToFilename(
        Package->GetName(), FPackageName::GetAssetPackageExtension());
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.Error = GError;
    if (!UPackage::SavePackage(Package, InAsset, *Filename, Args))
    {
      OutError = FString::Printf(TEXT("Could not save %s."), *Filename);
      return false;
    }
    return true;
  }

  bool RenderWidget(UWidgetBlueprint *InBlueprint, FIntPoint InSize,
                    FImage &OutImage, FString &OutError)
  {
    if (!InBlueprint)
    {
      OutError = TEXT("No blueprint to render.");
      return false;
    }
    if (InSize.X <= 0 || InSize.Y <= 0 || InSize.X > MaxRenderEdge ||
        InSize.Y > MaxRenderEdge)
    {
      OutError = FString::Printf(
          TEXT("The render size must be between 1 and %d on each edge."),
          MaxRenderEdge);
      return false;
    }
    if (!FApp::CanEverRender())
    {
      OutError = TEXT("This editor cannot render (it runs without a GPU "
                      "device).");
      return false;
    }
    if (InBlueprint->Status != BS_UpToDate &&
        InBlueprint->Status != BS_UpToDateWithWarnings)
    {
      FKismetEditorUtilities::CompileBlueprint(
          InBlueprint, EBlueprintCompileOptions::SkipGarbageCollection);
    }
    if (InBlueprint->Status == BS_Error || !InBlueprint->GeneratedClass)
    {
      OutError = TEXT("The blueprint has compile errors; fix them before "
                      "rendering.");
      return false;
    }
    UWorld *World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
      OutError = TEXT("There is no editor world to create the widget in.");
      return false;
    }
    UClass *WidgetClass = InBlueprint->GeneratedClass;
    UUserWidget *Widget = CreateWidget<UUserWidget>(World, WidgetClass);
    if (!Widget)
    {
      OutError = TEXT("The widget could not be created.");
      return false;
    }
    // Design mode, as in the UMG designer: Construct graphs that expect a
    // running game do not run.
    Widget->SetDesignerFlags(EWidgetDesignFlags::Designing);
    // Textures compile asynchronously after load or creation; draw only
    // final data, not the placeholder.
    FAssetCompilingManager::Get().FinishAllCompilation();

    const TSharedRef<SWidget> SlateWidget = Widget->TakeWidget();
    const FVector2D DrawSize(InSize.X, InSize.Y);
    UTextureRenderTarget2D *Target =
        FWidgetRenderer::CreateTargetFor(DrawSize, TF_Bilinear, true);
    Target->ClearColor = FLinearColor::Black;

    // Several passes so layout caches and fonts settle.
    FWidgetRenderer Renderer(true, true);
    for (int32 Pass = 0; Pass < 4; ++Pass)
    {
      Renderer.DrawWidget(Target, SlateWidget, DrawSize, 0.016f);
      FlushRenderingCommands();
    }

    FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
    ReadFlags.SetLinearToGamma(false);
    TArray<FColor> Pixels;
    FTextureRenderTargetResource *Resource =
        Target->GameThread_GetRenderTargetResource();
    const bool bRead = Resource && Resource->ReadPixels(Pixels, ReadFlags) &&
                       Pixels.Num() == InSize.X * InSize.Y;
    Widget->RemoveFromParent();
    Target->MarkAsGarbage();
    if (!bRead)
    {
      OutError = TEXT("Reading the rendered pixels failed.");
      return false;
    }

    // The readback is gamma-encoded twice; undo one encode so the PNG
    // matches what the screen shows.
    InitImage(OutImage, InSize.X, InSize.Y);
    const TArrayView64<FColor> Out = OutImage.AsBGRA8();
    for (int32 Index = 0; Index < Pixels.Num(); ++Index)
    {
      FColor Pixel = FLinearColor(Pixels[Index]).ToFColor(false);
      Pixel.A = 255;
      Out[Index] = Pixel;
    }
    return true;
  }

}
