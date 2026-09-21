#include "UIWTInputProcessor.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UICommandList.h"
#include "Widgets/SWidget.h"

FUIWTInputProcessor::FUIWTInputProcessor(TSharedRef<FUICommandList> InCommands)
    : Commands(InCommands) {}

bool FUIWTInputProcessor::HandleKeyDownEvent(FSlateApplication &SlateApp,
                                             const FKeyEvent &InKeyEvent)
{
  if (InKeyEvent.IsRepeat())
  {
    return false;
  }

  if (TSharedPtr<SWidget> Focused = SlateApp.GetKeyboardFocusedWidget())
  {
    const FString WidgetType = Focused->GetType().ToString();
    if (WidgetType.Contains(TEXT("EditableText")) ||
        WidgetType.Contains(TEXT("MultiLineEditableText")))
    {
      return false;
    }
  }

  return Commands->ProcessCommandBindings(InKeyEvent);
}
