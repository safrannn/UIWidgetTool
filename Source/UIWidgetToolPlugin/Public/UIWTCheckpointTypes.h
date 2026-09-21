#pragma once

#include "CoreMinimal.h"

#include "UIWTCheckpointTypes.generated.h"

UIWIDGETTOOLPLUGIN_API DECLARE_LOG_CATEGORY_EXTERN(LogUIWidgetToolPlugin, Log, All);

USTRUCT()
struct UIWIDGETTOOLPLUGIN_API FUIWTComponentRecord {
  GENERATED_BODY()

  UPROPERTY()
  FString ClassPath;

  UPROPERTY()
  FTransform RelativeTransform = FTransform::Identity;

  UPROPERTY()
  TArray<uint8> PropertyData;
};

USTRUCT()
struct UIWIDGETTOOLPLUGIN_API FUIWTActorRecord {
  GENERATED_BODY()

  UPROPERTY()
  FGuid RecordId;

  UPROPERTY()
  FGuid ActorInstanceGuid;

  UPROPERTY()
  FString ActorPath;

  UPROPERTY()
  FString ClassPath;

  UPROPERTY()
  bool bRuntimeSpawned = false;

  UPROPERTY()
  int32 PlayerIndex = INDEX_NONE;

  UPROPERTY()
  FTransform Transform = FTransform::Identity;

  UPROPERTY()
  TArray<uint8> PropertyData;

  UPROPERTY()
  TMap<FName, FUIWTComponentRecord> Components;

  UPROPERTY()
  TArray<uint8> CustomData;
};

USTRUCT()
struct UIWIDGETTOOLPLUGIN_API FUIWTCustomSection {
  GENERATED_BODY()

  UPROPERTY()
  FName SectionName;

  UPROPERTY()
  int32 SchemaVersion = 0;

  UPROPERTY()
  bool bRequired = false;

  UPROPERTY()
  TArray<uint8> Data;
};

USTRUCT()
struct UIWIDGETTOOLPLUGIN_API FUIWTCheckpointHeader {
  GENERATED_BODY()

  UPROPERTY()
  FGuid CheckpointId;

  UPROPERTY()
  FString EngineVersion;

  UPROPERTY()
  FString MapPackagePath;

  UPROPERTY()
  FString LevelSignature;

  UPROPERTY()
  FDateTime CapturedAtUtc = FDateTime(0);

  UPROPERTY()
  FString DisplayName;

  UPROPERTY()
  float WorldTimeSeconds = 0.f;

  UPROPERTY()
  int32 ActorCount = 0;

  UPROPERTY()
  int64 UncompressedPayloadSize = 0;

  UPROPERTY()
  int64 CompressedPayloadSize = 0;

  UPROPERTY()
  uint32 PayloadCrc = 0;
};

USTRUCT()
struct UIWIDGETTOOLPLUGIN_API FUIWTCheckpointPayload {
  GENERATED_BODY()

  UPROPERTY()
  TArray<FUIWTActorRecord> Actors;

  UPROPERTY()
  TArray<FUIWTCustomSection> CustomSections;
};
