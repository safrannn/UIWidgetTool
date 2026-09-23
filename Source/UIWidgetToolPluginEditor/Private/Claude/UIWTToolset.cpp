#include "UIWTToolset.h"

#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/Widget.h"
#include "Kismet/KismetSystemLibrary.h"
#include "ToolsetRegistry/ToolsetLibrary.h"
#include "UIWTChatTypes.h"
#include "UIWTClaudeService.h"
#include "Core/UIWTGeneratedBlueprints.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "WidgetBlueprint.h"

namespace
{
  // Null when no run is active; the script error is already raised.
  const FUIWTActiveRun *RequireActiveRun()
  {
    const FUIWTActiveRun *Run =
        FUIWTClaudeService::Get().GetActiveRun();
    if (!Run || !Run->IsValid())
    {
      UKismetSystemLibrary::RaiseScriptError(
          TEXT("No UI Widget Tool run is active. Tools only work while the "
               "manager window has a run in flight."));
      return nullptr;
    }
    return Run;
  }

  UWidgetBlueprint *ResolveRunBlueprint(const FUIWTActiveRun &Run)
  {
    UWidgetBlueprint *Blueprint =
        Cast<UWidgetBlueprint>(Run.BlueprintPath.TryLoad());
    if (!Blueprint)
    {
      UKismetSystemLibrary::RaiseScriptError(FString::Printf(
          TEXT("The run's blueprint %s could not be loaded."),
          *Run.BlueprintPath.ToString()));
    }
    return Blueprint;
  }

  // The object must belong to the run's blueprint: a widget in its
  // tree, or a slot on one of those widgets.
  bool ObjectBelongsToRun(UObject *Object, const FUIWTActiveRun &Run)
  {
    if (!Object)
    {
      UKismetSystemLibrary::RaiseScriptError(TEXT("Object is null."));
      return false;
    }
    UWidgetBlueprint *Blueprint = ResolveRunBlueprint(Run);
    if (!Blueprint)
    {
      return false;
    }
    if (Object->IsA<UWidget>() || Object->IsA<UPanelSlot>())
    {
      if (Blueprint->WidgetTree && Object->IsIn(Blueprint->WidgetTree))
      {
        return true;
      }
    }
    UKismetSystemLibrary::RaiseScriptError(FString::Printf(
        TEXT("%s is not a widget or slot in the run's blueprint %s. Only the "
             "blueprint returned by GetContext may be edited."),
        *Object->GetPathName(), *Run.BlueprintPath.ToString()));
    return false;
  }
}

FUIWTToolContext UUIWTToolset::GetContext()
{
  FUIWTToolContext Context;
  const FUIWTActiveRun *Run = RequireActiveRun();
  if (!Run)
  {
    return Context;
  }

  const FWidgetPreviewObject *Entry =
      UUIWidgetPreviewObjectManagerSettings::Get()->FindWidgetPreviewObject(
          Run->EntryId);
  if (!Entry)
  {
    UKismetSystemLibrary::RaiseScriptError(
        TEXT("The run's entry no longer exists."));
    return Context;
  }

  Context.BlueprintPath = Run->BlueprintPath.ToString();
  Context.EntryId = Run->EntryId.ToString();
  Context.Level = Run->LevelPackagePath;
  Context.Checkpoint = Run->CheckpointDisplay;
  Context.PickedWidget = Run->PickedWidget;
  Context.Note = Entry->Note;
  return Context;
}

bool UUIWTToolset::SaveWidgetBlueprint(UWidgetBlueprint *WidgetBlueprint)
{
  const FUIWTActiveRun *Run = RequireActiveRun();
  if (!Run)
  {
    return false;
  }
  if (!WidgetBlueprint ||
      FSoftObjectPath(WidgetBlueprint) != Run->BlueprintPath)
  {
    UKismetSystemLibrary::RaiseScriptError(FString::Printf(
        TEXT("Only the run's blueprint %s may be saved."),
        *Run->BlueprintPath.ToString()));
    return false;
  }
  FText Error;
  if (!UIWTGenerated::SaveWidgetBlueprint(WidgetBlueprint, Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error.ToString());
    return false;
  }
  return true;
}

