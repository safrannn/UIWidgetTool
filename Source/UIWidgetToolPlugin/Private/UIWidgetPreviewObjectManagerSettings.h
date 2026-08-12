#pragma once

#include "Blueprint/UserWidget.h"
#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UIWidgetPreviewObjectManagerSettings.generated.h"

USTRUCT()
struct FWidgetPreviewObjectInputDescriptor {
  GENERATED_BODY()

  UPROPERTY()
  FName PropertyName;

  UPROPERTY()
  FString OverrideValue;
};

USTRUCT()
struct FWidgetPreviewObject {
  GENERATED_BODY()

  UPROPERTY()
  FGuid Id;

  UPROPERTY()
  FString LevelBookmark;

  UPROPERTY()
  FString WidgetName;

  // soft ref only
  UPROPERTY()
  TSoftClassPtr<UUserWidget> WidgetClass;

  // user overrided widget property
  UPROPERTY()
  TArray<FWidgetPreviewObjectInputDescriptor> Overrides;
};

UCLASS(config = EditorPerProjectUserSettings, defaultconfig)
class UUIWidgetPreviewObjectManagerSettings : public UDeveloperSettings {
  GENERATED_BODY()

public:
  UPROPERTY(config, EditAnywhere, Category = "UI Widget Tool")
  TArray<FWidgetPreviewObject> WidgetPreviewObjects;

  virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

  static UUIWidgetPreviewObjectManagerSettings *Get() {
    return GetMutableDefault<UUIWidgetPreviewObjectManagerSettings>();
  }

  FGuid AddWidgetPreviewObject(const TSoftClassPtr<UUserWidget> &InClass,
                               const FString &InWidgetName);

  void RemoveWidgetPreviewObject(const FGuid &Id);

  FWidgetPreviewObject *FindWidgetPreviewObject(const FGuid &Id);

  // data entries for table display sorted by widget name then id.
  TArray<const FWidgetPreviewObject *>
  GetWidgetPreviewObjectsSortedForTable() const;

  void SaveWidgetPreviewObjects();

  virtual void PostInitProperties() override;

private:
  TMap<FGuid, int32> IdToIndex;

  void RebuildIndex();
};
