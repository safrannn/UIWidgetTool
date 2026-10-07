#pragma once

#include "CoreMinimal.h"

// Whether Claude Code is installed, checked once at module startup and again
// after an install from the settings page.
namespace UIWTClaudeInstall
{
  enum class EState : uint8
  {
    NotInstalled,
    Installing,
    Installed
  };

  // Looks for the executable the same way Connect does.
  void Check();

  EState GetState();

  // Runs the official installer on a background thread, then checks again.
  // Does nothing unless the state is NotInstalled.
  void Install();
}
