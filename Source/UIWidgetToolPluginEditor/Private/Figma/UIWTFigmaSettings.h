#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "UIWTFigmaSettings.generated.h"

// Figma-only import settings shared by the team (DefaultEditor.ini). The
// settings every design source shares are in UUIWTDesignSettings; the token
// is per developer, in UUIWTLocalSettings.
UCLASS(config = Editor, defaultconfig,
       meta = (DisplayName = "UI Widget Tool (Figma import)"))
class UUIWTFigmaSettings : public UDeveloperSettings
{
  GENERATED_BODY()

public:
  virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

  static const UUIWTFigmaSettings *Get()
  {
    return GetDefault<UUIWTFigmaSettings>();
  }

  // Scale Figma renders layers it can't draw as widgets at (vectors,
  // gradients, layers with effects), and the frame's reference image. 2
  // keeps them sharp at up to twice the reference resolution.
  UPROPERTY(config, EditAnywhere, Category = "Images",
            meta = (ClampMin = "1", ClampMax = "4"))
  float RenderScale = 2.f;

  // Image fills with more texture pixels per design pixel than this are
  // downscaled on import (a 4000-pixel photo in a 300-pixel card).
  UPROPERTY(config, EditAnywhere, Category = "Images",
            meta = (ClampMin = "1", ClampMax = "8"))
  float MaxImageScale = 2.f;

  // Seconds a single Figma request may take.
  UPROPERTY(config, EditAnywhere, Category = "Network",
            meta = (ClampMin = "10", Units = "s"))
  int32 RequestTimeoutSeconds = 120;
};
