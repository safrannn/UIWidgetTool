#include "UIWidgetToolPluginStyle.h"

#include "Brushes/SlateImageBrush.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Styling/SlateStyleRegistry.h"

TSharedPtr<FSlateStyleSet> FUIWidgetToolPluginStyle::StyleInstance = nullptr;

void FUIWidgetToolPluginStyle::Initialize()
{
  if (!StyleInstance.IsValid())
  {
    StyleInstance = Create();
    FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
  }
}

void FUIWidgetToolPluginStyle::Shutdown()
{
  if (StyleInstance.IsValid())
  {
    FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
    StyleInstance.Reset();
  }
}

FName FUIWidgetToolPluginStyle::GetStyleSetName()
{
  static FName StyleSetName(TEXT("UIWidgetToolPluginStyle"));
  return StyleSetName;
}

TSharedRef<FSlateStyleSet> FUIWidgetToolPluginStyle::Create()
{
  TSharedRef<FSlateStyleSet> Style = MakeShared<FSlateStyleSet>(GetStyleSetName());
  Style->SetContentRoot(IPluginManager::Get()
                            .FindPlugin(TEXT("UIWidgetToolPlugin"))
                            ->GetBaseDir() /
                        TEXT("Resources"));

  const FVector2f Icon20x20(20.f, 20.f);
  Style->Set("UIWidgetTool.OpenManager",
             new FSlateVectorImageBrush(
                 FPaths::EngineContentDir() /
                     TEXT("Editor/Slate/Starship/Common/Widget.svg"),
                 Icon20x20));

  const FVector2f Icon16x16(16.f, 16.f);
  Style->Set("UIWidgetTool.Icons.Delete",
             new FSlateVectorImageBrush(
                 FPaths::EngineContentDir() /
                     TEXT("Slate/Starship/Common/Delete.svg"),
                 Icon16x16));

  Style->Set("UIWidgetTool.Icons.Update",
             new FSlateVectorImageBrush(
                 FPaths::EngineContentDir() /
                     TEXT("Slate/Starship/Common/update.svg"),
                 Icon16x16));

  Style->Set("UIWidgetTool.Icons.Settings",
             new FSlateVectorImageBrush(
                 FPaths::EngineContentDir() /
                     TEXT("Slate/Starship/Common/settings.svg"),
                 Icon16x16));

  // Snapshot viewer tabs.
  Style->Set("UIWidgetTool.Icons.RuntimeTree",
             new FSlateVectorImageBrush(
                 FPaths::EngineContentDir() /
                     TEXT("Slate/Starship/Common/file-tree.svg"),
                 Icon16x16));

  Style->Set("UIWidgetTool.Icons.Blueprint",
             new FSlateVectorImageBrush(
                 FPaths::EngineContentDir() /
                     TEXT("Slate/Starship/Common/blueprint.svg"),
                 Icon16x16));

  Style->Set("UIWidgetTool.Icons.OpenFile",
             new FSlateVectorImageBrush(
                 FPaths::EngineContentDir() /
                     TEXT("Slate/Starship/Common/folder-open.svg"),
                 Icon16x16));

  Style->Set("UIWidgetTool.Icons.OpenLevel",
             new FSlateVectorImageBrush(
                 FPaths::EngineContentDir() /
                     TEXT("Editor/Slate/Starship/Common/LevelOpen.svg"),
                 Icon16x16));

  Style->Set("UIWidgetTool.Icons.Rename",
             new FSlateVectorImageBrush(
                 FPaths::EngineContentDir() /
                     TEXT("Slate/Starship/Common/Rename.svg"),
                 Icon16x16));

  // Chat prompt image attachment.
  Style->Set("UIWidgetTool.Icons.AddImage",
             new FSlateVectorImageBrush(
                 FPaths::EngineContentDir() /
                     TEXT("Slate/Starship/Common/plus.svg"),
                 Icon16x16));

  Style->Set("UIWidgetTool.Icons.RemoveImage",
             new FSlateVectorImageBrush(
                 FPaths::EngineContentDir() /
                     TEXT("Slate/Starship/Common/close-small.svg"),
                 Icon16x16));

  return Style;
}