FString UUIWTToolset::ListWidgetProperties(UObject *Object)
{
  const FUIWTActiveRun *Run = RequireActiveRun();
  if (!Run || !ObjectBelongsToRun(Object, *Run))
  {
    return FString();
  }
  return UToolsetLibrary::ListStructProperties(Object->GetClass(), true);
}

FString UUIWTToolset::GetWidgetProperties(UObject *Object,
                                          const TArray<FName> &PropertyNames)
{
  const FUIWTActiveRun *Run = RequireActiveRun();
  if (!Run || !ObjectBelongsToRun(Object, *Run))
  {
    return FString();
  }
  return UToolsetLibrary::GetObjectProperties(Object, PropertyNames);
}

bool UUIWTToolset::SetWidgetProperties(UObject *Object,
                                       const FString &PropertiesJson)
{
  const FUIWTActiveRun *Run = RequireActiveRun();
  if (!Run || !ObjectBelongsToRun(Object, *Run))
  {
    return false;
  }
  Object->Modify();
  if (UWidgetBlueprint *Blueprint = ResolveRunBlueprint(*Run))
  {
    Blueprint->Modify();
    Blueprint->MarkPackageDirty();
  }
  return UToolsetLibrary::SetObjectProperties(Object, PropertiesJson);
}

UUIWTAgentSkill::UUIWTAgentSkill()
{
  Description =
      TEXT("Edit the Widget Blueprint selected in the UI Widget Tool "
           "manager window, as the user asked in its chat panel.");
  Instructions = GetInstructionsText();
}

FString UUIWTAgentSkill::GetInstructionsText()
{
  return TEXT(
      "You are editing an Unreal Engine Widget Blueprint through the Unreal "
      "MCP server. Follow this procedure exactly. Tools are reached through "
      "the server's list_toolsets / describe_toolset / call_tool meta-tools; "
      "the toolset names below are suffixes of the registered names (for "
      "example UMGToolSet.UMGToolSet and UIWidgetToolPluginEditor."
      "UIWTToolset).\n"
      "\n"
      "1. Call UIWTToolset.GetContext first. It returns BlueprintPath (the "
      "blueprint you may edit), PickedWidget (the widget the user selected, "
      "if any), and EntryId.\n"
      "2. Call UMGToolSet.GetWidgetDescription on BlueprintPath to read the "
      "tree before changing anything. Address widgets by the [N] index / "
      "Widgets[N] references it returns; do not guess names.\n"
      "3. Make the change with UMGToolSet tools (AddWidget, MoveWidget, "
      "RemoveWidget, RenameWidget, WrapWidgets, SetNamedSlotContent, ...) "
      "and UIWTToolset.ListWidgetProperties / GetWidgetProperties / "
      "SetWidgetProperties for property values. UMGToolSet's own text "
      "mentions ObjectTools.list_properties / get_properties / "
      "set_properties - that toolset is NOT available here; the UIWTToolset "
      "equivalents above are the replacement and take the same widget or "
      "slot references. Layout (anchors, padding, "
      "alignment, size) lives on a widget's Slot, not the widget. Never "
      "remove or rename a widget that the native parent class binds "
      "(BindWidget); the compile will fail.\n"
      "4. Call UMGToolSet.CompileWidgetBlueprint. If it reports errors, fix "
      "them and compile again. If they cannot be fixed, do NOT save: report "
      "the errors and stop; the editor restores the blueprint from disk.\n"
      "5. After a clean compile, call UIWTToolset.SaveWidgetBlueprint on "
      "BlueprintPath.\n"
      "6. Reply with a short summary of what changed.\n"
      "\n"
      "Rules: edit only the blueprint GetContext returned; never create, "
      "duplicate or delete assets; never call AgentSkillToolset.CreateSkill "
      "or any tool outside UMGToolSet and UIWTToolset; never call "
      "UndoTransaction (it is a no-op here). If PickedWidget is set, the "
      "request is about that widget unless the prompt says otherwise.");
}
