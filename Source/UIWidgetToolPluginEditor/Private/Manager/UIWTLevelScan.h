#pragma once

#include "CoreMinimal.h"

struct FUIWTLevelScanRequest
{
  FGuid EntryId;
  FName WidgetPackageName;
};

struct FUIWTLevelScanResult
{
  FGuid EntryId;

  TArray<FName> LevelPackagePaths;
};

namespace UIWTLevelScan
{
  // Finds the level packages that reference each widget package, walking the
  // Asset Registry's referencers up to MaxDepth hops. Every level found is
  // returned, sorted. Synchronous; the registry walk is the whole cost.
  TArray<FUIWTLevelScanResult>
  Scan(const TArray<FUIWTLevelScanRequest> &InRequests);

  FString MakeLevelDisplayName(FName InPackagePath,
                               const TSet<FName> &InAllPaths);

}
