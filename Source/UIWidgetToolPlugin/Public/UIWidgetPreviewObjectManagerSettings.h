#pragma once

#include "Blueprint/UserWidget.h"
#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"
#include "UIWTCheckpointCodec.h"

#include "UIWidgetPreviewObjectManagerSettings.generated.h"

USTRUCT()
struct UIWIDGETTOOLPLUGIN_API FWidgetPreviewObject {
  GENERATED_BODY()

  UPROPERTY()
  FGuid Id;

  UPROPERTY()
  FString WidgetName;

  UPROPERTY()
  TSoftClassPtr<UUserWidget> WidgetClass;

  UPROPERTY(config)
  FGuid CheckpointId;

  // The level this entry shows and plays in. None until chosen; the manager
  // then falls back to the checkpoint's map, else the first scanned level.
  UPROPERTY(config)
  FName LevelPackagePath;

  // The root original this row's blueprint was duplicated from. Always the
  // root, never an intermediate copy, so deleting a copy orphans nothing.
  // Invalid on hand-added rows, whose blueprint lives under Content/ and is
  // never edited by the tool.
  UPROPERTY()
  FGuid SourceEntryId;

  // One-line, LLM-written description of the copy's current state.
  UPROPERTY()
  FString Note;

  bool IsOriginal() const { return !SourceEntryId.IsValid(); }
};

UCLASS(config = EditorPerProjectUserSettings, defaultconfig)
class UIWIDGETTOOLPLUGIN_API UUIWidgetPreviewObjectManagerSettings
    : public UDeveloperSettings {
  GENERATED_BODY()

public:
  UUIWidgetPreviewObjectManagerSettings();

  UPROPERTY(config, EditAnywhere, Category = "UI Widget Tool")
  TArray<FWidgetPreviewObject> WidgetPreviewObjects;

  UPROPERTY(config, EditAnywhere, Category = "Checkpoints")
  FDirectoryPath CheckpointDirectory;

  UPROPERTY(config, EditAnywhere, Category = "Checkpoints",
            meta = (ClampMin = "0"))
  int32 MaxCheckpointsPerMap = 20;

  UPROPERTY(config, EditAnywhere, Category = "Checkpoints",
            meta = (ClampMin = "1"))
  int32 MaxCompressedPayloadMB = 64;

  UPROPERTY(config, EditAnywhere, Category = "Checkpoints",
            meta = (ClampMin = "1"))
  int32 MaxDecompressedPayloadMB = 512;

  UPROPERTY(config, EditAnywhere, Category = "Checkpoints",
            meta = (ClampMin = "1"))
  int32 MaxSnapshotFileMB = 256;

  UPROPERTY(config, EditAnywhere, Category = "Level Scan",
            meta = (ClampMin = "1", ClampMax = "8"))
  int32 MaxDepth = 3;

  UPROPERTY(config, EditAnywhere, Category = "Restore",
            meta = (ClampMin = "0.0"))
  float StreamingConvergenceTimeoutSeconds = 10.f;

  // Once a play has restored a checkpoint and shown its widget, re-take the
  // checkpoint's widget snapshot in place so the viewer shows the widget as
  // it is now rather than as it was captured.
  UPROPERTY(config, EditAnywhere, Category = "Restore")
  bool bRefreshSnapshotOnRestore = true;

  UPROPERTY(config, EditAnywhere, Category = "Capture")
  TArray<FString> ExcludedActorProperties;

  UPROPERTY(config, EditAnywhere, Category = "Capture")
  TArray<FString> ExcludedComponentClasses;

  virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

  static UUIWidgetPreviewObjectManagerSettings *Get() {
    return GetMutableDefault<UUIWidgetPreviewObjectManagerSettings>();
  }

  FGuid AddWidgetPreviewObject(const TSoftClassPtr<UUserWidget> &InClass,
                               const FString &InWidgetName);

  void RemoveWidgetPreviewObject(const FGuid &Id);

  FGuid DuplicateWidgetPreviewObject(const FGuid &SourceId);

  FWidgetPreviewObject *FindWidgetPreviewObject(const FGuid &Id);

  void SaveWidgetPreviewObjects();

  FString GetResolvedCheckpointDirectory() const;

  FUIWTCheckpointLoadOptions MakeLoadOptions() const;

  TSet<FGuid> GetAssignedCheckpointIds() const;

  // True when ExcludedActorProperties names the property for InOwnerClass or
  // any of its ancestors, or for every class through a "*:Property" entry.
  bool IsActorPropertyExcluded(const UClass *InOwnerClass,
                               FName InPropertyName) const;

  bool IsComponentClassExcluded(const FString &InClassPath) const;

  virtual void PostInitProperties() override;

#if WITH_EDITOR
  virtual void
  PostEditChangeProperty(FPropertyChangedEvent &PropertyChangedEvent) override;
#endif

private:
  TMap<FGuid, int32> IdToIndex;

  void RebuildIndex();
};
