#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"

struct FUIWTPromptImage;

enum class EUIWTChatRole : uint8
{
  User,
  Assistant,
  // A failed or cancelled run; rendered like an assistant reply, in red.
  Error,
  // Transient "working..." line; replaced by the reply when the run ends.
  Status
};

struct FUIWTChatMessage
{
  EUIWTChatRole Role = EUIWTChatRole::User;
  FString Text;
  // User messages only: the image sent with the prompt, if any.
  TSharedPtr<const FUIWTPromptImage> Image;
};

// One entry's conversation. In-memory only: Claude Code sessions are keyed to
// this machine, and the settings ini is project-shared.
struct FUIWTChatState
{
  FString SessionId;
  TArray<FUIWTChatMessage> Messages;
  // Why the session's last turn was rolled back (cancelled, timed out,
  // failed); empty after a clean turn. The session still remembers that
  // turn's edits, so the next resumed prompt says they were discarded.
  FString RolledBackReason;
  // The widget folder on disk the last run wrote its files to.
  FString RunDirectory;
};

// What a run was started against. Tools read this, never the current
// selection, so clicking another entry mid-run cannot redirect Claude.
struct FUIWTActiveRun
{
  FGuid EntryId;
  // The entry's blueprint being edited, e.g. /Game/UI/WBP_Foo.WBP_Foo
  FSoftObjectPath BlueprintPath;
  FString LevelPackagePath;
  FString CheckpointDisplay;
  FString PickedWidget;

  // The blueprint's own content folder, e.g. /Game/UI/WBP_Foo, where the
  // run's textures go.
  FString ContentFolder;
  // The same folder on disk, for the run's loose files: the reference
  // image, renders and zooms, and whatever Claude writes with its own tools.
  FString RunDirectory;
  // The full-resolution reference image in RunDirectory; empty when the
  // entry never had one.
  FString ReferenceImagePath;
  // The latest RenderWidgetBlueprint output; empty before the first render.
  FString LastRenderPath;
  // Numbers the files the image tools write, so none is overwritten.
  int32 FileCounter = 0;
  // Textures the run created or changed. They are saved with the blueprint
  // and restored or deleted when the run fails.
  TArray<FSoftObjectPath> Textures;

  // An AI pass after a design import: the blueprint's widgets by GUID when
  // it started, so the renames it makes can be written into the design
  // sidecar at the end.
  bool bDesignRefine = false;
  TMap<FGuid, FName> WidgetsBefore;
  // The pass covers the sidecar's changedNodes; they're cleared when it
  // succeeds.
  bool bRefineChangedParts = false;
  // For the usage record: what the pass was on, and its estimate.
  FString RefineSource;
  FString RefineScope;
  FString RefineModel;
  int32 RefineNodes = 0;
  int32 RefineRounds = 0;
  int64 EstimatedRead = 0;
  int64 EstimatedCached = 0;
  int64 EstimatedWritten = 0;
  double EstimatedDollars = -1.0;

  // An image-reading run (import-image.md, section 1): Claude reads an image
  // into a design tree. BlueprintPath stays empty until the first
  // WriteDesignTree imports it; later calls merge into it.
  bool bImageRead = false;
  // Saved/UIWidgetTool/Image/<name>_<crc>: reference.png, images/,
  // design.json. Also the run's folder.
  FString ImageCacheDir;
  FString ImageSourceFile;
  FString ImageCrc;
  // Where the first WriteDesignTree imports to (empty: the defaults).
  FString ImageTargetFolder;
  FString ImageBlueprintName;
  // Reference pixels per design pixel.
  double ImageScale = 1.0;
  bool bImageSave = true;
  // Crop pixel hash → its file under images/, so identical art is one file.
  TMap<FString, FString> ImageCrops;

  bool IsValid() const { return EntryId.IsValid(); }
};
