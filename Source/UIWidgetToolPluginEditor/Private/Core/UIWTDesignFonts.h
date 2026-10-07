#pragma once

#include "Core/UIWTDesignTree.h"
#include "CoreMinimal.h"

class UFont;

// The fonts a design uses, found in the project or downloaded, before the
// design is converted (import-figma.md → Fonts).
//
// A design font is found when the font map (UUIWTDesignSettings::Fonts) has
// its family, or when the font library folder has a Font asset for the family
// with a typeface named like the style. A missing one is taken from the fonts
// installed on this computer (when bUseInstalledFonts), else downloaded from
// Google Fonts as a static TrueType file (when bDownloadGoogleFonts). Either
// way the file is imported as a Font Face and added to the family's library
// Font as a typeface named like the style: <library>/<Family>/<Family> and
// <Family>-<Style>. The converter then uses it through the font resolver like
// any library font.
namespace UIWTDesignFonts
{
  // One face of a font file, as its OpenType name table names it.
  struct FInstalledFace
  {
    // Absolute.
    FString File;
    // The face's index in a collection (.ttc, .otc); 0 otherwise.
    int32 Index = 0;
    // Typographic family and subfamily (name IDs 16 and 17), else the legacy
    // ones (1 and 2): Inter / Semi Bold.
    FString Family;
    FString Style;
    // Legacy family and subfamily (1 and 2): Inter SemiBold / Regular.
    FString LegacyFamily;
    FString LegacyStyle;
    // Name ID 6, e.g. Inter-SemiBold. Figma gives it too.
    FString PostScript;
    // OS/2 fsType: the embedding permissions the font's maker set.
    uint16 FsType = 0;
    // Has an fvar table: one file for every weight, which UE can't pick from.
    bool bVariable = false;

    // fsType says Restricted License embedding: the font may not be
    // embedded in anything, a game included.
    bool IsRestricted() const { return (FsType & 0x000F) == 0x0002; }
  };

  // Reads the faces of a TrueType or OpenType file or collection (only its
  // table directory, name, OS/2 and fvar headers). Empty for anything else.
  TArray<FInstalledFace> ReadFontFile(const FString &InFile);

  // The folders installed fonts are in: on Windows the system Fonts folder
  // and the per-user one (%LOCALAPPDATA%\Microsoft\Windows\Fonts).
  TArray<FString> GetInstalledFontFolders();

  // The installed face for InFont: the same PostScript name, else the same
  // family and style. Variable fonts and fonts whose license forbids
  // embedding are skipped; OutWhyNot then says so. Null when there is none.
  const FInstalledFace *FindInstalledFace(const TArray<FInstalledFace> &InFaces,
                                          const UIWTDesignTree::FFontRef &InFont,
                                          FString &OutWhyNot);
  // Every distinct family and style the design's text runs use, the screen's
  // and its main components', in the order they first appear.
  TArray<UIWTDesignTree::FFontRef> CollectFonts(const UIWTDesignTree::FDocument &InDocument);

  // The family's Font in the library folder: <library>/<Family>/<Family>,
  // or a Font asset anywhere in the folder named like the family (ignoring
  // case, spaces and a Font_ prefix or _Font suffix). Null when there is none.
  UFont *FindLibraryFont(const FString &InFamily);

  // The typeface of InFont named like InStyle, ignoring case, spaces and
  // dashes. None when it has none.
  FName FindTypeface(const UFont *InFont, const FString &InStyle);

  // "Semi Bold Italic" → 600 and italic. False for a style that names more
  // than a weight and italic ("Condensed Bold"): Google's API can't select it.
  bool ParseStyle(const FString &InStyle, int32 &OutWeight, bool &bOutItalic);

  struct FFontsResult
  {
    // "Inter Semi Bold", one per design font, by what happened to it.
    TArray<FString> Mapped;
    TArray<FString> InLibrary;
    // Taken from the fonts installed on this computer, with the file, e.g.
    // "Inter Semi Bold (C:\Windows\Fonts\Inter-SemiBold.ttf)". Their
    // licenses are the user's to check.
    TArray<FString> Installed;
    TArray<FString> Downloaded;
    // "Foo Bold: why", for fonts that stay unmapped (the import reports
    // them as fontUnmapped and uses the default font).
    TArray<FString> Failed;
    // Font and Font Face assets created or changed (all saved).
    TArray<FString> Assets;
  };

  using FOnFontsReady = TFunction<void(const FFontsResult &)>;

  // Finds the design's fonts and downloads the missing ones (see above).
  // Never fails the import: fonts that can't be had are listed in Failed.
  // InOnDone runs on the game thread, never before EnsureFonts returns.
  void EnsureFonts(const FString &InDesignFile, FOnFontsReady InOnDone);

  // The result as a JSON object text, for tools and logs.
  FString ResultToJson(const FFontsResult &InResult);

  // One line for a notification, e.g. "Downloaded 2 fonts from Google Fonts:
  // Inter Bold, Inter Regular." Empty when nothing was imported or failed.
  FString Summary(const FFontsResult &InResult);
}
