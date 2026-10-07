#include "UIWTJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

double UIWTJson::Num(const FJsonObject &InObject, const TCHAR *InField, double InDefault)
{
  double Value = InDefault;
  return InObject.TryGetNumberField(InField, Value) ? Value : InDefault;
}

FString UIWTJson::Str(const FJsonObject &InObject, const TCHAR *InField)
{
  FString Value;
  InObject.TryGetStringField(InField, Value);
  return Value;
}

bool UIWTJson::Bool(const FJsonObject &InObject, const TCHAR *InField, bool bInDefault)
{
  bool bValue = bInDefault;
  return InObject.TryGetBoolField(InField, bValue) ? bValue : bInDefault;
}

const FJsonObject *UIWTJson::Obj(const FJsonObject &InObject, const TCHAR *InField)
{
  const TSharedPtr<FJsonObject> *Value = nullptr;
  return InObject.TryGetObjectField(InField, Value) && Value->IsValid() ? Value->Get() : nullptr;
}

const UIWTJson::FJsonArray *UIWTJson::Arr(const FJsonObject &InObject, const TCHAR *InField)
{
  const FJsonArray *Value = nullptr;
  return InObject.TryGetArrayField(InField, Value) ? Value : nullptr;
}

FString UIWTJson::Pretty(const TSharedRef<FJsonObject> &InObject)
{
  FString Json;
  TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
      TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);
  FJsonSerializer::Serialize(InObject, Writer);
  return Json;
}

FString UIWTJson::Condensed(const TSharedRef<FJsonObject> &InObject)
{
  FString Json;
  TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
      TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
  FJsonSerializer::Serialize(InObject, Writer);
  return Json;
}
