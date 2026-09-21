#include "UIWTCommands.h"

#define LOCTEXT_NAMESPACE "UIWidgetTool"

FUIWTCommands::FUIWTCommands()
    : TCommands<FUIWTCommands>(TEXT("UIWidgetTool"),
                               LOCTEXT("UIWidgetTool", "UI Widget Tool"),
                               NAME_None,
                               FUIWidgetToolPluginStyle::GetStyleSetName()) {}

void FUIWTCommands::RegisterCommands()
{
  UI_COMMAND(OpenManager, "UI Widget Tool",
             "Open the UI Widget Tool manager.", EUserInterfaceActionType::Button,
             FInputChord());

  UI_COMMAND(CaptureCheckpoint, "Add level checkpoint",
             "Capture the active PIE world to a new level checkpoint.\n"
             "Press Alt+F3 during play, or use Shift+F1 and click this button.", EUserInterfaceActionType::Button,
             FInputChord(EModifierKey::Alt, EKeys::F3));
}

#undef LOCTEXT_NAMESPACE
