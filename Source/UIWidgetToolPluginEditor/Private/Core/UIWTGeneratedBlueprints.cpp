#include "UIWTGeneratedBlueprints.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UIWidgetToolPlugin.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"

#define LOCTEXT_NAMESPACE "UIWidgetTool"

namespace UIWTGenerated
{

  namespace
  {
    // With the trailing slash RegisterMountPoint expects.
    const TCHAR *const MountRoot = TEXT("/UIWidgetToolGenerated/");
    bool bMounted = false;

    // "/UIWidgetToolGenerated" - the form asset paths and the registry use.
    FString GetMountRootNoSlash()
    {
      FString Root(MountRoot);
      Root.RemoveFromEnd(TEXT("/"));
      return Root;
    }

    FString StripCopySuffix(const FString &InName)
    {
      int32 Index;
      if (!InName.FindLastChar(TEXT('_'), Index))
      {
        return InName;
      }
      const FString Tail = InName.Mid(Index + 1);
      if (!Tail.StartsWith(TEXT("Copy")))
      {
        return InName;
      }
      const FString Digits = Tail.Mid(4);
      for (const TCHAR C : Digits)
      {
        if (!FChar::IsDigit(C))
        {
          return InName;
        }
      }
      return InName.Left(Index);
    }
  }

  FString GetDirectory()
  {
    return FPaths::ConvertRelativePathToFull(
        FPaths::Combine(UIWT::GetSavePath(), TEXT("WidgetBlueprints")));
  }

  void RegisterMountPoint()
  {
    if (bMounted)
    {
      return;
    }
    const FString Directory = GetDirectory();
    // The mount needs an existing directory.
    IFileManager::Get().MakeDirectory(*Directory, true);
    FPackageName::RegisterMountPoint(MountRoot, Directory);
    bMounted = true;

    if (IAssetRegistry *Registry = IAssetRegistry::Get())
    {
      Registry->ScanPathsSynchronous({GetMountRootNoSlash()});
    }
  }

  void UnregisterMountPoint()
  {
    if (!bMounted)
    {
      return;
    }
    FPackageName::UnRegisterMountPoint(MountRoot, GetDirectory());
    bMounted = false;
  }

  bool IsGeneratedPath(const FString &InPath)
  {
    return InPath.StartsWith(MountRoot);
  }

  UWidgetBlueprint *
  FindWidgetBlueprint(const TSoftClassPtr<UUserWidget> &InClass)
  {
    if (InClass.IsNull())
    {
      return nullptr;
    }

    const FSoftObjectPath ClassPath = InClass.ToSoftObjectPath();
    const FString PackageName = ClassPath.GetLongPackageName();
    if (PackageName.IsEmpty())
    {
      return nullptr;
    }

    FString AssetName = ClassPath.GetAssetName();
    if (AssetName.EndsWith(TEXT("_C")))
    {
      AssetName.LeftChopInline(2);
    }
    if (UWidgetBlueprint *Blueprint = LoadObject<UWidgetBlueprint>(
            nullptr,
            *FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName)))
    {
      return Blueprint;
    }

