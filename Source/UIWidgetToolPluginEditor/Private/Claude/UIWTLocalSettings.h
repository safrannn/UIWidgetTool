#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"

#include "UIWTLocalSettings.generated.h"

// Passed to claude as --model; the families pick their latest model.
UENUM()
enum class EUIWTClaudeModel : uint8
{
  Default UMETA(DisplayName = "Default (Claude Code's own choice)"),
  Fable UMETA(DisplayName = "Fable (latest)"),
  Opus UMETA(DisplayName = "Opus (latest)"),
  Sonnet UMETA(DisplayName = "Sonnet (latest)"),
  Haiku UMETA(DisplayName = "Haiku (latest)"),
  Custom UMETA(DisplayName = "Custom model name")
};

// Passed to claude as --effort.
UENUM()
enum class EUIWTClaudeEffort : uint8
{
  Default UMETA(DisplayName = "Default (Claude Code's own choice)"),
  Low,
  Medium,
  High,
  ExtraHigh UMETA(DisplayName = "Extra High"),
  Max
};

// Passed to claude as --permission-mode. Every built-in tool is available in
// each mode; the mode decides which calls are allowed.
UENUM()
enum class EUIWTPermissionMode : uint8
{
  Auto UMETA(DisplayName = "Auto (a safety check refuses risky actions)"),
  BypassPermissions UMETA(DisplayName = "Bypass Permissions (no checks)"),
  DontAsk UMETA(DisplayName = "Don't Ask (editor tools and reading only)")
};

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

  // The model runs use. Applies from the next message.
  UPROPERTY(config, EditAnywhere, Category = "Claude Code")
  EUIWTClaudeModel Model = EUIWTClaudeModel::Default;

  // A model name or alias claude accepts, for example claude-opus-5-5.
  UPROPERTY(config, EditAnywhere, Category = "Claude Code",
            meta = (EditCondition = "Model == EUIWTClaudeModel::Custom",
                    EditConditionHides))
  FString CustomModel;

  // How much the model thinks before acting. Higher is slower and better
  // on hard layouts. Applies from the next message.
  UPROPERTY(config, EditAnywhere, Category = "Claude Code")
  EUIWTClaudeEffort Effort = EUIWTClaudeEffort::Default;

  // Runs always have every built-in tool (shell, files, web) besides the
  // editor's. Auto lets a safety check refuse risky actions; Bypass
  // Permissions allows everything without asking; Don't Ask refuses every
  // call except the editor tools and reading project files.
  UPROPERTY(config, EditAnywhere, Category = "Claude Code")
  EUIWTPermissionMode PermissionMode = EUIWTPermissionMode::Auto;

  // A run that has not finished by then is terminated and its edits
  // discarded. 0 means no time limit.
  UPROPERTY(config, EditAnywhere, Category = "Claude Code",
            meta = (ClampMin = "0", Units = "s"))
  int32 RunTimeoutSeconds = 1200;

  // Port the editor's MCP server listens on while connected.
  UPROPERTY(config, EditAnywhere, Category = "MCP Server",
            meta = (ClampMin = "1024", ClampMax = "65535"))
  int32 McpServerPort = 8000;

  // --model's value; empty for Claude Code's own choice.
  FString GetModelArgument() const
  {
    switch (Model)
    {
    case EUIWTClaudeModel::Fable:
      return TEXT("fable");
    case EUIWTClaudeModel::Opus:
      return TEXT("opus");
    case EUIWTClaudeModel::Sonnet:
      return TEXT("sonnet");
    case EUIWTClaudeModel::Haiku:
      return TEXT("haiku");
    case EUIWTClaudeModel::Custom:
      return CustomModel.TrimStartAndEnd();
    default:
      return FString();
    }
  }

  // --effort's value; empty for Claude Code's own choice.
  FString GetEffortArgument() const
  {
    switch (Effort)
    {
    case EUIWTClaudeEffort::Low:
      return TEXT("low");
    case EUIWTClaudeEffort::Medium:
      return TEXT("medium");
    case EUIWTClaudeEffort::High:
      return TEXT("high");
    case EUIWTClaudeEffort::ExtraHigh:
      return TEXT("xhigh");
    case EUIWTClaudeEffort::Max:
      return TEXT("max");
    default:
      return FString();
    }
  }

  // --permission-mode's value.
  FString GetPermissionModeArgument() const
  {
    switch (PermissionMode)
    {
    case EUIWTPermissionMode::BypassPermissions:
      return TEXT("bypassPermissions");
    case EUIWTPermissionMode::DontAsk:
      return TEXT("dontAsk");
    default:
      return TEXT("auto");
    }
  }
};
