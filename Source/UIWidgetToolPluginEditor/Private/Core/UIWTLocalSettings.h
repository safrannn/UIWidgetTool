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

// Per-developer settings: the Claude Code integration, the MCP server and
// the Figma import. Stored in Saved/, never committed.
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

  // A Figma personal access token (Figma → Settings → Security), used when
  // the FIGMA_TOKEN environment variable isn't set. Stored as plain text in
  // Saved/; a Claude run with Bypass Permissions can read it, so prefer the
  // environment variable.
  UPROPERTY(config, EditAnywhere, Category = "Figma",
            meta = (PasswordField = true))
  FString FigmaToken;

  // The model the AI pass after a design import uses (the import menu's
  // "AI pass" toggle), apart from the chat's. Sonnet costs about half as
  // much as Opus, and the pass is mostly structural edits checked by renders.
  UPROPERTY(config, EditAnywhere, Category = "AI Pass (design imports)")
  EUIWTClaudeModel RefineModel = EUIWTClaudeModel::Sonnet;

  UPROPERTY(config, EditAnywhere, Category = "AI Pass (design imports)",
            meta = (EditCondition = "RefineModel == EUIWTClaudeModel::Custom",
                    EditConditionHides))
  FString RefineCustomModel;

  UPROPERTY(config, EditAnywhere, Category = "AI Pass (design imports)")
  EUIWTClaudeEffort RefineEffort = EUIWTClaudeEffort::Default;

  // The most apply → render → compare rounds the pass makes.
  UPROPERTY(config, EditAnywhere, Category = "AI Pass (design imports)",
            meta = (ClampMin = "1", ClampMax = "10"))
  int32 RefineRounds = 3;

  // The import menu's toggles: refine Figma / Photoshop imports with Claude.
  // Off by default: an import alone uses no tokens.
  UPROPERTY(config)
  bool bRefineFigmaImports = false;

  UPROPERTY(config)
  bool bRefinePsdImports = false;

  // An image import is a Claude run either way; this adds the AI pass's
  // work to it.
  UPROPERTY(config)
  bool bRefineImageImports = false;

  // --model's value; empty for Claude Code's own choice.
  FString GetModelArgument() const { return ModelArgument(Model, CustomModel); }

  // --effort's value; empty for Claude Code's own choice.
  FString GetEffortArgument() const { return EffortArgument(Effort); }

  // The same for the AI pass.
  FString GetRefineModelArgument() const
  {
    return ModelArgument(RefineModel, RefineCustomModel);
  }

  FString GetRefineEffortArgument() const { return EffortArgument(RefineEffort); }

  static FString ModelArgument(EUIWTClaudeModel InModel, const FString &InCustom)
  {
    switch (InModel)
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
      return InCustom.TrimStartAndEnd();
    default:
      return FString();
    }
  }

  static FString EffortArgument(EUIWTClaudeEffort InEffort)
  {
    switch (InEffort)
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
