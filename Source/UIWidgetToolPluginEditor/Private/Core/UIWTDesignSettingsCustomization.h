#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

// Adds the manager and checkpoint settings (UUIWidgetPreviewObjectManagerSettings,
// per developer) to the UI Widget Tool project settings page, under their own
// categories. They still save to their own ini, from their PostEditChangeProperty.
class FUIWTDesignSettingsCustomization : public IDetailCustomization
{
public:
  static TSharedRef<IDetailCustomization> MakeInstance();

  virtual void CustomizeDetails(IDetailLayoutBuilder &DetailBuilder) override;
};
