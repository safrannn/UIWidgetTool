#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"

struct FFileChangeData;

// Keeps the checkpoint list current when file changes.
class FUIWTDirectoryWatcher
{
public:
  FUIWTDirectoryWatcher() = default;
  ~FUIWTDirectoryWatcher();
  UE_NONCOPYABLE(FUIWTDirectoryWatcher)

  void Watch(const FString &InDirectory);
  void Stop();

  FSimpleDelegate OnChanged;

private:
  void OnDirectoryChanged(const TArray<FFileChangeData> &Changes);
  bool TickDebounce(float DeltaTime);

  FString WatchedDirectory;
  FDelegateHandle WatchHandle;
  FTSTicker::FDelegateHandle DebounceHandle;
};
