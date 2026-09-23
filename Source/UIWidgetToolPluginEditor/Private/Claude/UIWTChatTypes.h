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

  bool IsValid() const { return EntryId.IsValid(); }
};
