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

// Design import settings shared by the team (Figma and Photoshop imports):
// where imports go, the reference resolution and the font map. Stored in
// DefaultEditor.ini so they are committed with the project.
UCLASS(config = Editor, defaultconfig,
       meta = (DisplayName = "UI Widget Tool (design import)"))
class UUIWTDesignSettings : public UDeveloperSettings
{
  GENERATED_BODY()

public:
  virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

  static const UUIWTDesignSettings *Get()
  {
    return GetDefault<UUIWTDesignSettings>();
  }

  // Content folder new imports go to when none is given. Each blueprint
  // gets its own folder inside it. Empty means /Game/UI/Imported.
  UPROPERTY(config, EditAnywhere, Category = "Import",
            meta = (ContentDir))
  FDirectoryPath DefaultImportFolder;

  // Content folder for the child blueprints made from design components,
  // shared by every screen that uses them. Empty means the default import
  // folder's Components folder.
  UPROPERTY(config, EditAnywhere, Category = "Import",
            meta = (ContentDir))
  FDirectoryPath ComponentsFolder;

  // The screen size designs are made at. A design frame of this size is
  // imported as a screen (a canvas that follows the screen's size), any
  // other size as a single widget.
  UPROPERTY(config, EditAnywhere, Category = "Import",
            meta = (ClampMin = "1", ClampMax = "16384"))
  FIntPoint ReferenceResolution = FIntPoint(1920, 1080);

  // Design fonts and the UE fonts they map to. Text in an unmapped font
  // uses UMG's default font and is listed in the import report.
  UPROPERTY(config, EditAnywhere, Category = "Fonts",
            meta = (TitleProperty = "{Family} {Style}"))
  TArray<FUIWTFontMapping> Fonts;

  // Hidden Photoshop layers are imported collapsed, so they can be shown in
  // UE later. Off leaves them out.
  UPROPERTY(config, EditAnywhere, Category = "Photoshop")
  bool bImportHiddenLayers = true;

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
