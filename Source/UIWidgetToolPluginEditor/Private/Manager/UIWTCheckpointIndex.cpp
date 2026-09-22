#include "UIWTCheckpointIndex.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "UIWTCheckpointTypes.h"
#include "UIWidgetPreviewObjectManagerSettings.h"

FString FUIWTCheckpointIndexEntry::GetDisplayString(bool bMultiLine) const
{
  if (!bValid)
  {
    return FString::Printf(TEXT("(invalid: %s)"), *ErrorCode);
  }

  const FString Label = GetEffectiveDisplayName();
  const FString CapturedAt = Header.CapturedAtUtc.ToString(TEXT("%Y-%m-%d %H:%M"));
  const FString Actors =
      FString::Printf(TEXT("%d actors%s"), Header.ActorCount,
                      HasSnapshot() ? TEXT(" + snapshot") : TEXT(""));

  const TCHAR *Separator = bMultiLine ? TEXT("\n") : TEXT(" - ");
  return Label + Separator + CapturedAt + Separator + Actors;
}

void FUIWTCheckpointIndex::Rebuild()
{
  Entries.Reset();
  IdToIndex.Reset();

  const UUIWidgetPreviewObjectManagerSettings *Settings =
      GetDefault<UUIWidgetPreviewObjectManagerSettings>();
  IFileManager &FileManager = IFileManager::Get();

  Directory = Settings->GetResolvedCheckpointDirectory();
  bDirectoryExists = FileManager.DirectoryExists(*Directory);

  const FUIWTCheckpointLoadOptions BaseOptions = Settings->MakeLoadOptions();

  for (const FString &SidecarPath :
       UIWTCheckpointCodec::FindSidecarFiles(Directory))
  {
    FUIWTCheckpointIndexEntry &Entry = Entries.AddDefaulted_GetRef();
    Entry.SidecarPath = SidecarPath;

    FUIWTCheckpointSidecar Sidecar;
    FUIWTCheckpointResult Result;
    if (!UIWTCheckpointCodec::ReadSidecar(SidecarPath, Sidecar, Result))
    {
      Entry.ErrorCode = Result.GetErrorCode();
      continue;
    }

    Entry.PayloadPath =
        FPaths::Combine(FPaths::GetPath(SidecarPath), Sidecar.PayloadFileName);

    const FString SnapshotPath =
        UIWTCheckpointCodec::MakeSnapshotPathFromSidecar(SidecarPath);
    if (FileManager.FileExists(*SnapshotPath))
    {
      Entry.SnapshotPath = SnapshotPath;
    }

    // The binary header is authoritative; the sidecar only located it.
    FUIWTCheckpointLoadOptions Options = BaseOptions;
    Options.ExpectedCheckpointId = Sidecar.Header.CheckpointId;
    Options.ExpectedMapPackagePath = Sidecar.Header.MapPackagePath;
    if (!UIWTCheckpointCodec::ValidateHeader(Entry.PayloadPath, Options,
                                             Entry.Header, Result))
    {
      Entry.Header = Sidecar.Header;
      Entry.ErrorCode = Result.GetErrorCode();
      continue;
    }

    Entry.bValid = true;
  }

  Entries.Sort([](const FUIWTCheckpointIndexEntry &A,
                  const FUIWTCheckpointIndexEntry &B)
               { return A.Header.CapturedAtUtc > B.Header.CapturedAtUtc; });

  for (int32 Index = 0; Index < Entries.Num(); ++Index)
  {
    if (Entries[Index].Header.CheckpointId.IsValid())
    {
      IdToIndex.Add(Entries[Index].Header.CheckpointId, Index);
    }
  }

  UE_LOG(LogUIWidgetToolPlugin, Verbose,
         TEXT("Checkpoint index rebuilt from '%s': %d entries, %d invalid."),
         *Directory, Entries.Num(), NumInvalid());
}

const FUIWTCheckpointIndexEntry *
FUIWTCheckpointIndex::Find(const FGuid &InCheckpointId) const
{
  if (const int32 *Index = IdToIndex.Find(InCheckpointId))
  {
    return &Entries[*Index];
  }
  return nullptr;
}

TArray<const FUIWTCheckpointIndexEntry *>
FUIWTCheckpointIndex::GetValid(const FString &InMapPackagePath) const
{
  TArray<const FUIWTCheckpointIndexEntry *> Result;
  for (const FUIWTCheckpointIndexEntry &Entry : Entries)
  {
    if (Entry.bValid &&
        (InMapPackagePath.IsEmpty() ||
         Entry.Header.MapPackagePath.Equals(InMapPackagePath,
                                            ESearchCase::IgnoreCase)))
    {
      Result.Add(&Entry);
    }
  }
  return Result;
}

int32 FUIWTCheckpointIndex::NumInvalid() const
{
  int32 Count = 0;
  for (const FUIWTCheckpointIndexEntry &Entry : Entries)
  {
    Count += Entry.bValid ? 0 : 1;
  }
  return Count;
}
