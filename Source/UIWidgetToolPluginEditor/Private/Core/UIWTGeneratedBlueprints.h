#pragma once

#include "Blueprint/UserWidget.h"
#include "CoreMinimal.h"

class UWidgetBlueprint;

// Widget blueprint copies made by the manager's Duplicate, and the empty
// blueprints the chat makes for entries without one. They live under
// <Saved>/UIWidgetTool/WidgetBlueprints/, mounted as /UIWidgetToolGenerated/
// so they are real packages: DuplicateAsset can target them, TSoftClassPtr
// resolves them, Play can CreateWidget from them, and the asset registry
// indexes them. Saved/ is per-developer scratch; nothing under Content/ may
// reference a copy.
namespace UIWTGenerated
{

  // Absolute directory the mount points at.
  FString GetDirectory();

  void RegisterMountPoint();
  void UnregisterMountPoint();

  // True for a long package name or object path under the mount.
  bool IsGeneratedPath(const FString &InPath);

  // Resolves an entry's class to its blueprint, loading if needed.
  UWidgetBlueprint *
  FindWidgetBlueprint(const TSoftClassPtr<UUserWidget> &InClass);

  // Every widget the tool makes or edits lives in its own folder,
  // <dir>/<name>/<name>, together with what runs generate for it: textures
  // as assets, and the reference image, renders and zooms as loose files.

  // The content folder holding the blueprint's package, e.g. /Game/UI/WBP_Foo.
  FString GetFolder(const UWidgetBlueprint *InBlueprint);
  // Absolute directory on disk of a content folder, without a trailing slash.
  FString GetFolderOnDisk(const FString &InFolder);
  // True when the blueprint's folder is named after it.
  bool IsInOwnFolder(const UWidgetBlueprint *InBlueprint);
  // Moves the blueprint from <dir>/<name> to <dir>/<name>/<name>, along with
  // textures an earlier run left in <dir>/<name>_Textures. The rename saves
  // the moved packages; a Content/ move leaves redirectors behind, a mount
  // move does not. OutMoved is false when it was already there.
  bool MoveToOwnFolder(UWidgetBlueprint *InBlueprint, bool &OutMoved,
                       FText &OutError);

  // Copies Source into the mount as <Source>_Copy, _Copy2, ... (an existing
  // _CopyN suffix on Source is stripped first), in its own folder, and saves
  // it. The copy still uses Source's textures. Null with OutError set on
  // failure.
  UWidgetBlueprint *DuplicateWidgetBlueprint(UWidgetBlueprint *InSource,
                                             FText &OutError);

  // Whether InName can name a new blueprint in the mount: a valid asset name
  // not already taken there. OutError says why not.
  bool CheckNewWidgetName(const FString &InName, FText &OutError);

  // Creates a UserWidget blueprint with an empty tree in the mount, in its
  // own folder, and saves it. It is named InName when given, which must pass
  // CheckNewWidgetName; otherwise WBP_NewWidget, WBP_NewWidget2, ... Null
  // with OutError set on failure.
  UWidgetBlueprint *CreateEmptyWidgetBlueprint(FText &OutError,
                                               const FString &InName = FString());

  // Saves the blueprint's package, in the mount or under Content/. Refuses
  // blueprints whose last compile failed, so what is on disk is always in a
  // compilable state.
  bool SaveWidgetBlueprint(UWidgetBlueprint *InBlueprint, FText &OutError);

  // Discards in-memory edits by reloading the package from disk.
  bool ReloadWidgetBlueprint(UWidgetBlueprint *InBlueprint, FText &OutError);

  // Renames the blueprint's asset, and so its package file, to InNewName - a
  // copy in the mount or a hand-added one under Content/. A blueprint in its
  // own folder takes the folder along: every asset and loose file in it
  // moves to <dir>/<InNewName>/. Refuses invalid names and names already
  // taken there. Afterwards InBlueprint's class path is the new one; a
  // Content/ rename leaves redirectors behind for unloaded referencers, a
  // mount rename does not.
  bool RenameWidgetBlueprint(UWidgetBlueprint *InBlueprint,
                             const FString &InNewName, FText &OutError);

  // Deletes a copy's or an empty blueprint's asset. Refuses anything outside
  // the mount. When the blueprint had its own folder, the folder's textures
  // go too, except any another asset still uses, and so does the folder on
  // disk once no asset is left in it.
  bool DeleteWidgetBlueprint(const TSoftClassPtr<UUserWidget> &InClass,
                             FText &OutError);

}
