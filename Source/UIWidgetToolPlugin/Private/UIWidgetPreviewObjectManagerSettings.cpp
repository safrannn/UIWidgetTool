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

#if WITH_EDITOR
void UUIWidgetPreviewObjectManagerSettings::PostEditChangeProperty(
    FPropertyChangedEvent &PropertyChangedEvent) {
  Super::PostEditChangeProperty(PropertyChangedEvent);

  // WidgetPreviewObjects is EditAnywhere, so Project Settings can add, remove,
  // reorder or duplicate entries without going through Add/RemoveWidgetPreview
  // Object. Every one of those invalidates IdToIndex, after which
  // FindWidgetPreviewObject returns the wrong entry or indexes out of bounds.
  RebuildIndex();

  // RebuildIndex may have assigned Ids to hand-added or duplicated entries;
  // persist them so they survive a restart.
  SaveWidgetPreviewObjects();
}
#endif

void UUIWidgetPreviewObjectManagerSettings::RebuildIndex() {
  IdToIndex.Reset();
  IdToIndex.Reserve(WidgetPreviewObjects.Num());
  for (int32 Index = 0; Index < WidgetPreviewObjects.Num(); ++Index) {
    FWidgetPreviewObject &WidgetPreviewObject = WidgetPreviewObjects[Index];

    // An entry added by hand in Project Settings arrives with a default
    // (invalid) Id, and the array UI's duplicate action copies an existing one.
    // Either case would collapse two entries onto a single map slot, so give
    // the offender a fresh Id before indexing it.
    if (!WidgetPreviewObject.Id.IsValid() ||
        IdToIndex.Contains(WidgetPreviewObject.Id)) {
      WidgetPreviewObject.Id = FGuid::NewGuid();
    }

    IdToIndex.Add(WidgetPreviewObject.Id, Index);
  }
}
