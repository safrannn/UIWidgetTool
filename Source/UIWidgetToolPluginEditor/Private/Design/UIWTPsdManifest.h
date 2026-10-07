#pragma once

#include "Core/UIWTDesignTree.h"
#include "CoreMinimal.h"

// The Photoshop reader (import-ps.md step 2): the UXP exporter's folder
// (manifest.json, layers/<id>.png, composite.png) → the shared design tree.
// Photoshop has already rendered every leaf (effects baked in), so this only
// turns absolute document-pixel bounds into boxes relative to the parent at
// 1x, and point sizes into pixels.
namespace UIWTPsdManifest
{
  struct FOptions
  {
    // The design tree's referenceSize (UUIWTDesignSettings).
    FIntPoint ReferenceSize = FIntPoint(1920, 1080);
    // Hidden layers: imported collapsed, or left out.
    bool bImportHidden = true;
  };

  struct FResult
  {
    UIWTDesignTree::FDocument Document;
    int32 NodeCount = 0;
    // exportedAt from the manifest: tells a re-import whether anything was
    // exported since.
    FString ExportedAt;
  };

  // Reads a manifest. Image paths stay relative to InDirectory, which holds
  // the layer PNGs (their headers give image sizes).
  bool Read(const FString &InManifestJson, const FString &InDirectory, const FOptions &InOptions,
            FResult &OutResult, FString &OutError);

  // Copies the export folder of InManifestFile into the project's cache
  // (Saved/UIWidgetTool/Psd/<folder>), reads it, and writes design.json and
  // reference.png (the composite) there, so the import never writes into
  // the designer's folder. OutDesignFile is design.json's absolute path.
  bool Prepare(const FString &InManifestFile, const FOptions &InOptions, FString &OutDesignFile,
               FResult &OutResult, FString &OutError);

  // Absolute: Saved/UIWidgetTool/Psd/<export folder name>_<hash of its path>
  FString GetCacheDirectory(const FString &InManifestFile);
}
