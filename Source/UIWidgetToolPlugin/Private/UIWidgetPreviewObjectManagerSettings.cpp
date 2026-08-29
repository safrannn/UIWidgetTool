#include "UIWidgetPreviewObjectManagerSettings.h"

FGuid UUIWidgetPreviewObjectManagerSettings::AddWidgetPreviewObject(
    const TSoftClassPtr<UUserWidget> &InClass, const FString &InWidgetName) {
  FWidgetPreviewObject WidgetPreviewObject;
  WidgetPreviewObject.Id = FGuid::NewGuid();
  WidgetPreviewObject.WidgetClass = InClass;
  WidgetPreviewObject.WidgetName = InWidgetName;

  const int32 NewIndex = WidgetPreviewObjects.Add(WidgetPreviewObject);
  IdToIndex.Add(WidgetPreviewObject.Id, NewIndex);

  SaveWidgetPreviewObjects();
  return WidgetPreviewObject.Id;
}

void UUIWidgetPreviewObjectManagerSettings::RemoveWidgetPreviewObject(
    const FGuid &Id) {
  const int32 *IndexPtr = IdToIndex.Find(Id);
  if (!IndexPtr) {
    return;
  }

  const int32 IndexToRemove = *IndexPtr;
  const int32 LastIndex = WidgetPreviewObjects.Num() - 1;

  if (IndexToRemove != LastIndex) {
    WidgetPreviewObjects.Swap(IndexToRemove, LastIndex);
    IdToIndex.Add(WidgetPreviewObjects[IndexToRemove].Id, IndexToRemove);
  }

  WidgetPreviewObjects.RemoveAt(LastIndex);
  IdToIndex.Remove(Id);

  SaveWidgetPreviewObjects();
}

FWidgetPreviewObject *
UUIWidgetPreviewObjectManagerSettings::FindWidgetPreviewObject(
    const FGuid &Id) {
  if (const int32 *IndexPtr = IdToIndex.Find(Id)) {
    return &WidgetPreviewObjects[*IndexPtr];
  }
  return nullptr;
}

TArray<const FWidgetPreviewObject *>
UUIWidgetPreviewObjectManagerSettings::GetWidgetPreviewObjectsSortedForTable()
    const {
  TArray<const FWidgetPreviewObject *> Sorted;
  Sorted.Reserve(WidgetPreviewObjects.Num());
  for (const FWidgetPreviewObject &WidgetPreviewObject : WidgetPreviewObjects) {
    Sorted.Add(&WidgetPreviewObject);
  }

  Sorted.Sort([](const FWidgetPreviewObject &A, const FWidgetPreviewObject &B) {
    const int32 NameCompare = A.WidgetName.Compare(B.WidgetName);
    if (NameCompare != 0) {
      return NameCompare < 0;
    }

    if (A.Id.A != B.Id.A)
      return A.Id.A < B.Id.A;
    if (A.Id.B != B.Id.B)
      return A.Id.B < B.Id.B;
    if (A.Id.C != B.Id.C)
      return A.Id.C < B.Id.C;
    return A.Id.D < B.Id.D;
  });

  return Sorted;
}

void UUIWidgetPreviewObjectManagerSettings::SaveWidgetPreviewObjects() {
  SaveConfig();
  TryUpdateDefaultConfigFile();
}

void UUIWidgetPreviewObjectManagerSettings::PostInitProperties() {
  Super::PostInitProperties();
  RebuildIndex();
}

void UUIWidgetPreviewObjectManagerSettings::RebuildIndex() {
  IdToIndex.Reset();
  IdToIndex.Reserve(WidgetPreviewObjects.Num());
  for (int32 Index = 0; Index < WidgetPreviewObjects.Num(); ++Index) {
    IdToIndex.Add(WidgetPreviewObjects[Index].Id, Index);
  }
}
