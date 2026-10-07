#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"

#include "UIWTDesignSettings.generated.h"

class UFont;

// One design font mapped to a UE font asset.
USTRUCT()
struct FUIWTFontMapping
{
  GENERATED_BODY()

  // The design's font family, for example Inter. Compared ignoring case.
  UPROPERTY(EditAnywhere, Category = "Font")
  FString Family;

  // The design's style, for example Bold or Semi Bold Italic. Compared
  // ignoring case and spaces. Empty matches every style of the family that
  // has no mapping of its own.
  UPROPERTY(EditAnywhere, Category = "Font")
  FString Style;

  // The font asset used for text in this family and style.
  UPROPERTY(EditAnywhere, Category = "Font")
  TSoftObjectPtr<UFont> Font;

  // The typeface in Font. None picks the typeface named like the design's
  // style (ignoring case and spaces), then Regular, then the first one.
  UPROPERTY(EditAnywhere, Category = "Font")
  FName Typeface;
};

// Design import setting is stored in DefaultEditor.ini.
UCLASS(config = Editor, defaultconfig,
       meta = (DisplayName = "UI Widget Tool"))
class UUIWTDesignSettings : public UDeveloperSettings
{
  GENERATED_BODY()

public:
  virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

  static const UUIWTDesignSettings *Get()
  {
    return GetDefault<UUIWTDesignSettings>();
  }

  // Default at /Game/UI/Imported.
  UPROPERTY(config, EditAnywhere, Category = "Import",
            meta = (ContentDir))
  FDirectoryPath DefaultImportFolder;

  // Content folder for the child blueprints made from design components,
  // shared by every screen that uses them. Empty means the default import
  // folder's Components folder. Config-only: not shown on the settings page.
  UPROPERTY(config)
  FDirectoryPath ComponentsFolder;

  // The screen size designs are made at. A design frame of this size is
  // imported as a screen (a canvas that follows the screen's size), any
  // other size as a single widget.
  UPROPERTY(config, EditAnywhere, Category = "Import",
            meta = (ClampMin = "1", ClampMax = "16384"))
  FIntPoint ReferenceResolution = FIntPoint(1920, 1080);

  // Design fonts and the UE fonts they map to. A family listed here never
  // uses the font library. Text in a font that is in neither uses UMG's
  // default font and is listed in the import report.
  UPROPERTY(config, EditAnywhere, Category = "Import|Fonts",
            meta = (TitleProperty = "{Family} {Style}"))
  TArray<FUIWTFontMapping> Fonts;

  // Content folder of the font library: a folder per family with its Font
  // asset and Font Faces, e.g. Inter/Inter and Inter/Inter-SemiBold. Text in
  // a family the font map doesn't list uses the family's Font here, with the
  // typeface named like the design's style. Empty means /Game/UI/Fonts.
  UPROPERTY(config, EditAnywhere, Category = "Import|Fonts",
            meta = (ContentDir))
  FDirectoryPath FontLibraryFolder;

  // Figma and Photoshop imports take fonts that are in neither the font map
  // nor the library from the fonts installed on this computer (on Windows
  // the Fonts folders) into the library, before trying Google Fonts. Being installed
  // doesn't mean a font may ship in a game (Windows' own fonts may not):
  // check the license of every font the import lists as taken from this
  // computer. Fonts whose file forbids embedding are never taken, and
  // variable fonts are skipped (UE can't pick a weight from them).
  UPROPERTY(config, EditAnywhere, Category = "Import|Fonts")
  bool bUseInstalledFonts = true;

  // Figma and Photoshop imports download fonts that are in neither the font
  // map, the library nor (with bUseInstalledFonts) this computer's fonts from
  // Google Fonts (fonts.google.com) into the library, as static TrueType
  // files.
  // Google Fonts are open source (OFL or Apache), so they can ship in a game.
  UPROPERTY(config, EditAnywhere, Category = "Import|Fonts")
  bool bDownloadGoogleFonts = true;

  // Hidden Photoshop layers are imported collapsed, so they can be shown in
  // UE later. Off leaves them out.
  UPROPERTY(config, EditAnywhere, Category = "Import|Photoshop")
  bool bImportHiddenLayers = true;

  // Scale Figma renders layers it can't draw as widgets at (vectors,
  // gradients, layers with effects), and the frame's reference image. 2
  // keeps them sharp at up to twice the reference resolution.
  UPROPERTY(config, EditAnywhere, Category = "Import|Figma",
            meta = (ClampMin = "1", ClampMax = "4"))
  float RenderScale = 2.f;

  // Image fills with more texture pixels per design pixel than this are
  // downscaled on import (a 4000-pixel photo in a 300-pixel card).
  UPROPERTY(config, EditAnywhere, Category = "Import|Figma",
            meta = (ClampMin = "1", ClampMax = "8"))
  float MaxImageScale = 2.f;

  // Seconds a single Figma request may take.
  UPROPERTY(config, EditAnywhere, Category = "Import|Figma",
            meta = (ClampMin = "10", Units = "s"))
  int32 RequestTimeoutSeconds = 120;

  // DefaultImportFolder as a long package path without a trailing slash.
  FString GetImportFolder() const
  {
    const FString Folder = Normalize(DefaultImportFolder.Path);
    return Folder.IsEmpty() ? FString(TEXT("/Game/UI/Imported")) : Folder;
  }

  // ComponentsFolder as a long package path without a trailing slash.
  FString GetComponentsFolder() const
  {
    const FString Folder = Normalize(ComponentsFolder.Path);
    return Folder.IsEmpty() ? GetImportFolder() / TEXT("Components") : Folder;
  }

  // FontLibraryFolder as a long package path without a trailing slash.
  FString GetFontLibraryFolder() const
  {
    const FString Folder = Normalize(FontLibraryFolder.Path);
    return Folder.IsEmpty() ? FString(TEXT("/Game/UI/Fonts")) : Folder;
  }

private:
  static FString Normalize(const FString &InPath)
  {
    FString Path = InPath.TrimStartAndEnd();
    Path.ReplaceInline(TEXT("\\"), TEXT("/"));
    while (Path.EndsWith(TEXT("/")))
    {
      Path.LeftChopInline(1);
    }
    return Path.StartsWith(TEXT("/")) ? Path : FString();
  }
};
