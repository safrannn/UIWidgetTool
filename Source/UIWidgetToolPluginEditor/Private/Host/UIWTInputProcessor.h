#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/IInputProcessor.h"

class FUICommandList;

class FUIWTInputProcessor : public IInputProcessor
{
public:
  explicit FUIWTInputProcessor(TSharedRef<FUICommandList> InCommands);

  virtual void Tick(const float DeltaTime, FSlateApplication &SlateApp,
                    TSharedRef<ICursor> Cursor) override {}

  virtual bool HandleKeyDownEvent(FSlateApplication &SlateApp,
                                  const FKeyEvent &InKeyEvent) override;

  virtual const TCHAR *GetDebugName() const override
  {
    return TEXT("UIWidgetToolInputProcessor");
  }

private:
  TSharedRef<FUICommandList> Commands;
};
