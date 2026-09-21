#pragma once

#include "CoreMinimal.h"
#include "UIWTCheckpointTypes.h"
#include "UIWTRestoreReport.h"
#include "UObject/Interface.h"

#include "UIWTSaveable.generated.h"

class UWorld;

UINTERFACE(MinimalAPI, Blueprintable)
class UUIWidgetToolSaveable : public UInterface
{
  GENERATED_BODY()
};

class UIWIDGETTOOLPLUGIN_API IUIWidgetToolSaveable
{
  GENERATED_BODY()

public:
  UFUNCTION(BlueprintNativeEvent, BlueprintCallable,
            Category = "UI Widget Tool|Checkpoint")
  bool ShouldCaptureToCheckpoint() const;
  virtual bool ShouldCaptureToCheckpoint_Implementation() const { return true; }

  UFUNCTION(BlueprintNativeEvent, BlueprintCallable,
            Category = "UI Widget Tool|Checkpoint")
  int32 GetCheckpointDataVersion() const;
  virtual int32 GetCheckpointDataVersion_Implementation() const { return 1; }

  UFUNCTION(BlueprintNativeEvent, BlueprintCallable,
            Category = "UI Widget Tool|Checkpoint")
  void OnCheckpointCapture(TArray<uint8> &OutCustomData);
  virtual void
  OnCheckpointCapture_Implementation(TArray<uint8> &OutCustomData) {}

  UFUNCTION(BlueprintNativeEvent, BlueprintCallable,
            Category = "UI Widget Tool|Checkpoint")
  void OnCheckpointRestore(const TArray<uint8> &InCustomData,
                           int32 InDataVersion);
  virtual void
  OnCheckpointRestore_Implementation(const TArray<uint8> &InCustomData,
                                     int32 InDataVersion) {}

  UFUNCTION(BlueprintNativeEvent, BlueprintCallable,
            Category = "UI Widget Tool|Checkpoint")
  bool IsSafeToDestroyOnRestore() const;
  virtual bool IsSafeToDestroyOnRestore_Implementation() const { return false; }
};

// Whether InObject's class implements the interface, natively or in Blueprint.
inline bool UIWTImplementsSaveable(const UObject *InObject)
{
  return InObject->GetClass()->ImplementsInterface(
      UUIWidgetToolSaveable::StaticClass());
}

DECLARE_MULTICAST_DELEGATE_TwoParams(FUIWTOnCaptureSections, UWorld *, TArray<FUIWTCustomSection> &);

DECLARE_MULTICAST_DELEGATE_ThreeParams(FUIWTOnRestoreSections, UWorld *, const TArray<FUIWTCustomSection> &, FUIWTRestoreReport &);

namespace UIWTCheckpointDelegates
{
  UIWIDGETTOOLPLUGIN_API FUIWTOnCaptureSections &OnCaptureSections();
  UIWIDGETTOOLPLUGIN_API FUIWTOnRestoreSections &OnRestoreSections();
}
