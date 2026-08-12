#include "UIWidgetReflection.h"
#include "UIWidgetPreviewObjectManagerSettings.h"

namespace UIWidgetReflection {
bool IsSupportedProperty(const FProperty *Property) {
  if (!Property) {
    return false;
  }

  if (!Property->HasAnyPropertyFlags(CPF_Edit) ||
      Property->HasAnyPropertyFlags(CPF_Transient |
                                    CPF_DisableEditOnInstance)) {
    return false;
  }

  return Property->IsA<FBoolProperty>() ||
         Property->IsA<FNumericProperty>() // int/float/byte/enum-as-numeric
         || Property->IsA<FEnumProperty>() || Property->IsA<FNameProperty>() ||
         Property->IsA<FStrProperty>() || Property->IsA<FTextProperty>();
}

TArray<FReflectedProperty> BuildPropertyList(UClass *WidgetClass) {
  TArray<FReflectedProperty> Result;
  if (!WidgetClass) {
    return Result;
  }

  const UObject *Cdo = WidgetClass->GetDefaultObject();
  if (!Cdo) {
    return Result;
  }

  for (TFieldIterator<FProperty> It(WidgetClass); It; ++It) {
    const FProperty *Property = *It;

    FReflectedProperty WidgetPreviewObject;
    WidgetPreviewObject.PropertyName = Property->GetFName();
    WidgetPreviewObject.CppDataType = Property->GetCPPType();
    WidgetPreviewObject.bSupported = IsSupportedProperty(Property);

    // live CDO value for the greyed-out placeholder.
    const void *ValuePtr = Property->ContainerPtrToValuePtr<void>(Cdo);
    Property->ExportTextItem_Direct(WidgetPreviewObject.CdoValue, ValuePtr,
                                    nullptr, nullptr, PPF_None);

    // Only show properties the designer could meaningfully override.
    if (Property->HasAnyPropertyFlags(CPF_Edit) ||
        WidgetPreviewObject.bSupported) {
      Result.Add(MoveTemp(WidgetPreviewObject));
    }
  }

  return Result;
}

TArray<FName>
ApplyOverrides(UUserWidget *Widget,
               const TArray<FWidgetPreviewObjectInputDescriptor> &Overrides) {
  TArray<FName> Failed;
  if (!Widget) {
    return Failed;
  }

  UClass *Class = Widget->GetClass();

  for (const FWidgetPreviewObjectInputDescriptor &Override : Overrides) {
    if (Override.OverrideValue.IsEmpty()) {
      continue;
    }

    // Resolve fresh against the live class every rebuild — never cache.
    FProperty *Property = Class->FindPropertyByName(Override.PropertyName);
    if (!Property || !IsSupportedProperty(Property)) {
      Failed.Add(Override.PropertyName); // property gone or type changed
      continue;
    }

    void *ValuePtr = Property->ContainerPtrToValuePtr<void>(Widget);
    const TCHAR *Result = Property->ImportText_Direct(
        *Override.OverrideValue, ValuePtr, Widget, PPF_None);

    if (Result == nullptr) {
      Failed.Add(Override.PropertyName); // parse failure
    }
  }

  return Failed;
}
} // namespace UIWidgetReflection
