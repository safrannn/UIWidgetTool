#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

// Adds the Claude Code install button to UI Widget Tool (local) settings.
class FUIWTLocalSettingsCustomization : public IDetailCustomization
{
public:
  static TSharedRef<IDetailCustomization> MakeInstance();

  virtual void CustomizeDetails(IDetailLayoutBuilder &DetailBuilder) override;
};
