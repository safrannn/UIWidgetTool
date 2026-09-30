#pragma once

#include "CoreMinimal.h"
#include "Design/UIWTPsdManifest.h"

// Reading a design source with the project's settings, for the tools, the
// console commands and the chat panel's import menu alike.
namespace UIWTDesignSources
{
  // A Photoshop export, copied into the project and read as a design tree.
  struct FPsdExport
  {
    FString DesignFile;
    // The export's composite.png, or empty when it has none.
    FString ReferenceImage;
    UIWTPsdManifest::FResult Read;
  };

  // UIWTPsdManifest::Prepare with UUIWTDesignSettings' reference resolution
  // and hidden-layer setting. InManifestFile must be a manifest.json.
  bool PreparePsd(const FString &InManifestFile, FPsdExport &OutExport, FString &OutError);
}
