#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "UIWTDevToolset.generated.h"

class UWidgetBlueprint;

/**
 * General asset tools for a UI Widget Tool run: read and write any
 * property, import image files, save, render. They act only on assets inside
 * the run blueprint's own folder (GetContext's TextureFolder) and write
 * files only inside its RunFolder. Fails when no run is active.
 */
UCLASS(BlueprintType, Hidden)
class UUIWTDevToolset : public UToolsetDefinition
{
  GENERATED_BODY()

public:
  /**
   * Lists the editable properties of an object in the run's folder as a
   * JSON schema.
   * @param Object The blueprint, one of its widgets or slots, or a texture
   *   in TextureFolder.
   * @return JSON schema text.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Dev")
  static FString ListObjectProperties(UObject *Object);

  /**
   * Reads property values from an object in the run's folder.
   * @param Object The blueprint, one of its widgets or slots, or a texture
   *   in TextureFolder.
   * @param PropertyNames The properties to read.
   * @return JSON object text of name to value.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Dev")
  static FString GetObjectProperties(UObject *Object,
                                     const TArray<FName> &PropertyNames);

  /**
   * Sets property values on an object in the run's folder, as the Details
   * panel would. Not saved until SaveAsset or UIWTToolset.SaveWidgetBlueprint.
   * @param Object The blueprint, one of its widgets or slots, or a texture
   *   in TextureFolder.
   * @param PropertiesJson JSON object text of name to value.
   * @return True when every property was set.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Dev")
  static bool SetObjectProperties(UObject *Object,
                                  const FString &PropertiesJson);

  /**
   * Imports a PNG, JPEG or BMP file as a UI texture (uncompressed, sRGB, no
   * mips) in a folder inside TextureFolder. Calling again with the same name
   * replaces the pixels. Saved with the blueprint.
   * @param SourceFile Absolute path of the image file, anywhere on disk.
   * @param DestinationPath TextureFolder, or a sub-folder of it.
   * @param AssetName Name of the texture asset, for example T_Background.
   * @return The texture's object path.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Dev")
  static FString ImportTexture(const FString &SourceFile,
                               const FString &DestinationPath,
                               const FString &AssetName);

  /**
   * Saves an asset in the run's folder to disk. Blueprints with compile
   * errors are refused.
   * @param Asset The asset to save.
   * @return True when saved.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Dev")
  static bool SaveAsset(UObject *Asset);

  /**
   * Renders the run's blueprint at any size and writes it as a PNG, for
   * checking how it lays out at other resolutions. Compiles first if
   * needed.
   * @param WidgetBlueprint The blueprint returned by GetContext.
   * @param Width Render width in pixels.
   * @param Height Render height in pixels.
   * @param OutFile Absolute path of the PNG to write, inside RunFolder.
   * @return True when the PNG was written.
   */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Dev")
  static bool RenderWidgetToPng(UWidgetBlueprint *WidgetBlueprint, int32 Width,
                                int32 Height, const FString &OutFile);
};

/**
 * Test hooks for developing the plugin: apply and export specs outside a
 * run, and start or fake runs over MCP. Registered only by the
 * UIWT.RegisterDevTools console command, never by Connect MCP. Spec hooks
 * act only on blueprints under /Game/UI.
 */
UCLASS(BlueprintType, Hidden)
class UUIWTTestHooksToolset : public UToolsetDefinition
{
  GENERATED_BODY()

public:
  /** UIWTWidgetSpec::Apply on a /Game/UI blueprint, reported like UIWTToolset.ApplyWidgetSpec. */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Test Hooks")
  static FString DevApplyWidgetSpec(UWidgetBlueprint *WidgetBlueprint,
                                    const FString &SpecJson);

  /** UIWTWidgetSpec::Export on a /Game/UI blueprint. */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Test Hooks")
  static FString DevExportWidgetSpec(UWidgetBlueprint *WidgetBlueprint);

  /** Starts a run with no Claude process on the blueprint (moved into its own folder first), with an optional image file as the attachment. */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Test Hooks")
  static bool DevBeginRun(UWidgetBlueprint *WidgetBlueprint,
                          const FString &EntryId, const FString &ImageFile);

  /** Ends the run DevBeginRun started, as a success or a failure. */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Test Hooks")
  static bool DevEndRun(bool bSuccess);

  /** A real run through FUIWTClaudeService::StartRun, as the chat panel starts one. Connects MCP first. */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Test Hooks")
  static FString DevStartRun(const FString &EntryId, const FString &Prompt,
                             const FString &ImageFile);

  /** Whether a run is in flight, and the entry's chat messages. */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Test Hooks")
  static FString DevRunStatus(const FString &EntryId);

  /** UIWTGenerated::RenameWidgetBlueprint on a generated or /Game/UI blueprint. Entries are not updated. */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Test Hooks")
  static bool DevRenameWidgetBlueprint(UWidgetBlueprint *WidgetBlueprint,
                                       const FString &NewName);

  /** UIWTGenerated::DuplicateWidgetBlueprint; returns the copy's object path. */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Test Hooks")
  static FString DevDuplicateWidgetBlueprint(UWidgetBlueprint *WidgetBlueprint);

  /** UIWTGenerated::DeleteWidgetBlueprint on a generated blueprint. */
  UFUNCTION(meta = (AICallable), Category = "UI Widget Tool Test Hooks")
  static bool DevDeleteWidgetBlueprint(UWidgetBlueprint *WidgetBlueprint);
};
