#pragma once

#include "Blueprint/UserWidget.h"

#include "UIWTSpecTestWidget.generated.h"

class UImage;
class UTextBlock;

// Parent class for the widget spec tests (UIWTWidgetSpecTests.cpp): one
// required and one optional BindWidget, to check how Apply treats them.
// Hidden from class pickers; nothing else uses it.
UCLASS(HideDropdown)
class UUIWTSpecTestWidget : public UUserWidget
{
  GENERATED_BODY()

public:
  UPROPERTY(meta = (BindWidget))
  TObjectPtr<UTextBlock> RequiredText;

  UPROPERTY(meta = (BindWidgetOptional))
  TObjectPtr<UImage> OptionalImage;
};
