#include "UIWTDesignSources.h"

#include "Core/UIWTDesignSettings.h"
#include "Misc/Paths.h"

bool UIWTDesignSources::PreparePsd(const FString &InManifestFile, FPsdExport &OutExport,
                                   FString &OutError)
{
  const FString Manifest = InManifestFile.TrimStartAndEnd().TrimQuotes();
  if (!FPaths::GetExtension(Manifest).Equals(TEXT("json"), ESearchCase::IgnoreCase))
  {
    OutError = TEXT("Give the manifest.json of a Photoshop export (Export for Unreal in the "
                    "UIWidgetTool Photoshop panel).");
    return false;
  }
  if (!FPaths::FileExists(Manifest))
  {
    OutError = FString::Printf(TEXT("%s doesn't exist. Export from Photoshop again, to the same "
                                    "folder when re-importing."),
                               *Manifest);
    return false;
  }
  const UUIWTDesignSettings *Settings = UUIWTDesignSettings::Get();
  UIWTPsdManifest::FOptions Options;
  Options.ReferenceSize = Settings->ReferenceResolution;
  Options.bImportHidden = Settings->bImportHiddenLayers;
  if (!UIWTPsdManifest::Prepare(Manifest, Options, OutExport.DesignFile, OutExport.Read, OutError))
  {
    return false;
  }
  const FString Reference = FPaths::GetPath(OutExport.DesignFile) / TEXT("reference.png");
  OutExport.ReferenceImage = FPaths::FileExists(Reference) ? Reference : FString();
  return true;
}
