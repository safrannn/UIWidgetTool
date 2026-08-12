#include "UIWidgetToolPluginStyle.h"

#include "Brushes/SlateImageBrush.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyleRegistry.h"

TSharedPtr<FSlateStyleSet> FUIWidgetToolPluginStyle::StyleInstance = nullptr;

void FUIWidgetToolPluginStyle::Initialize() {
  if (!StyleInstance.IsValid()) {
    StyleInstance = Create();
    FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
  }
}

void FUIWidgetToolPluginStyle::Shutdown() {
  if (StyleInstance.IsValid()) {
    FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
    StyleInstance.Reset();
  }
}

FName FUIWidgetToolPluginStyle::GetStyleSetName() {
  static FName StyleSetName(TEXT("UIWidgetToolPluginStyle"));
  return StyleSetName;
}

TSharedRef<FSlateStyleSet> FUIWidgetToolPluginStyle::Create() {
  TSharedRef<FSlateStyleSet> Style = MakeShared<FSlateStyleSet>(GetStyleSetName());
  Style->SetContentRoot(IPluginManager::Get()
                            .FindPlugin(TEXT("UIWidgetToolPlugin"))
                            ->GetBaseDir() /
                        TEXT("Resources"));

  const FVector2D Icon40x40(40.f, 40.f);
  const FVector2D Icon20x20(20.f, 20.f);
  Style->Set("UIWidgetTool.OpenManager",
             new FSlateImageBrush(Style->RootToContentDir(TEXT("Icon128"), TEXT(".png")), Icon40x40));
  Style->Set("UIWidgetTool.OpenManager.Small",
             new FSlateImageBrush(Style->RootToContentDir(TEXT("Icon128"), TEXT(".png")), Icon20x20));

  return Style;
}
