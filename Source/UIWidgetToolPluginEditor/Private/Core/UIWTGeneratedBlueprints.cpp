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
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "UIWidgetToolPlugin.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintOperationUtils.h"

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

    // Free when no widget of that name sits in the mount, flat or in its own
    // folder, on disk or only in memory, and no folder of that name exists.
    bool IsWidgetNameFree(const FString &InName)
    {
      const FString Folder = GetMountRootNoSlash() / InName;
      for (const FString &PackageName : {Folder, Folder / InName})
      {
        if (FPackageName::DoesPackageExist(PackageName) ||
            FindPackage(nullptr, *PackageName))
        {
          return false;
        }
      }
      return !IFileManager::Get().DirectoryExists(*GetFolderOnDisk(Folder));
    }

    void CloseEditors(UObject *InAsset)
    {
      if (UAssetEditorSubsystem *Editors =
              GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
                      : nullptr)
      {
        Editors->CloseAllEditorsForAsset(InAsset);
      }
    }

    // AssetTools stops a rename with a modal "Source code, config INI, and
    // text files may need Find/Replace" question whenever a class default
    // object refers to a renamed asset, and the manager's entries live on
    // the settings object's defaults. The callers repoint entries
    // themselves, so they are detached for the rename and put back as they
    // were.
    bool RenameAssetsWithoutEntryPrompt(const TArray<FAssetRenameData> &InRenames)
    {
      TSet<FSoftObjectPath> RenamedClasses;
      for (const FAssetRenameData &Rename : InRenames)
      {
        const UBlueprint *Blueprint = Cast<UBlueprint>(Rename.Asset.Get());
        if (Blueprint && Blueprint->GeneratedClass)
        {
          RenamedClasses.Add(FSoftObjectPath(Blueprint->GeneratedClass));
        }
      }
      TArray<FWidgetPreviewObject> &Entries =
          UUIWidgetPreviewObjectManagerSettings::Get()->WidgetPreviewObjects;
      TArray<TPair<int32, TSoftClassPtr<UUserWidget>>> Detached;
      for (int32 Index = 0; Index < Entries.Num(); ++Index)
      {
        TSoftClassPtr<UUserWidget> &Class = Entries[Index].WidgetClass;
        if (RenamedClasses.Contains(Class.ToSoftObjectPath()))
        {
          Detached.Emplace(Index, Class);
          Class.Reset();
        }
      }

      IAssetTools &AssetTools =
          FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools")
              .Get();
      const bool bRenamed = AssetTools.RenameAssets(InRenames);

      for (const TPair<int32, TSoftClassPtr<UUserWidget>> &Pair : Detached)
      {
        Entries[Pair.Key].WidgetClass = Pair.Value;
      }
      return bRenamed;
    }

    // A redirector keeps unloaded referencers of a Content/ asset working.
    // Nothing under Content/ may reference the mount, so there it is only
    // clutter in Saved/.
    void DeleteMountRedirectors(const TArray<FString> &InOldObjectPaths)
    {
      TArray<UObject *> Redirectors;
      for (const FString &Path : InOldObjectPaths)
      {
        if (IsGeneratedPath(Path))
        {
          if (UObjectRedirector *Redirector =
                  FindObject<UObjectRedirector>(nullptr, *Path))
          {
            Redirectors.Add(Redirector);
          }
        }
      }
      if (Redirectors.Num() > 0)
      {
        ObjectTools::DeleteObjects(Redirectors, false);
      }
    }

    bool IsPackageFile(const FString &InFilename)
    {
      const FString Extension = FPaths::GetExtension(InFilename, true);
      return Extension == FPackageName::GetAssetPackageExtension() ||
             Extension == FPackageName::GetMapPackageExtension();
    }

    bool HasPackageFiles(const FString &InDirectory)
    {
      bool bFound = false;
      IFileManager::Get().IterateDirectoryRecursively(
          *InDirectory,
          [&bFound](const TCHAR *InPath, bool bInIsDirectory)
          {
            bFound = !bInIsDirectory && IsPackageFile(InPath);
            return !bFound;
          });
      return bFound;
    }

    // The reference image, renders, zooms and whatever Claude wrote: every
    // file under InFrom that is not a package, to the same place under InTo.
    void MoveLooseFiles(const FString &InFrom, const FString &InTo)
    {
      TArray<FString> Files;
      IFileManager::Get().FindFilesRecursive(Files, *InFrom, TEXT("*"), true,
                                             false);
      for (const FString &File : Files)
      {
        if (IsPackageFile(File))
        {
          continue;
        }
        FString Relative = File;
        FPaths::MakePathRelativeTo(Relative, *(InFrom + TEXT("/")));
        IFileManager::Get().Move(*(InTo / Relative), *File, false, true);
      }
    }

    // After a widget in its own folder was deleted: deletes the folder's
    // assets nothing outside it uses, then the folder on disk once no
    // package is left in it.
    void DeleteWidgetFolder(const FString &InFolder,
                            const FName InDeletedPackage)
    {
      IAssetRegistry &Registry = IAssetRegistry::GetChecked();
      TArray<FAssetData> Assets;
      Registry.GetAssetsByPath(FName(*InFolder), Assets, true);
      TSet<FName> FolderPackages{InDeletedPackage};
      for (const FAssetData &Asset : Assets)
      {
        FolderPackages.Add(Asset.PackageName);
      }

      TArray<UObject *> Unused;
      for (const FAssetData &Asset : Assets)
      {
        TArray<FName> Referencers;
        Registry.GetReferencers(Asset.PackageName, Referencers);
        const bool bUsedElsewhere = Referencers.ContainsByPredicate(
            [&FolderPackages](const FName InReferencer)
            { return !FolderPackages.Contains(InReferencer); });
        if (!bUsedElsewhere)
        {
          if (UObject *Object = Asset.GetAsset())
          {
            Unused.Add(Object);
          }
        }
      }
      // Forced: the deleted blueprint's class can still hold them in memory.
      if (Unused.Num() > 0)
      {
        ObjectTools::ForceDeleteObjects(Unused, false);
      }

      const FString Directory = GetFolderOnDisk(InFolder);
      if (!Directory.IsEmpty() && !HasPackageFiles(Directory))
      {
        IFileManager::Get().DeleteDirectory(*Directory, false, true);
      }
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

  FString GetFolder(const UWidgetBlueprint *InBlueprint)
  {
    return InBlueprint ? FPackageName::GetLongPackagePath(
                             InBlueprint->GetOutermost()->GetName())
                       : FString();
  }

  FString GetFolderOnDisk(const FString &InFolder)
  {
    FString Directory;
    if (!FPackageName::TryConvertLongPackageNameToFilename(InFolder + TEXT("/"),
                                                          Directory))
    {
      return FString();
    }
    Directory = FPaths::ConvertRelativePathToFull(Directory);
    FPaths::NormalizeDirectoryName(Directory);
    return Directory;
  }

  bool IsInOwnFolder(const UWidgetBlueprint *InBlueprint)
  {
    return InBlueprint &&
           FPackageName::GetShortName(GetFolder(InBlueprint)) ==
               InBlueprint->GetName();
  }

  bool MoveToOwnFolder(UWidgetBlueprint *InBlueprint, bool &OutMoved,
                       FText &OutError)
  {
    OutMoved = false;
    if (!InBlueprint)
    {
      OutError = LOCTEXT("MoveNull", "No blueprint to move.");
      return false;
    }
    if (IsInOwnFolder(InBlueprint))
    {
      return true;
    }

    const FString Name = InBlueprint->GetName();
    const FString OldFolder = GetFolder(InBlueprint);
    const FString NewFolder = OldFolder / Name;
    if (FPackageName::DoesPackageExist(NewFolder / Name) ||
        FindPackage(nullptr, *(NewFolder / Name)))
    {
      OutError = FText::Format(
          LOCTEXT("MoveTaken", "{0} already exists, so {1} cannot move there."),
          FText::FromString(NewFolder / Name), FText::FromString(Name));
      return false;
    }
    CloseEditors(InBlueprint);

    TArray<FAssetRenameData> Renames;
    TArray<FString> OldObjectPaths;
    Renames.Emplace(InBlueprint, NewFolder, Name);
    OldObjectPaths.Add(InBlueprint->GetPathName());
    // Where textures went before widgets had their own folder.
    TArray<FAssetData> OldTextures;
    IAssetRegistry::GetChecked().GetAssetsByPath(
        FName(*(OldFolder / (Name + TEXT("_Textures")))), OldTextures, true);
    for (const FAssetData &Asset : OldTextures)
    {
      UObject *Object = Asset.IsRedirector() ? nullptr : Asset.GetAsset();
      if (Object && !FPackageName::DoesPackageExist(NewFolder /
                                                    Object->GetName()))
      {
        Renames.Emplace(Object, NewFolder, Object->GetName());
        OldObjectPaths.Add(Object->GetPathName());
      }
    }

    if (!RenameAssetsWithoutEntryPrompt(Renames))
    {
      OutError = FText::Format(
          LOCTEXT("MoveFailed", "Could not move {0} into its own folder {1}."),
          FText::FromString(Name), FText::FromString(NewFolder));
      return false;
    }
    DeleteMountRedirectors(OldObjectPaths);
    const FString OldTextureDirectory =
        GetFolderOnDisk(OldFolder / (Name + TEXT("_Textures")));
    if (!OldTextureDirectory.IsEmpty() && !HasPackageFiles(OldTextureDirectory))
    {
      IFileManager::Get().DeleteDirectory(*OldTextureDirectory, false, true);
    }
    OutMoved = true;
    return true;
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
      if (IsWidgetNameFree(AssetName))
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
        AssetTools.DuplicateAsset(AssetName, Folder / AssetName, InSource));
    if (!Copy)
    {
      OutError = FText::Format(
          LOCTEXT("DupFailed", "DuplicateAsset refused to create {0}/{1}."),
          FText::FromString(Folder / AssetName), FText::FromString(AssetName));
      return nullptr;
    }

    if (!SaveWidgetBlueprint(Copy, OutError))
    {
      return nullptr;
    }
    return Copy;
  }

  bool CheckNewWidgetName(const FString &InName, FText &OutError)
  {
    FText Reason;
    if (InName.IsEmpty() ||
        !FName::IsValidXName(InName,
                             INVALID_OBJECTNAME_CHARACTERS
                             INVALID_LONGPACKAGE_CHARACTERS,
                             &Reason))
    {
      OutError = Reason.IsEmpty()
                     ? LOCTEXT("NewNameEmpty", "The name cannot be empty.")
                     : Reason;
      return false;
    }
    if (bMounted && !IsWidgetNameFree(InName))
    {
      OutError = FText::Format(LOCTEXT("NewNameTaken", "{0} already exists."),
                               FText::FromString(InName));
      return false;
    }
    return true;
  }

  UWidgetBlueprint *CreateEmptyWidgetBlueprint(FText &OutError,
                                               const FString &InName)
  {
    if (!bMounted)
    {
      OutError = LOCTEXT("NewNoMount",
                         "The generated-blueprint folder is not mounted.");
      return nullptr;
    }

    const FString Base = TEXT("WBP_NewWidget");
    const FString Folder = GetMountRootNoSlash();

    FString AssetName;
    if (!InName.IsEmpty())
    {
      if (!CheckNewWidgetName(InName, OutError))
      {
        return nullptr;
      }
      AssetName = InName;
    }
    for (int32 Attempt = 1; AssetName.IsEmpty() && Attempt < 1000; ++Attempt)
    {
      AssetName = Attempt == 1 ? Base
                               : FString::Printf(TEXT("%s%d"), *Base, Attempt);
      if (IsWidgetNameFree(AssetName))
      {
        break;
      }
      AssetName.Reset();
    }
    if (AssetName.IsEmpty())
    {
      OutError = LOCTEXT("NewNoName", "Could not find a free widget name.");
      return nullptr;
    }

    UPackage *Package = CreatePackage(*(Folder / AssetName / AssetName));
    UWidgetBlueprint *Blueprint = FWidgetBlueprintOperationUtils::CreateWidgetBlueprint(
        Package, FName(*AssetName), BPTYPE_Normal, UUserWidget::StaticClass());
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
      OutError = FText::Format(
          LOCTEXT("NewFailed", "Could not create {0}/{1}."),
          FText::FromString(Folder), FText::FromString(AssetName));
      return nullptr;
    }

    if (!SaveWidgetBlueprint(Blueprint, OutError))
    {
      return nullptr;
    }
    return Blueprint;
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
    const bool bOwnFolder = IsInOwnFolder(InBlueprint);
    const FString NewFolder =
        bOwnFolder ? FPackageName::GetLongPackagePath(Folder) / InNewName
                   : Folder;
    if (FPackageName::DoesPackageExist(NewFolder / InNewName) ||
        (bOwnFolder &&
         IFileManager::Get().DirectoryExists(*GetFolderOnDisk(NewFolder))))
    {
      OutError = FText::Format(
          LOCTEXT("RenameTaken", "{0} already exists."),
          FText::FromString(bOwnFolder ? NewFolder : NewFolder / InNewName));
      return false;
    }

    CloseEditors(InBlueprint);

    TArray<FAssetRenameData> Renames;
    TArray<FString> OldObjectPaths;
    Renames.Emplace(InBlueprint, NewFolder, InNewName);
    OldObjectPaths.Add(InBlueprint->GetPathName());
    if (bOwnFolder)
    {
      // The rest of the folder moves along, sub-folders kept.
      TArray<FAssetData> Assets;
      IAssetRegistry::GetChecked().GetAssetsByPath(FName(*Folder), Assets,
                                                   true);
      for (const FAssetData &Asset : Assets)
      {
        if (Asset.PackageName == FName(*OldPackageName) || Asset.IsRedirector())
        {
          continue;
        }
        if (UObject *Object = Asset.GetAsset())
        {
          Renames.Emplace(Object,
                          NewFolder +
                              Asset.PackagePath.ToString().Mid(Folder.Len()),
                          Object->GetName());
          OldObjectPaths.Add(Object->GetPathName());
        }
      }
    }

    const FString OldDirectory = GetFolderOnDisk(Folder);
    const FString NewDirectory = GetFolderOnDisk(NewFolder);
    if (!RenameAssetsWithoutEntryPrompt(Renames))
    {
      OutError = FText::Format(
          LOCTEXT("RenameFailed", "Could not rename {0} to {1}."),
          FText::FromString(OldPackageName), FText::FromString(InNewName));
      return false;
    }
    DeleteMountRedirectors(OldObjectPaths);

    if (bOwnFolder && !OldDirectory.IsEmpty() && !NewDirectory.IsEmpty())
    {
      MoveLooseFiles(OldDirectory, NewDirectory);
      // A Content/ rename leaves redirectors, and so the folder, behind.
      if (!HasPackageFiles(OldDirectory))
      {
        IFileManager::Get().DeleteDirectory(*OldDirectory, false, true);
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
    const bool bOwnFolder = IsInOwnFolder(Blueprint);
    const FString Folder = GetFolder(Blueprint);
    CloseEditors(Blueprint);
    if (ObjectTools::DeleteObjects({Blueprint}, false) == 0)
    {
      OutError = FText::Format(
          LOCTEXT("DeleteFailed", "Could not delete {0}."),
          FText::FromString(PackageName));
      return false;
    }
    if (bOwnFolder)
    {
      DeleteWidgetFolder(Folder, FName(*PackageName));
    }
    return true;
  }

}

#undef LOCTEXT_NAMESPACE
