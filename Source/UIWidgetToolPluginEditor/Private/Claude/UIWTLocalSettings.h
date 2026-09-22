#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"

#include "UIWTLocalSettings.generated.h"

// Per-developer settings for the Claude Code integration.
UCLASS(config = EditorPerProjectUserSettings,
       meta = (DisplayName = "UI Widget Tool (local)"))
class UUIWTLocalSettings : public UDeveloperSettings
{
  GENERATED_BODY()

public:
  virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

  static UUIWTLocalSettings *Get()
  {
    return GetMutableDefault<UUIWTLocalSettings>();
  }

  // Full path to the Claude Code executable (claude.exe or claude.cmd).
  // Empty means "search PATH at Connect time".
  UPROPERTY(config, EditAnywhere, Category = "Claude Code")
  FFilePath ClaudeExecutable;

  // A run that has not finished by then is terminated and its edits
  // discarded.
  UPROPERTY(config, EditAnywhere, Category = "Claude Code",
            meta = (ClampMin = "30"))
  int32 RunTimeoutSeconds = 600;

  // Port the editor's MCP server listens on while connected.
  UPROPERTY(config, EditAnywhere, Category = "MCP Server",
            meta = (ClampMin = "1024", ClampMax = "65535"))
  int32 McpServerPort = 8000;
};
