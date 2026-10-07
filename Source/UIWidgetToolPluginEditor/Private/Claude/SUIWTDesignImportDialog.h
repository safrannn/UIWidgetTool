#pragma once

#include "CoreMinimal.h"

class SWidget;

enum class EUIWTDesignSource : uint8
{
  Figma,
  Photoshop,
  // import-image.md: Claude reads the image into a design tree.
  Image
};

// What the import dialog asks for.
struct FUIWTDesignImportChoice
{
  // The Figma frame link, the Photoshop export's manifest.json, or the
  // image file.
  FString Source;
  // Empty: the design settings' default import folder.
  FString TargetFolder;
  // Images: 0 = 1x, 1 = 2x, 2 = fit to the reference resolution
  // (UIWTImageReader::EScale).
  int32 ImageScale = 0;
};

// The chat panel's import menu: the dialog that asks where a Figma frame or
// a Photoshop export comes from and goes to, and the per-source "AI pass"
// toggle (import-tree.md → Claude pass), kept in the local settings.
namespace UIWTDesignImportDialog
{
  bool IsAIPassOn(EUIWTDesignSource InSource);
  void SetAIPass(EUIWTDesignSource InSource, bool bInOn);

  // What the pass needs, for the toggle's tooltip.
  FText DescribeAIPass();
  // The pass's model setting, as shown in the settings.
  FText AIPassModelName();

  // Modal. InOutChoice fills the fields and gets what was entered; false
  // when cancelled.
  bool Show(EUIWTDesignSource InSource, const TSharedPtr<SWidget> &InParent,
            FUIWTDesignImportChoice &InOutChoice);
}
