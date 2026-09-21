#include "UIWTDirectoryWatcher.h"

#include "DirectoryWatcherModule.h"
#include "HAL/FileManager.h"
#include "IDirectoryWatcher.h"
#include "Modules/ModuleManager.h"

namespace
{
  constexpr float DebounceSeconds = 0.5f;
}

FUIWTDirectoryWatcher::~FUIWTDirectoryWatcher()
{
  Stop();
}

void FUIWTDirectoryWatcher::Watch(const FString &InDirectory)
{
  if (InDirectory == WatchedDirectory && WatchHandle.IsValid())
  {
    return;
  }
  Stop();

  if (InDirectory.IsEmpty() ||
      !IFileManager::Get().DirectoryExists(*InDirectory))
  {
    return;
  }

  FDirectoryWatcherModule &Module =
      FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(
          "DirectoryWatcher");
  IDirectoryWatcher *Watcher = Module.Get();
  if (!Watcher)
  {
    return;
  }

  Watcher->RegisterDirectoryChangedCallback_Handle(
      InDirectory,
      IDirectoryWatcher::FDirectoryChanged::CreateRaw(
          this, &FUIWTDirectoryWatcher::OnDirectoryChanged),
      WatchHandle);
  WatchedDirectory = InDirectory;
}

void FUIWTDirectoryWatcher::Stop()
{
  if (DebounceHandle.IsValid())
  {
    FTSTicker::GetCoreTicker().RemoveTicker(DebounceHandle);
    DebounceHandle.Reset();
  }
  if (!WatchHandle.IsValid() || WatchedDirectory.IsEmpty())
  {
    return;
  }
  if (FDirectoryWatcherModule *Module =
          FModuleManager::GetModulePtr<FDirectoryWatcherModule>(
              "DirectoryWatcher"))
  {
    if (IDirectoryWatcher *Watcher = Module->Get())
    {
      Watcher->UnregisterDirectoryChangedCallback_Handle(WatchedDirectory,
                                                         WatchHandle);
    }
  }
  WatchHandle.Reset();
  WatchedDirectory.Reset();
}

void FUIWTDirectoryWatcher::OnDirectoryChanged(
    const TArray<FFileChangeData> &Changes)
{
  if (DebounceHandle.IsValid())
  {
    FTSTicker::GetCoreTicker().RemoveTicker(DebounceHandle);
  }
  DebounceHandle = FTSTicker::GetCoreTicker().AddTicker(
      FTickerDelegate::CreateRaw(this, &FUIWTDirectoryWatcher::TickDebounce),
      DebounceSeconds);
}

bool FUIWTDirectoryWatcher::TickDebounce(float DeltaTime)
{
  DebounceHandle.Reset();
  OnChanged.ExecuteIfBound();
  return false;
}
