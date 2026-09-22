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

  FString SnapshotPath;

  bool HasSnapshot() const { return !SnapshotPath.IsEmpty(); }

  // The sidecar's DisplayName, or "<MapName>_<CapturedAtUtc>" when it was
  // cleared. This is what the rename box opens with.
  FString GetEffectiveDisplayName() const
  {
    return Header.DisplayName.IsEmpty()
               ? UIWTCheckpointCodec::MakeDefaultDisplayName(Header)
               : Header.DisplayName;
  }

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

  const FUIWTCheckpointIndexEntry *Find(const FGuid &InCheckpointId) const;

  const FUIWTCheckpointIndexEntry *FindValid(const FGuid &InCheckpointId) const
  {
    const FUIWTCheckpointIndexEntry *Entry = Find(InCheckpointId);
    return Entry && Entry->bValid ? Entry : nullptr;
  }

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
