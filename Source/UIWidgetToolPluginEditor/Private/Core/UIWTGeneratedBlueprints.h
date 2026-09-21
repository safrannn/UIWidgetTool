#pragma once

#include "Blueprint/UserWidget.h"
#include "CoreMinimal.h"

class UWidgetBlueprint;

// Widget blueprint copies the LLM edits. They live under
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

  // Copies Source into the mount as <Source>_Copy, _Copy2, ... (an existing
  // _CopyN suffix on Source is stripped first) and saves it. Null with
  // OutError set on failure.
  UWidgetBlueprint *DuplicateWidgetBlueprint(UWidgetBlueprint *InSource,
                                             FText &OutError);

  // Saves the blueprint's package. Refuses blueprints outside the mount and
  // blueprints whose last compile failed, so a copy on disk is always in a
  // compilable state.
  bool SaveWidgetBlueprint(UWidgetBlueprint *InBlueprint, FText &OutError);

  // Discards in-memory edits by reloading the package from disk.
  bool ReloadWidgetBlueprint(UWidgetBlueprint *InBlueprint, FText &OutError);

  // Renames the blueprint's asset, and so its package file, to InNewName in
  // its own folder - a copy in the mount or a hand-added one under Content/.
  // Refuses invalid names and names already taken there. Afterwards
  // InBlueprint's class path is the new one; a Content/ rename leaves a
  // redirector behind for unloaded referencers, a mount rename does not.
  bool RenameWidgetBlueprint(UWidgetBlueprint *InBlueprint,
                             const FString &InNewName, FText &OutError);

  // Deletes a copy's asset. Refuses anything outside the mount.
  bool DeleteWidgetBlueprint(const TSoftClassPtr<UUserWidget> &InClass,
                             FText &OutError);

}
