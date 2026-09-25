#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/AgentSkill.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "UIWTToolset.generated.h"

class UWidgetBlueprint;
enum class EUIWTPermissionMode : uint8;

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

  /** The blueprint's own folder on disk, TextureFolder's directory. The reference image, renders and zooms are written here as loose files; open them with the Read tool. */
  UPROPERTY(BlueprintReadOnly, Category = "UIWidgetTool")
  FString RunFolder;

  /** Absolute path of the full-resolution reference image (the image the user attached, now or in an earlier turn); empty when there is none. The image tools take its pixel coordinates. */
  UPROPERTY(BlueprintReadOnly, Category = "UIWidgetTool")
  FString ReferenceImage;

  /** Reference image width in pixels; 0 when there is none. */
  UPROPERTY(BlueprintReadOnly, Category = "UIWidgetTool")
  int32 ReferenceWidth = 0;

  /** Reference image height in pixels; 0 when there is none. */
  UPROPERTY(BlueprintReadOnly, Category = "UIWidgetTool")
  int32 ReferenceHeight = 0;

  /** The blueprint's own content folder, <dir>/<name>. The texture tools write here, and UIWTDevToolset works only on assets inside it. */
  UPROPERTY(BlueprintReadOnly, Category = "UIWidgetTool")
  FString TextureFolder;
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
   * Saves the run's Widget Blueprint to disk. Call after
   * CompileWidgetBlueprint succeeded; uncompiled edits are compiled first,
   * and a blueprint with compile errors is refused, so what is on disk is
   * always compilable.
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

  /**
   * Builds or rebuilds the run's whole widget tree from one JSON spec, then
   * compiles. The spec is {"root": node}; a node is {"class": "Button",
   * "name": "Btn_Play", "variable": true, "props": {widget properties},
   * "slot": {properties of the slot in its parent}, "children": [nodes]}.
   * "class" is a UMG class name (CanvasPanel, Border, Button, Image,
   * TextBlock, ...), a /Script/... class path or a /Game/... Widget
   * Blueprint. Property JSON is the same as SetWidgetProperties.
   * Widgets with the same name and class as a spec node are kept (graph
   * references, bindings and animations stay valid); others are created,
   * and widgets missing from the spec are deleted. Slots are recreated, so
   * give each widget's full slot properties. An invalid spec (unknown class,
   * key or property name, duplicate name, children on a non-panel, or
   * deleting a widget the graph, an animation or a BindWidget needs) is
   * rejected with every problem listed and nothing changed; so is a
   * blueprint with named-slot content. Applying again with a corrected spec
   * is always safe.
   * @param WidgetBlueprint The blueprint returned by GetContext.
   * @param SpecJson The spec as JSON text.
   * @return A summary: widgets created, kept and removed, and compile result.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool")
  static FString ApplyWidgetSpec(UWidgetBlueprint *WidgetBlueprint,
                                 const FString &SpecJson);

  /**
   * Returns the run's widget tree as a spec for ApplyWidgetSpec: every
   * widget with its class, name, variable flag, and the widget and slot
   * properties that differ from their class defaults. Edit it and apply it
   * to change the tree while keeping existing widgets. {} for an empty tree.
   * @param WidgetBlueprint The blueprint returned by GetContext.
   * @return The spec as JSON text.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool")
  static FString ExportWidgetSpec(UWidgetBlueprint *WidgetBlueprint);

  /**
   * Saves a magnified crop as a PNG in the run folder; open it with the Read
   * tool. Use it to measure positions, sizes and colours exactly and to
   * compare details with the latest render. Coordinates are reference-image
   * pixels, which are also the render's pixels when it was rendered at the
   * reference size.
   * @param Source "Reference" (the reference image), "Render" (the latest
   *   RenderWidgetBlueprint output) or "Both" (reference left, render right).
   * @param X Left edge of the crop.
   * @param Y Top edge of the crop.
   * @param Width Crop width.
   * @param Height Crop height.
   * @param Scale Magnification from 1 to 16, nearest neighbour; lowered when
   *   the result would be wider or taller than 1568 pixels.
   * @return The PNG's path and the scale used: a pixel at (px, py) in it is
   *   at (X + px / scale, Y + py / scale).
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool")
  static FString ZoomImage(const FString &Source, int32 X, int32 Y,
                           int32 Width, int32 Height, int32 Scale);

  /**
   * Cuts a region of the reference image into a UI texture asset in the
   * run's TextureFolder, for art brushes cannot draw: icons, portraits, item
   * art, illustrated frames and ornaments, textured backgrounds. Use the
   * returned object path as a brush's "resourceObject". Calling again with
   * the same name replaces the pixels. Crop tightly and leave out text and
   * anything that will be a separate widget.
   * @param TextureName Asset name, for example T_Icon_Fire.
   * @param X Left edge in reference-image pixels.
   * @param Y Top edge in reference-image pixels.
   * @param Width Crop width in reference-image pixels.
   * @param Height Crop height in reference-image pixels.
   * @param Mode "Copy" keeps the pixels as they are (opaque art, backgrounds);
   *   "KeyDark" makes the crop's dark background transparent and keeps the
   *   colours (a glowing icon on a dark panel); "Mask" does the same but
   *   makes the art white, to be coloured with the brush tint (flat icons and
   *   glyphs that appear in several colours).
   * @param Shape "Rect", or "Circle" to cut an ellipse filling the output
   *   (round portraits, orbs, round buttons).
   * @param Threshold KeyDark and Mask only: the background is taken from
   *   the crop's border pixels, so crop with a little background all
   *   round; this many brightness levels (0-255) above it still count as
   *   background. 0 means the default, 12; raise it when the background
   *   still shows through, lower it when faint parts of the art vanish.
   * @param OutputWidth Texture width; 0 uses the crop width. Use 2x the
   *   crop size for art that will be shown larger.
   * @param OutputHeight Texture height; 0 uses the crop height.
   * @return The texture's object path.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool")
  static FString CreateTextureFromReference(const FString &TextureName,
                                            int32 X, int32 Y, int32 Width,
                                            int32 Height, const FString &Mode,
                                            const FString &Shape,
                                            int32 Threshold, int32 OutputWidth,
                                            int32 OutputHeight);

  /**
   * Imports a PNG, JPEG or BMP file from the run folder as a UI texture in
   * the run's TextureFolder, for images made there with your own tools
   * (gradients, tiling noise, composited art). Calling again with the same
   * name replaces the pixels.
   * @param FilePath Absolute path of a file inside the run folder.
   * @param TextureName Asset name, for example T_Leather.
   * @return The texture's object path.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool")
  static FString ImportTextureFile(const FString &FilePath,
                                   const FString &TextureName);

  /**
   * Renders the run's blueprint as the screen would show it, compiling it
   * first if needed, and saves the PNG in the run folder. With a reference
   * image, also saves the two side by side (reference left, render right)
   * and reports how different they are. Open the PNGs with the Read tool
   * and use ZoomImage with Source "Both" for details.
   * @param WidgetBlueprint The blueprint returned by GetContext.
   * @param Width Render width; 0 uses the reference width, or 1920 without
   *   a reference.
   * @param Height Render height; 0 uses the reference height, or 1080.
   * @return The paths written and the mean colour difference in percent.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool")
  static FString RenderWidgetBlueprint(UWidgetBlueprint *WidgetBlueprint,
                                       int32 Width, int32 Height);
};

/** The procedure Claude follows for a UI Widget Tool run. Also prepended to the headless prompt that opens a session. */
UCLASS()
class UUIWTAgentSkill : public UAgentSkill
{
  GENERATED_BODY()

public:
  UUIWTAgentSkill();

  static FString GetInstructionsText();

  // Appended to every request, since the setting can change between turns
  // of one session: what the run's built-in tools may do.
  static FString GetAccessText(EUIWTPermissionMode InMode,
                               const FString &InRunFolder);
};
