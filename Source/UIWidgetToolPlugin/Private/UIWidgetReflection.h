#pragma once

#include "Blueprint/UserWidget.h"
#include "CoreMinimal.h"
#include "UIWidgetPreviewObjectManagerSettings.h"

// read default value and write user overrides onto a live widget
struct FReflectedProperty {
  FName PropertyName;
  FString CppDataType;
  FString CdoValue;
  bool bSupported;
};

namespace UIWidgetReflection {
/*
False cases:
- property is not editable or should not be editted: not CPF_Edit(property not
user settable), CPF_Transient(for runtime only), or
CPF_DisableEditOnInstance(class default editable but locked on instance).
- property is editable but needs more handling: non basic data types
*/
bool IsSupportedProperty(const FProperty *Property);

// build property list from class CDO.
TArray<FReflectedProperty> BuildPropertyList(UClass *WidgetClass);

// apply user overrides onto a live widget instance.
// returns the property names that failed to apply so the row can flag them.
TArray<FName>
ApplyOverrides(UUserWidget *Widget,
               const TArray<FWidgetPreviewObjectInputDescriptor> &Overrides);
} // namespace UIWidgetReflection
