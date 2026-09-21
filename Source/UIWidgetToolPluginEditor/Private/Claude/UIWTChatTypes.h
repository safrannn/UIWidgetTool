#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"

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
  // The copy being edited, e.g. /UIWidgetToolGenerated/WBP_Foo_Copy.WBP_Foo_Copy
  FSoftObjectPath BlueprintPath;
  // The root original's blueprint, for reference only.
  FSoftObjectPath OriginalBlueprintPath;
  FString LevelPackagePath;
  FString CheckpointDisplay;
  FString PickedWidget;

  bool IsValid() const { return EntryId.IsValid(); }
};
