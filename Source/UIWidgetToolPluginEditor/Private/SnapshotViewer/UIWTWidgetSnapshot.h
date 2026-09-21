#pragma once

#include "CoreMinimal.h"
#include "UIWTSnapshotFormat.h"

namespace UIWTWidgetSnapshot {

struct FSnapshotResult {
  FString FilePath;

  int32 RootCount = 0;
  int32 WidgetCount = 0;
  int64 FileBytes = 0;

  FString RootDescription;

  FString Message;
};

// Captures every PIE viewport's widget tree and screenshot to InFilePath. A
// relative or empty path is resolved against the checkpoint directory, so
// each snapshot sits beside its checkpoint.
bool TakeSnapshot(const FString &InFilePath, FSnapshotResult &OutResult);

}
