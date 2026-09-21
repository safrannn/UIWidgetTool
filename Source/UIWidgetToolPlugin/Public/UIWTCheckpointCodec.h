#pragma once

#include "CoreMinimal.h"
#include "UIWTCheckpointTypes.h"

#define UIWT_CHECKPOINT_FORMAT_VERSION 2

#define UIWT_CHECKPOINT_MAGIC 0x54574955

#define UIWT_CHECKPOINT_PAYLOAD_EXT TEXT("lvlcp")
#define UIWT_CHECKPOINT_SIDECAR_EXT TEXT("json")

#define UIWT_CHECKPOINT_SNAPSHOT_EXT TEXT("widgetsnapshot")

#define UIWT_CHECKPOINT_PART_SUFFIX TEXT(".part")

enum class EUIWTCheckpointError : uint8 {
  None = 0,

  FileNotFound,
  FileTruncated,
  ReadFailed,
  BadMagic,
  UnsupportedVersion,
  BadHeaderLength,
  HeaderParseFailed,

  SidecarParseFailed,
  SidecarMismatch,
  MapMismatch,

  CompressedSizeLimit,
  UncompressedSizeLimit,

  DecompressionFailed,
  CrcMismatch,
  PayloadParseFailed,
  ActorCountMismatch,

  DirectoryCreateFailed,
  WriteFailed,
  CommitFailed,
};

struct UIWIDGETTOOLPLUGIN_API FUIWTCheckpointResult {
  EUIWTCheckpointError Error = EUIWTCheckpointError::None;
  FString Message;

  bool IsValid() const { return Error == EUIWTCheckpointError::None; }

  FString GetErrorCode() const;

  static FUIWTCheckpointResult Ok() { return FUIWTCheckpointResult(); }
  static FUIWTCheckpointResult Fail(EUIWTCheckpointError InError,
                                    FString InMessage);
};

struct UIWIDGETTOOLPLUGIN_API FUIWTCheckpointSidecar {
  int32 SchemaVersion = UIWT_CHECKPOINT_FORMAT_VERSION;

  FString PayloadFileName;

  FUIWTCheckpointHeader Header;
};

struct UIWIDGETTOOLPLUGIN_API FUIWTCheckpointLoadOptions {
  int64 MaxCompressedBytes = 0;
  int64 MaxUncompressedBytes = 0;

  FGuid ExpectedCheckpointId;
  FString ExpectedMapPackagePath;
};

struct UIWIDGETTOOLPLUGIN_API FUIWTRetentionReport {
  int32 Examined = 0;
  int32 Deleted = 0;
  int32 SkippedAssigned = 0;
  int32 FailedToDelete = 0;
};

namespace UIWTCheckpointCodec {
UIWIDGETTOOLPLUGIN_API bool IsSupportedVersion(int32 InVersion);

UIWIDGETTOOLPLUGIN_API FString
MakeBaseFileName(const FUIWTCheckpointHeader &InHeader);

UIWIDGETTOOLPLUGIN_API FString
MakeSnapshotPathFromSidecar(const FString &InSidecarPath);

UIWIDGETTOOLPLUGIN_API bool Save(const FString &InDirectory,
                                 FUIWTCheckpointHeader &InOutHeader,
                                 const FUIWTCheckpointPayload &InPayload,
                                 FString &OutPayloadPath,
                                 FString &OutSidecarPath,
                                 FUIWTCheckpointResult &OutResult);

UIWIDGETTOOLPLUGIN_API bool ReadSidecar(const FString &InSidecarPath,
                                        FUIWTCheckpointSidecar &OutSidecar,
                                        FUIWTCheckpointResult &OutResult);

UIWIDGETTOOLPLUGIN_API bool
SetCheckpointDisplayName(const FString &InSidecarPath,
                         const FString &InDisplayName,
                         FUIWTCheckpointResult &OutResult);

UIWIDGETTOOLPLUGIN_API bool
ValidateHeader(const FString &InPayloadPath,
               const FUIWTCheckpointLoadOptions &InOptions,
               FUIWTCheckpointHeader &OutHeader,
               FUIWTCheckpointResult &OutResult);

UIWIDGETTOOLPLUGIN_API bool Load(const FString &InPayloadPath,
                                 const FUIWTCheckpointLoadOptions &InOptions,
                                 FUIWTCheckpointHeader &OutHeader,
                                 FUIWTCheckpointPayload &OutPayload,
                                 FUIWTCheckpointResult &OutResult);

UIWIDGETTOOLPLUGIN_API TArray<FString>
FindSidecarFiles(const FString &InDirectory);

UIWIDGETTOOLPLUGIN_API FUIWTRetentionReport
Prune(const FString &InDirectory, const FString &InMapPackagePath,
      int32 InMaxPerMap, const TSet<FGuid> &InAssignedIds);

UIWIDGETTOOLPLUGIN_API bool DeleteCheckpointPair(const FString &InSidecarPath,
                                                 const FString &InPayloadPath);

}