    TArray<FAssetData> Assets;
    IAssetRegistry::GetChecked().GetAssetsByPackageName(FName(*PackageName),
                                                        Assets);
    for (const FAssetData &Asset : Assets)
    {
      if (UWidgetBlueprint *Blueprint =
              Cast<UWidgetBlueprint>(Asset.GetAsset()))
      {
        return Blueprint;
      }
    }
    return nullptr;
  }

  UWidgetBlueprint *DuplicateWidgetBlueprint(UWidgetBlueprint *InSource,
                                             FText &OutError)
  {
    if (!InSource)
    {
      OutError = LOCTEXT("DupNoSource", "No source blueprint to duplicate.");
      return nullptr;
    }
    if (!bMounted)
    {
      OutError = LOCTEXT("DupNoMount",
                         "The generated-blueprint folder is not mounted.");
      return nullptr;
    }

    const FString Base = StripCopySuffix(InSource->GetName());
    const FString Folder = GetMountRootNoSlash();

    FString AssetName;
    for (int32 Attempt = 1; Attempt < 1000; ++Attempt)
    {
      AssetName = Attempt == 1 ? Base + TEXT("_Copy")
                               : FString::Printf(TEXT("%s_Copy%d"), *Base, Attempt);
      if (!FPackageName::DoesPackageExist(Folder / AssetName))
      {
        break;
      }
      AssetName.Reset();
    }
    if (AssetName.IsEmpty())
    {
      OutError = LOCTEXT("DupNoName", "Could not find a free copy name.");
      return nullptr;
    }

    IAssetTools &AssetTools =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
    UWidgetBlueprint *Copy = Cast<UWidgetBlueprint>(
        AssetTools.DuplicateAsset(AssetName, Folder, InSource));
    if (!Copy)
    {
      OutError = FText::Format(
          LOCTEXT("DupFailed", "DuplicateAsset refused to create {0}/{1}."),
          FText::FromString(Folder), FText::FromString(AssetName));
      return nullptr;
    }

    if (!SaveWidgetBlueprint(Copy, OutError))
    {
      return nullptr;
    }
    return Copy;
  }

  bool SaveWidgetBlueprint(UWidgetBlueprint *InBlueprint, FText &OutError)
  {
    if (!InBlueprint)
    {
      OutError = LOCTEXT("SaveNull", "No blueprint to save.");
      return false;
    }
    UPackage *Package = InBlueprint->GetOutermost();
    const FString PackageName = Package->GetName();
    if (!IsGeneratedPath(PackageName))
    {
      OutError = FText::Format(
          LOCTEXT("SaveOutsideMount",
                  "{0} is not a generated copy; only copies under {1} may "
                  "be saved by the tool."),
          FText::FromString(PackageName), FText::FromString(MountRoot));
      return false;
    }
    if (InBlueprint->Status == BS_Error)
    {
      OutError = LOCTEXT("SaveCompileError",
                         "The blueprint has compile errors; fix them and "
                         "compile before saving.");
      return false;
    }

    const FString Filename = FPackageName::LongPackageNameToFilename(
        PackageName, FPackageName::GetAssetPackageExtension());

    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.Error = GError;
    if (!UPackage::SavePackage(Package, InBlueprint, *Filename, Args))
    {
      OutError = FText::Format(LOCTEXT("SaveFailed", "Could not save {0}."),
                               FText::FromString(Filename));
      return false;
    }
    return true;
  }

  bool ReloadWidgetBlueprint(UWidgetBlueprint *InBlueprint, FText &OutError)
  {
    if (!InBlueprint)
    {
      OutError = LOCTEXT("ReloadNull", "No blueprint to reload.");
      return false;
    }
    UPackage *Package = InBlueprint->GetOutermost();
    if (!IsGeneratedPath(Package->GetName()))
    {
      OutError = LOCTEXT("ReloadOutsideMount",
                         "Only generated copies are reloaded by the tool.");
      return false;
    }
    if (UAssetEditorSubsystem *Editors =
            GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
                    : nullptr)
    {
      Editors->CloseAllEditorsForAsset(InBlueprint);
    }
    return UPackageTools::ReloadPackages(
        {Package}, OutError, EReloadPackagesInteractionMode::AssumePositive);
  }

  bool RenameWidgetBlueprint(UWidgetBlueprint *InBlueprint,
                             const FString &InNewName, FText &OutError)
  {
    if (!InBlueprint)
    {
      OutError = LOCTEXT("RenameNull", "No blueprint to rename.");
      return false;
    }
    const FString OldPackageName = InBlueprint->GetOutermost()->GetName();

    FText Reason;
    if (InNewName.IsEmpty() ||
        !FName::IsValidXName(InNewName,
                             INVALID_OBJECTNAME_CHARACTERS
                             INVALID_LONGPACKAGE_CHARACTERS,
                             &Reason))
    {
      OutError = Reason.IsEmpty()
                     ? LOCTEXT("RenameEmpty", "The name cannot be empty.")
                     : Reason;
      return false;
    }
    if (InNewName == InBlueprint->GetName())
    {
      return true;
    }

    const FString Folder = FPackageName::GetLongPackagePath(OldPackageName);
    if (FPackageName::DoesPackageExist(Folder / InNewName))
    {
      OutError = FText::Format(
          LOCTEXT("RenameTaken", "A blueprint named {0} already exists in {1}."),
          FText::FromString(InNewName), FText::FromString(Folder));
      return false;
    }

    if (UAssetEditorSubsystem *Editors =
            GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
                    : nullptr)
    {
      Editors->CloseAllEditorsForAsset(InBlueprint);
    }

    const FString OldObjectPath = InBlueprint->GetPathName();
    IAssetTools &AssetTools =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
    if (!AssetTools.RenameAssets({FAssetRenameData(InBlueprint, Folder,
                                                   InNewName)}))
    {
      OutError = FText::Format(
          LOCTEXT("RenameFailed", "Could not rename {0} to {1}."),
          FText::FromString(OldPackageName), FText::FromString(InNewName));
      return false;
    }

    // A redirector keeps unloaded referencers of a Content/ blueprint
    // working. Nothing under Content/ may reference a copy, so in the mount
    // it is only clutter in Saved/.
    if (IsGeneratedPath(OldPackageName))
    {
      if (UObjectRedirector *Redirector =
              FindObject<UObjectRedirector>(nullptr, *OldObjectPath))
      {
        ObjectTools::DeleteObjects({Redirector}, false);
      }
    }
    return true;
  }

  bool DeleteWidgetBlueprint(const TSoftClassPtr<UUserWidget> &InClass,
                             FText &OutError)
  {
    const FString PackageName = InClass.ToSoftObjectPath().GetLongPackageName();
    if (!IsGeneratedPath(PackageName))
    {
      OutError = LOCTEXT("DeleteOutsideMount",
                         "Only generated copies are deleted by the tool.");
      return false;
    }
    UWidgetBlueprint *Blueprint = FindWidgetBlueprint(InClass);
    if (!Blueprint)
    {
      // Already gone (Saved/ wiped, or deleted by hand) - nothing to do.
      return true;
    }
    if (UAssetEditorSubsystem *Editors =
            GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
                    : nullptr)
    {
      Editors->CloseAllEditorsForAsset(Blueprint);
    }
    if (ObjectTools::DeleteObjects({Blueprint}, false) == 0)
    {
      OutError = FText::Format(
          LOCTEXT("DeleteFailed", "Could not delete {0}."),
          FText::FromString(PackageName));
      return false;
    }
    return true;
  }

}

#undef LOCTEXT_NAMESPACE
