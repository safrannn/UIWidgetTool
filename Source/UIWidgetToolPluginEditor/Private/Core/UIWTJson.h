#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

// Small JSON helpers the design readers and tools share: lenient field
// reads (a missing or mistyped field gives the default) and writing an
// object as text.
namespace UIWTJson
{
  using FJsonArray = TArray<TSharedPtr<FJsonValue>>;

  double Num(const FJsonObject &InObject, const TCHAR *InField, double InDefault = 0.0);
  FString Str(const FJsonObject &InObject, const TCHAR *InField);
  bool Bool(const FJsonObject &InObject, const TCHAR *InField, bool bInDefault);
  // Null when the field is missing or not an object (or array).
  const FJsonObject *Obj(const FJsonObject &InObject, const TCHAR *InField);
  const FJsonArray *Arr(const FJsonObject &InObject, const TCHAR *InField);

  FString Pretty(const TSharedRef<FJsonObject> &InObject);
  FString Condensed(const TSharedRef<FJsonObject> &InObject);
}
