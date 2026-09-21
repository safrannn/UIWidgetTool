#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"

struct FFileChangeData;

// Watches one directory through the DirectoryWatcher module and fires
// OnChanged half a second after the last change, so a burst of writes is
// reported once. Stops itself when destroyed.
class FUIWTDirectoryWatcher
{
public:
  FUIWTDirectoryWatcher() = default;
  ~FUIWTDirectoryWatcher();
  UE_NONCOPYABLE(FUIWTDirectoryWatcher)

  // Watches InDirectory, replacing any earlier watch. An empty or missing
  // directory just stops the watch.
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
