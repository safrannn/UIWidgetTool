#pragma once

#include "CoreMinimal.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "UIWTSnapshotFormat.h"

struct FUIWTSnapshotRoot {
  TSharedPtr<FUIWTSnapshotNode> Node;
  TSharedPtr<FSlateDynamicImageBrush> Brush;
  FIntPoint Dimensions = FIntPoint::ZeroValue;
  int32 WidgetCount = 0;
  FString DisplayName;

  bool HasImage() const { return Brush.IsValid(); }
};

struct FUIWTSnapshotDocument {
  FString FilePath;
  TArray<FUIWTSnapshotRoot> Roots;
  FString Warning;

  FUIWTSnapshotDocument() = default;
  ~FUIWTSnapshotDocument();

  FUIWTSnapshotDocument(const FUIWTSnapshotDocument &) = delete;
  FUIWTSnapshotDocument &operator=(const FUIWTSnapshotDocument &) = delete;

  int32 TotalWidgetCount() const;

  const FUIWTSnapshotRoot *GetRoot(int32 InIndex) const {
    return Roots.IsValidIndex(InIndex) ? &Roots[InIndex] : nullptr;
  }
};

namespace UIWTSnapshotReader {

bool LoadSnapshotDocument(const FString &InFilePath,
                          FUIWTSnapshotDocument &OutDocument, FText &OutError);

}
