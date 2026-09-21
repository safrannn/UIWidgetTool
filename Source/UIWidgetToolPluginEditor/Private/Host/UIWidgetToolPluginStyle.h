#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"

class FUIWidgetToolPluginStyle
{
public:
  static void Initialize();
  static void Shutdown();

  static FName GetStyleSetName();
  static const ISlateStyle &Get() { return *StyleInstance; }

private:
  static TSharedRef<class FSlateStyleSet> Create();

  static TSharedPtr<class FSlateStyleSet> StyleInstance;
};
