#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"

/** Registers the plugin's icon set (Resources/Icon128.png) with Slate. */
class FUIWidgetToolPluginStyle {
public:
  static void Initialize();
  static void Shutdown();

  static FName GetStyleSetName();
  static const ISlateStyle &Get() { return *StyleInstance; }

private:
  static TSharedRef<class FSlateStyleSet> Create();

  static TSharedPtr<class FSlateStyleSet> StyleInstance;
};
