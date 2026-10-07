#pragma once

#include "CoreMinimal.h"
#include "Misc/Paths.h"

namespace UIWTPaths
{

  // True when InPath is strictly below InDirectory once both are made
  // absolute and "..", "." and separators are resolved; case-insensitive.
  inline bool IsInsideDirectory(const FString &InPath, const FString &InDirectory)
  {
    FString Path = FPaths::ConvertRelativePathToFull(InPath);
    FString Directory = FPaths::ConvertRelativePathToFull(InDirectory);
    FPaths::NormalizeFilename(Path);
    FPaths::NormalizeDirectoryName(Directory);
    FPaths::CollapseRelativeDirectories(Path);
    return Path.StartsWith(Directory + TEXT("/"), ESearchCase::IgnoreCase);
  }

}
