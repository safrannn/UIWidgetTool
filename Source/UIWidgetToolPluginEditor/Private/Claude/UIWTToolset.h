#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/AgentSkill.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "UIWTToolset.generated.h"

class UWidgetBlueprint;

/** What the current UI Widget Tool run is editing. */
USTRUCT(BlueprintType)
struct FUIWTToolContext
{
  GENERATED_BODY()

  /** Object path of the Widget Blueprint to edit. Pass this to UMGToolSet tools. */
  UPROPERTY(BlueprintReadOnly, Category = "UIWidgetTool")
  FString BlueprintPath;

  /** Manager entry id. */
  UPROPERTY(BlueprintReadOnly, Category = "UIWidgetTool")
  FString EntryId;

  /** Level package the entry plays on, if any. */
  UPROPERTY(BlueprintReadOnly, Category = "UIWidgetTool")
  FString Level;

  /** Checkpoint the entry restores, if any. */
  UPROPERTY(BlueprintReadOnly, Category = "UIWidgetTool")
  FString Checkpoint;

  /** Name of the widget the user picked in the snapshot viewer, if any. The request is about this widget unless the prompt says otherwise. */
  UPROPERTY(BlueprintReadOnly, Category = "UIWidgetTool")
  FString PickedWidget;

  /** The user's note on the entry. Read-only: only the user edits it. */
  UPROPERTY(BlueprintReadOnly, Category = "UIWidgetTool")
  FString Note;
};

/**
 * Glue tools for the UI Widget Tool manager. Every tool acts on the run the
 * user started from the manager window, never on the current selection, and
 * refuses anything outside that run's blueprint.
 */
UCLASS(BlueprintType, Hidden)
class UUIWTToolset : public UToolsetDefinition
{
  GENERATED_BODY()

public:
  /**
   * Returns what the current run is editing: the blueprint, level,
   * checkpoint, the widget the user picked, and the entry's note. Call this
   * first. Fails when no run is active.
   * @return The run context.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool")
  static FUIWTToolContext GetContext();

  /**
   * Saves the run's Widget Blueprint to disk. Call only after
   * CompileWidgetBlueprint succeeded; a blueprint with compile errors is
   * refused, so what is on disk is always compilable.
   * @param WidgetBlueprint The blueprint returned by GetContext.
   * @return True when saved.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool")
  static bool SaveWidgetBlueprint(UWidgetBlueprint *WidgetBlueprint);

  /**
   * Lists the editable properties of a widget or panel slot in the run's
   * blueprint as a JSON schema. Use it before SetWidgetProperties to learn
   * property names and types.
   * @param Object A UWidget or UPanelSlot from GetWidgetDescription / AddWidget.
   * @return JSON schema text.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool")
  static FString ListWidgetProperties(UObject *Object);

  /**
   * Reads property values from a widget or panel slot in the run's blueprint.
   * @param Object A UWidget or UPanelSlot from GetWidgetDescription / AddWidget.
   * @param PropertyNames The properties to read.
   * @return JSON object text of name to value.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool")
  static FString GetWidgetProperties(UObject *Object,
                                     const TArray<FName> &PropertyNames);

  /**
   * Sets property values on a widget or panel slot in the run's blueprint.
   * Layout (anchors, padding, alignment, size) lives on the widget's Slot,
   * not the widget. Unknown names fail and are reported; sibling properties
   * in the same call may already have been applied.
   * @param Object A UWidget or UPanelSlot from GetWidgetDescription / AddWidget.
   * @param PropertiesJson JSON object text of name to value.
   * @return True when every property was set.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool")
  static bool SetWidgetProperties(UObject *Object,
                                  const FString &PropertiesJson);
};

/** The procedure Claude follows for a UI Widget Tool run. Also prepended to the headless prompt that opens a session. */
UCLASS()
class UUIWTAgentSkill : public UAgentSkill
{
  GENERATED_BODY()

public:
  UUIWTAgentSkill();

  static FString GetInstructionsText();
};
