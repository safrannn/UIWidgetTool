#include "UIWTDesignSettingsCustomization.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "UObject/UnrealType.h"

TSharedRef<IDetailCustomization> FUIWTDesignSettingsCustomization::MakeInstance()
{
  return MakeShared<FUIWTDesignSettingsCustomization>();
}

void FUIWTDesignSettingsCustomization::CustomizeDetails(
    IDetailLayoutBuilder &DetailBuilder)
{
  const TArray<UObject *> ManagerSettings = {
      UUIWidgetPreviewObjectManagerSettings::Get()};

  for (TFieldIterator<FProperty> It(UUIWidgetPreviewObjectManagerSettings::StaticClass(),
                                    EFieldIterationFlags::None);
       It; ++It)
  {
    const FProperty *Property = *It;
    if (!Property->HasAnyPropertyFlags(CPF_Edit))
    {
      continue;
    }
    DetailBuilder.EditCategory(FName(Property->GetMetaData(TEXT("Category"))))
        .AddExternalObjectProperty(ManagerSettings, Property->GetFName());
  }
}
