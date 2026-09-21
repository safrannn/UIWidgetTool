#pragma once

#include "CoreMinimal.h"
#include "UIWTCheckpointCodec.h"

struct FUIWTCheckpointIndexEntry
{
  FString SidecarPath;
  FString PayloadPath;

  FUIWTCheckpointHeader Header;

  bool bValid = false;
  FString ErrorCode;

  // Empty when no .widgetsnapshot sits beside the sidecar.
  FString SnapshotPath;

  bool HasSnapshot() const { return !SnapshotPath.IsEmpty(); }

  // "<label> - <captured at> - <N actors>" on one line, or across three.
  FString GetDisplayString(bool bMultiLine = false) const;
};

class FUIWTCheckpointIndex
{
public:
  void Rebuild();

  const TArray<FUIWTCheckpointIndexEntry> &GetEntries() const
  {
    return Entries;
  }

  // Any indexed entry with that id, readable or not; null for an invalid id.
  const FUIWTCheckpointIndexEntry *Find(const FGuid &InCheckpointId) const;

  // Only an entry whose payload validated.
  const FUIWTCheckpointIndexEntry *FindValid(const FGuid &InCheckpointId) const
  {
    const FUIWTCheckpointIndexEntry *Entry = Find(InCheckpointId);
    return Entry && Entry->bValid ? Entry : nullptr;
  }

  // Valid entries only; an empty map path selects every map.
  TArray<const FUIWTCheckpointIndexEntry *>
  GetValid(const FString &InMapPackagePath = FString()) const;

  const FString &GetDirectory() const { return Directory; }

  bool DirectoryExists() const { return bDirectoryExists; }

  int32 NumInvalid() const;

private:
  TArray<FUIWTCheckpointIndexEntry> Entries;
  TMap<FGuid, int32> IdToIndex;
  FString Directory;
  bool bDirectoryExists = false;
};
