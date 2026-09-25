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

  bool IsValid() const { return EntryId.IsValid(); }
};
