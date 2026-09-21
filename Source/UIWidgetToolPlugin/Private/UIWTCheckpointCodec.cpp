#include "UIWTCheckpointCodec.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Compression.h"
#include "Misc/Crc.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

DEFINE_LOG_CATEGORY(LogUIWidgetToolPlugin);

namespace
{
  constexpr int64 PreludeBytes = sizeof(int32) * 3;
  constexpr int64 CrcTrailerBytes = sizeof(uint32);

  constexpr int32 MaxHeaderBytes = 64 * 1024;

  void SerializeTransform(FArchive &Ar, FTransform &InOutTransform)
  {
    FQuat Rotation = InOutTransform.GetRotation();
    FVector Translation = InOutTransform.GetTranslation();
    FVector Scale3D = InOutTransform.GetScale3D();

    Ar << Rotation;
    Ar << Translation;
    Ar << Scale3D;

    if (Ar.IsLoading())
    {
      Rotation.Normalize();
      InOutTransform = FTransform(Rotation, Translation, Scale3D);
    }
  }

  void SerializeComponentRecord(FArchive &Ar, FUIWTComponentRecord &InOutRecord)
  {
    Ar << InOutRecord.ClassPath;
    SerializeTransform(Ar, InOutRecord.RelativeTransform);
    Ar << InOutRecord.PropertyData;
  }

  void SerializeActorRecord(FArchive &Ar, FUIWTActorRecord &InOutRecord)
  {
    Ar << InOutRecord.RecordId;
    Ar << InOutRecord.ActorInstanceGuid;
    Ar << InOutRecord.ActorPath;
    Ar << InOutRecord.ClassPath;
    Ar << InOutRecord.bRuntimeSpawned;
    Ar << InOutRecord.PlayerIndex;
    SerializeTransform(Ar, InOutRecord.Transform);
    Ar << InOutRecord.PropertyData;

    int32 ComponentCount = InOutRecord.Components.Num();
    Ar << ComponentCount;
    if (Ar.IsLoading())
    {
      if (ComponentCount < 0)
      {
        Ar.SetError();
        return;
      }
      InOutRecord.Components.Reset();
      for (int32 Index = 0; Index < ComponentCount; ++Index)
      {
        FName Key;
        Ar << Key;
        FUIWTComponentRecord Component;
        SerializeComponentRecord(Ar, Component);
        if (Ar.IsError())
        {
          return;
        }
        InOutRecord.Components.Add(Key, MoveTemp(Component));
      }
    }
    else
    {
      for (TPair<FName, FUIWTComponentRecord> &Pair : InOutRecord.Components)
      {
        Ar << Pair.Key;
        SerializeComponentRecord(Ar, Pair.Value);
      }
    }

    Ar << InOutRecord.CustomData;
  }

  void SerializeCustomSection(FArchive &Ar, FUIWTCustomSection &InOutSection)
  {
    Ar << InOutSection.SectionName;
    Ar << InOutSection.SchemaVersion;
    Ar << InOutSection.bRequired;
    Ar << InOutSection.Data;
  }

  void SerializePayload(FArchive &Ar, FUIWTCheckpointPayload &InOutPayload)
  {
    int32 ActorCount = InOutPayload.Actors.Num();
    Ar << ActorCount;
    if (Ar.IsLoading())
    {
      if (ActorCount < 0)
      {
        Ar.SetError();
        return;
      }
      InOutPayload.Actors.Empty(FMath::Min(ActorCount, 4096));
      InOutPayload.Actors.AddDefaulted(ActorCount);
    }
    for (int32 Index = 0; Index < ActorCount && !Ar.IsError(); ++Index)
    {
      SerializeActorRecord(Ar, InOutPayload.Actors[Index]);
    }

    int32 SectionCount = InOutPayload.CustomSections.Num();
    Ar << SectionCount;
    if (Ar.IsLoading())
    {
      if (SectionCount < 0)
      {
        Ar.SetError();
        return;
      }
      InOutPayload.CustomSections.Empty(FMath::Min(SectionCount, 256));
      InOutPayload.CustomSections.AddDefaulted(SectionCount);
    }
    for (int32 Index = 0; Index < SectionCount && !Ar.IsError(); ++Index)
    {
      SerializeCustomSection(Ar, InOutPayload.CustomSections[Index]);
    }
  }

  void SerializeHeaderFields(FArchive &Ar, FUIWTCheckpointHeader &InOutHeader)
  {
    Ar << InOutHeader.CheckpointId;
    Ar << InOutHeader.EngineVersion;
    Ar << InOutHeader.MapPackagePath;
    Ar << InOutHeader.LevelSignature;

    int64 CapturedTicks = InOutHeader.CapturedAtUtc.GetTicks();
    Ar << CapturedTicks;
    if (Ar.IsLoading())
    {
      InOutHeader.CapturedAtUtc = FDateTime(CapturedTicks);
    }

    Ar << InOutHeader.DisplayName;
    Ar << InOutHeader.WorldTimeSeconds;
    Ar << InOutHeader.ActorCount;
    Ar << InOutHeader.UncompressedPayloadSize;
    Ar << InOutHeader.CompressedPayloadSize;
    Ar << InOutHeader.PayloadCrc;
  }

  void DeleteQuietly(const FString &InPath)
  {
    IFileManager::Get().Delete(*InPath, false, true, true);
  }

  // Renames a finished ".part" file over its final path, replacing any existing
  // file. On failure the part file is removed so nothing half-written remains.
  bool CommitPartFile(const FString &InFinalPath, const FString &InPartPath)
  {
    if (IFileManager::Get().Move(*InFinalPath, *InPartPath, true, true, false,
                                 false))
    {
      return true;
    }
    DeleteQuietly(InPartPath);
    return false;
  }

  FString SanitizeForFileName(const FString &InText, int32 InMaxLength)
  {
    FString Result = InText;
    Result.ReplaceInline(TEXT("/"), TEXT("_"));
    Result.ReplaceInline(TEXT("\\"), TEXT("_"));
    Result.ReplaceInline(TEXT(":"), TEXT("_"));
    Result.ReplaceInline(TEXT(" "), TEXT("_"));
    Result = FPaths::MakeValidFileName(Result, TEXT('_'));
    if (Result.Len() > InMaxLength)
    {
      Result.LeftInline(InMaxLength);
    }
    return Result;
  }

}

FString FUIWTCheckpointResult::GetErrorCode() const
{
  // Indexed by EUIWTCheckpointError; keep in declaration order.
  static const TCHAR *const Names[] = {
      TEXT("Ok"),
      TEXT("FileNotFound"),
      TEXT("FileTruncated"),
      TEXT("ReadFailed"),
      TEXT("BadMagic"),
      TEXT("UnsupportedVersion"),
      TEXT("BadHeaderLength"),
      TEXT("HeaderParseFailed"),
      TEXT("SidecarParseFailed"),
      TEXT("SidecarMismatch"),
      TEXT("MapMismatch"),
      TEXT("CompressedSizeLimit"),
      TEXT("UncompressedSizeLimit"),
      TEXT("DecompressionFailed"),
      TEXT("CrcMismatch"),
      TEXT("PayloadParseFailed"),
      TEXT("ActorCountMismatch"),
      TEXT("DirectoryCreateFailed"),
      TEXT("WriteFailed"),
      TEXT("CommitFailed"),
  };
  static_assert(UE_ARRAY_COUNT(Names) ==
                    static_cast<int32>(EUIWTCheckpointError::CommitFailed) + 1,
                "Add the new EUIWTCheckpointError's name to this table.");

  const int32 Index = static_cast<int32>(Error);
  return Index >= 0 && Index < UE_ARRAY_COUNT(Names) ? Names[Index]
                                                     : TEXT("Unknown");
}

FUIWTCheckpointResult
FUIWTCheckpointResult::Fail(EUIWTCheckpointError InError, FString InMessage)
{
  FUIWTCheckpointResult Result;
  Result.Error = InError;
  Result.Message = MoveTemp(InMessage);
  return Result;
}

namespace UIWTCheckpointCodec
{

  bool IsSupportedVersion(int32 InVersion)
  {
    return InVersion == UIWT_CHECKPOINT_FORMAT_VERSION;
  }

  FString MakeBaseFileName(const FUIWTCheckpointHeader &InHeader)
  {
    FString MapName = FPackageName::GetShortName(InHeader.MapPackagePath);
    if (MapName.IsEmpty())
    {
      MapName = TEXT("UnknownMap");
    }

    const FString Timestamp =
        InHeader.CapturedAtUtc.ToString(TEXT("%Y%m%dT%H%M%SZ"));

    return FString::Printf(TEXT("%s_%s_%s"), *SanitizeForFileName(MapName, 64),
                           *Timestamp,
                           *InHeader.CheckpointId.ToString(EGuidFormats::Digits));
  }

  FString MakeSnapshotPathFromSidecar(const FString &InSidecarPath)
  {
    if (InSidecarPath.IsEmpty())
    {
      return FString();
    }
    return FPaths::Combine(
        FPaths::GetPath(InSidecarPath),
        FString::Printf(TEXT("%s.%s"), *FPaths::GetBaseFilename(InSidecarPath),
                        UIWT_CHECKPOINT_SNAPSHOT_EXT));
  }

  static bool WriteSidecarJson(const FString &InPath,
                               const FUIWTCheckpointSidecar &InSidecar,
                               FUIWTCheckpointResult &OutResult)
  {
    const FUIWTCheckpointHeader &Header = InSidecar.Header;

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("SchemaVersion"), InSidecar.SchemaVersion);
    Root->SetStringField(TEXT("PayloadFileName"), InSidecar.PayloadFileName);
    Root->SetStringField(TEXT("CheckpointId"),
                         Header.CheckpointId.ToString(EGuidFormats::Digits));
    Root->SetStringField(TEXT("EngineVersion"), Header.EngineVersion);
    Root->SetStringField(TEXT("MapPackagePath"), Header.MapPackagePath);
    Root->SetStringField(TEXT("LevelSignature"), Header.LevelSignature);
    Root->SetStringField(TEXT("CapturedAtUtc"), Header.CapturedAtUtc.ToIso8601());
    Root->SetStringField(TEXT("DisplayName"), Header.DisplayName);
    Root->SetNumberField(TEXT("WorldTimeSeconds"), Header.WorldTimeSeconds);
    Root->SetNumberField(TEXT("ActorCount"), Header.ActorCount);
    Root->SetNumberField(TEXT("UncompressedPayloadSize"),
                         static_cast<double>(Header.UncompressedPayloadSize));
    Root->SetNumberField(TEXT("CompressedPayloadSize"),
                         static_cast<double>(Header.CompressedPayloadSize));
    Root->SetNumberField(TEXT("PayloadCrc"),
                         static_cast<double>(Header.PayloadCrc));

    FString Json;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
    if (!FJsonSerializer::Serialize(Root, Writer))
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::WriteFailed,
          FString::Printf(TEXT("Failed to serialize sidecar JSON for '%s'."),
                          *InPath));
      return false;
    }

    if (!FFileHelper::SaveStringToFile(
            Json, *InPath, FFileHelper::EEncodingOptions::ForceUTF8))
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::WriteFailed,
          FString::Printf(TEXT("Failed to write sidecar '%s'."), *InPath));
      return false;
    }
    return true;
  }

  bool SetCheckpointDisplayName(const FString &InSidecarPath,
                                const FString &InDisplayName,
                                FUIWTCheckpointResult &OutResult)
  {
    FUIWTCheckpointSidecar Sidecar;
    if (!ReadSidecar(InSidecarPath, Sidecar, OutResult))
    {
      return false;
    }

    if (Sidecar.Header.DisplayName.Equals(InDisplayName, ESearchCase::CaseSensitive))
    {
      OutResult = FUIWTCheckpointResult::Ok();
      return true;
    }

    Sidecar.Header.DisplayName = InDisplayName;

    const FString PartPath = InSidecarPath + UIWT_CHECKPOINT_PART_SUFFIX;
    if (!WriteSidecarJson(PartPath, Sidecar, OutResult))
    {
      DeleteQuietly(PartPath);
      return false;
    }

    if (!CommitPartFile(InSidecarPath, PartPath))
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::CommitFailed,
          FString::Printf(TEXT("Could not commit renamed sidecar to '%s'."),
                          *InSidecarPath));
      return false;
    }

    OutResult = FUIWTCheckpointResult::Ok();
    UE_LOG(LogUIWidgetToolPlugin, Log, TEXT("Renamed checkpoint %s to '%s'."),
           *Sidecar.Header.CheckpointId.ToString(EGuidFormats::Digits),
           *InDisplayName);
    return true;
  }

  bool ReadSidecar(const FString &InSidecarPath,
                   FUIWTCheckpointSidecar &OutSidecar,
                   FUIWTCheckpointResult &OutResult)
  {
    FString Json;
    if (!FFileHelper::LoadFileToString(Json, *InSidecarPath))
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::FileNotFound,
          FString::Printf(TEXT("Sidecar '%s' could not be read."),
                          *InSidecarPath));
      return false;
    }

    TSharedPtr<FJsonObject> Root;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::SidecarParseFailed,
          FString::Printf(TEXT("Sidecar '%s' is not valid JSON."),
                          *InSidecarPath));
      return false;
    }

    FUIWTCheckpointSidecar Sidecar;
    // Missing numeric fields keep their defaults; the binary header is
    // authoritative for anything that matters to loading.
    auto ReadNumber = [&Root](const TCHAR *InField, auto &OutValue)
    {
      double Number = 0.0;
      if (Root->TryGetNumberField(InField, Number))
      {
        OutValue = static_cast<std::decay_t<decltype(OutValue)>>(Number);
      }
    };
    ReadNumber(TEXT("SchemaVersion"), Sidecar.SchemaVersion);
    Root->TryGetStringField(TEXT("PayloadFileName"), Sidecar.PayloadFileName);

    FString CheckpointIdText;
    if (!Root->TryGetStringField(TEXT("CheckpointId"), CheckpointIdText) ||
        !FGuid::Parse(CheckpointIdText, Sidecar.Header.CheckpointId) ||
        !Sidecar.Header.CheckpointId.IsValid())
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::SidecarParseFailed,
          FString::Printf(TEXT("Sidecar '%s' has no usable CheckpointId."),
                          *InSidecarPath));
      return false;
    }

    if (Sidecar.PayloadFileName.IsEmpty())
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::SidecarParseFailed,
          FString::Printf(TEXT("Sidecar '%s' names no payload file."),
                          *InSidecarPath));
      return false;
    }

    if (Sidecar.PayloadFileName.Contains(TEXT("/")) ||
        Sidecar.PayloadFileName.Contains(TEXT("\\")) ||
        Sidecar.PayloadFileName.Contains(TEXT("..")))
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::SidecarParseFailed,
          FString::Printf(TEXT("Sidecar '%s' names a payload path rather than a "
                               "file name."),
                          *InSidecarPath));
      return false;
    }

    Root->TryGetStringField(TEXT("EngineVersion"), Sidecar.Header.EngineVersion);
    Root->TryGetStringField(TEXT("MapPackagePath"),
                            Sidecar.Header.MapPackagePath);
    Root->TryGetStringField(TEXT("LevelSignature"),
                            Sidecar.Header.LevelSignature);
    Root->TryGetStringField(TEXT("DisplayName"), Sidecar.Header.DisplayName);

    FString CapturedText;
    if (Root->TryGetStringField(TEXT("CapturedAtUtc"), CapturedText))
    {
      FDateTime::ParseIso8601(*CapturedText, Sidecar.Header.CapturedAtUtc);
    }

    ReadNumber(TEXT("WorldTimeSeconds"), Sidecar.Header.WorldTimeSeconds);
    ReadNumber(TEXT("ActorCount"), Sidecar.Header.ActorCount);
    ReadNumber(TEXT("UncompressedPayloadSize"),
               Sidecar.Header.UncompressedPayloadSize);
    ReadNumber(TEXT("CompressedPayloadSize"),
               Sidecar.Header.CompressedPayloadSize);
    ReadNumber(TEXT("PayloadCrc"), Sidecar.Header.PayloadCrc);

    OutSidecar = MoveTemp(Sidecar);
    OutResult = FUIWTCheckpointResult::Ok();
    return true;
  }

  bool Save(const FString &InDirectory, FUIWTCheckpointHeader &InOutHeader,
            const FUIWTCheckpointPayload &InPayload, FString &OutPayloadPath,
            FString &OutSidecarPath, FUIWTCheckpointResult &OutResult)
  {
    if (!InOutHeader.CheckpointId.IsValid())
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::WriteFailed,
          TEXT("Refusing to save a checkpoint with an invalid CheckpointId."));
      return false;
    }

    IFileManager &FileManager = IFileManager::Get();
    if (!FileManager.DirectoryExists(*InDirectory) &&
        !FileManager.MakeDirectory(*InDirectory, true))
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::DirectoryCreateFailed,
          FString::Printf(TEXT("Could not create checkpoint directory '%s'."),
                          *InDirectory));
      return false;
    }

    TArray<uint8> Uncompressed;
    {
      FMemoryWriter Writer(Uncompressed, true);
      FUIWTCheckpointPayload MutablePayload = InPayload;
      SerializePayload(Writer, MutablePayload);
      if (Writer.IsError())
      {
        OutResult = FUIWTCheckpointResult::Fail(
            EUIWTCheckpointError::WriteFailed,
            TEXT("Failed to serialize the checkpoint payload."));
        return false;
      }
    }

    TArray<uint8> Compressed;
    {
      int32 CompressedSize =
          FCompression::CompressMemoryBound(NAME_Zlib, Uncompressed.Num());
      Compressed.SetNumUninitialized(CompressedSize);
      if (!FCompression::CompressMemory(NAME_Zlib, Compressed.GetData(),
                                        CompressedSize, Uncompressed.GetData(),
                                        Uncompressed.Num()))
      {
        OutResult = FUIWTCheckpointResult::Fail(
            EUIWTCheckpointError::WriteFailed,
            TEXT("Failed to compress the checkpoint payload."));
        return false;
      }
      Compressed.SetNum(CompressedSize, EAllowShrinking::No);
    }

    InOutHeader.ActorCount = InPayload.Actors.Num();
    InOutHeader.UncompressedPayloadSize = Uncompressed.Num();
    InOutHeader.CompressedPayloadSize = Compressed.Num();
    InOutHeader.PayloadCrc =
        FCrc::MemCrc32(Compressed.GetData(), Compressed.Num());
    if (InOutHeader.EngineVersion.IsEmpty())
    {
      InOutHeader.EngineVersion = FEngineVersion::Current().ToString();
    }

    TArray<uint8> HeaderBytes;
    {
      FMemoryWriter Writer(HeaderBytes, true);
      SerializeHeaderFields(Writer, InOutHeader);
      if (Writer.IsError() || HeaderBytes.Num() <= 0 ||
          HeaderBytes.Num() > MaxHeaderBytes)
      {
        OutResult = FUIWTCheckpointResult::Fail(
            EUIWTCheckpointError::WriteFailed,
            TEXT("Failed to encode the checkpoint header."));
        return false;
      }
    }

    const FString BaseName = MakeBaseFileName(InOutHeader);
    const FString PayloadFileName =
        FString::Printf(TEXT("%s.%s"), *BaseName, UIWT_CHECKPOINT_PAYLOAD_EXT);
    const FString SidecarFileName =
        FString::Printf(TEXT("%s.%s"), *BaseName, UIWT_CHECKPOINT_SIDECAR_EXT);

    const FString PayloadPath = FPaths::Combine(InDirectory, PayloadFileName);
    const FString SidecarPath = FPaths::Combine(InDirectory, SidecarFileName);
    const FString PayloadPartPath = PayloadPath + UIWT_CHECKPOINT_PART_SUFFIX;
    const FString SidecarPartPath = SidecarPath + UIWT_CHECKPOINT_PART_SUFFIX;

    {
      TUniquePtr<FArchive> Writer(FileManager.CreateFileWriter(*PayloadPartPath));
      if (!Writer)
      {
        OutResult = FUIWTCheckpointResult::Fail(
            EUIWTCheckpointError::WriteFailed,
            FString::Printf(TEXT("Could not open '%s' for writing."),
                            *PayloadPartPath));
        return false;
      }

      int32 Magic = UIWT_CHECKPOINT_MAGIC;
      int32 FormatVersion = UIWT_CHECKPOINT_FORMAT_VERSION;
      int32 HeaderByteLength = HeaderBytes.Num();
      *Writer << Magic;
      *Writer << FormatVersion;
      *Writer << HeaderByteLength;
      Writer->Serialize(HeaderBytes.GetData(), HeaderBytes.Num());
      Writer->Serialize(Compressed.GetData(), Compressed.Num());
      uint32 Crc = InOutHeader.PayloadCrc;
      *Writer << Crc;

      const bool bClosed = Writer->Close();
      const bool bErrored = Writer->IsError();
      Writer.Reset();
      if (!bClosed || bErrored)
      {
        DeleteQuietly(PayloadPartPath);
        OutResult = FUIWTCheckpointResult::Fail(
            EUIWTCheckpointError::WriteFailed,
            FString::Printf(TEXT("Failed while writing '%s'."),
                            *PayloadPartPath));
        return false;
      }
    }

    {
      FUIWTCheckpointLoadOptions Options;
      Options.ExpectedCheckpointId = InOutHeader.CheckpointId;
      Options.ExpectedMapPackagePath = InOutHeader.MapPackagePath;
      FUIWTCheckpointHeader Verified;
      FUIWTCheckpointResult VerifyResult;
      if (!ValidateHeader(PayloadPartPath, Options, Verified, VerifyResult))
      {
        DeleteQuietly(PayloadPartPath);
        OutResult = FUIWTCheckpointResult::Fail(
            EUIWTCheckpointError::CommitFailed,
            FString::Printf(TEXT("Wrote '%s' but it did not validate: %s"),
                            *PayloadPartPath, *VerifyResult.Message));
        return false;
      }
    }

    FUIWTCheckpointSidecar Sidecar;
    Sidecar.SchemaVersion = UIWT_CHECKPOINT_FORMAT_VERSION;
    Sidecar.PayloadFileName = PayloadFileName;
    Sidecar.Header = InOutHeader;
    if (!WriteSidecarJson(SidecarPartPath, Sidecar, OutResult))
    {
      DeleteQuietly(PayloadPartPath);
      DeleteQuietly(SidecarPartPath);
      return false;
    }

    if (!CommitPartFile(PayloadPath, PayloadPartPath))
    {
      DeleteQuietly(SidecarPartPath);
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::CommitFailed,
          FString::Printf(TEXT("Could not commit payload to '%s'."),
                          *PayloadPath));
      return false;
    }
    if (!CommitPartFile(SidecarPath, SidecarPartPath))
    {
      DeleteQuietly(PayloadPath);
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::CommitFailed,
          FString::Printf(TEXT("Could not commit sidecar to '%s'."),
                          *SidecarPath));
      return false;
    }

    OutPayloadPath = PayloadPath;
    OutSidecarPath = SidecarPath;
    OutResult = FUIWTCheckpointResult::Ok();

    UE_LOG(LogUIWidgetToolPlugin, Log,
           TEXT("Saved checkpoint %s for %s: %d actors, %lld -> %lld bytes."),
           *InOutHeader.CheckpointId.ToString(EGuidFormats::Digits),
           *InOutHeader.MapPackagePath, InOutHeader.ActorCount,
           InOutHeader.UncompressedPayloadSize,
           InOutHeader.CompressedPayloadSize);
    return true;
  }

  static TUniquePtr<FArchive>
  OpenAndValidateHeader(const FString &InPayloadPath,
                        const FUIWTCheckpointLoadOptions &InOptions,
                        FUIWTCheckpointHeader &OutHeader,
                        FUIWTCheckpointResult &OutResult)
  {
    TUniquePtr<FArchive> Reader(
        IFileManager::Get().CreateFileReader(*InPayloadPath));
    if (!Reader)
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::FileNotFound,
          FString::Printf(TEXT("Checkpoint payload '%s' could not be opened."),
                          *InPayloadPath));
      return nullptr;
    }

    const int64 TotalSize = Reader->TotalSize();
    if (TotalSize < PreludeBytes + CrcTrailerBytes)
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::FileTruncated,
          FString::Printf(
              TEXT("'%s' is %lld bytes, too small to be a checkpoint."),
              *InPayloadPath, TotalSize));
      return nullptr;
    }

    int32 Magic = 0;
    *Reader << Magic;
    if (Magic != UIWT_CHECKPOINT_MAGIC)
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::BadMagic,
          FString::Printf(TEXT("'%s' does not start with the checkpoint magic."),
                          *InPayloadPath));
      return nullptr;
    }

    int32 FormatVersion = 0;
    *Reader << FormatVersion;
    if (!IsSupportedVersion(FormatVersion))
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::UnsupportedVersion,
          FString::Printf(
              TEXT("'%s' is format version %d; this build supports %d."),
              *InPayloadPath, FormatVersion, UIWT_CHECKPOINT_FORMAT_VERSION));
      return nullptr;
    }

    int32 HeaderByteLength = 0;
    *Reader << HeaderByteLength;
    if (HeaderByteLength <= 0 || HeaderByteLength > MaxHeaderBytes ||
        PreludeBytes + HeaderByteLength + CrcTrailerBytes > TotalSize)
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::BadHeaderLength,
          FString::Printf(TEXT("'%s' declares a %d byte header, which does not "
                               "fit in %lld bytes."),
                          *InPayloadPath, HeaderByteLength, TotalSize));
      return nullptr;
    }

    TArray<uint8> HeaderBytes;
    HeaderBytes.SetNumUninitialized(HeaderByteLength);
    Reader->Serialize(HeaderBytes.GetData(), HeaderByteLength);
    if (Reader->IsError())
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::ReadFailed,
          FString::Printf(TEXT("Failed to read the header of '%s'."),
                          *InPayloadPath));
      return nullptr;
    }

    FUIWTCheckpointHeader Header;
    {
      FMemoryReader HeaderReader(HeaderBytes, true);
      SerializeHeaderFields(HeaderReader, Header);
      if (HeaderReader.IsError() || !Header.CheckpointId.IsValid())
      {
        OutResult = FUIWTCheckpointResult::Fail(
            EUIWTCheckpointError::HeaderParseFailed,
            FString::Printf(TEXT("The header of '%s' did not parse."),
                            *InPayloadPath));
        return nullptr;
      }
    }

    if (InOptions.ExpectedCheckpointId.IsValid() &&
        InOptions.ExpectedCheckpointId != Header.CheckpointId)
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::SidecarMismatch,
          FString::Printf(
              TEXT("'%s' carries checkpoint %s but the sidecar names %s."),
              *InPayloadPath,
              *Header.CheckpointId.ToString(EGuidFormats::Digits),
              *InOptions.ExpectedCheckpointId.ToString(EGuidFormats::Digits)));
      return nullptr;
    }

    if (!InOptions.ExpectedMapPackagePath.IsEmpty() &&
        !InOptions.ExpectedMapPackagePath.Equals(Header.MapPackagePath,
                                                 ESearchCase::IgnoreCase))
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::MapMismatch,
          FString::Printf(TEXT("'%s' was captured on '%s', not '%s'."),
                          *InPayloadPath, *Header.MapPackagePath,
                          *InOptions.ExpectedMapPackagePath));
      return nullptr;
    }

    const int64 ActualCompressedSize =
        TotalSize - PreludeBytes - HeaderByteLength - CrcTrailerBytes;
    if (Header.CompressedPayloadSize != ActualCompressedSize)
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::FileTruncated,
          FString::Printf(
              TEXT("'%s' declares %lld compressed bytes but holds %lld."),
              *InPayloadPath, Header.CompressedPayloadSize,
              ActualCompressedSize));
      return nullptr;
    }
    if (Header.CompressedPayloadSize < 0 ||
        Header.CompressedPayloadSize > MAX_int32)
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::CompressedSizeLimit,
          FString::Printf(
              TEXT("'%s' declares an unrepresentable compressed size (%lld)."),
              *InPayloadPath, Header.CompressedPayloadSize));
      return nullptr;
    }
    if (InOptions.MaxCompressedBytes > 0 &&
        Header.CompressedPayloadSize > InOptions.MaxCompressedBytes)
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::CompressedSizeLimit,
          FString::Printf(
              TEXT("'%s' is %lld compressed bytes, over the %lld byte limit."),
              *InPayloadPath, Header.CompressedPayloadSize,
              InOptions.MaxCompressedBytes));
      return nullptr;
    }
    if (Header.UncompressedPayloadSize < 0 ||
        Header.UncompressedPayloadSize > MAX_int32)
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::UncompressedSizeLimit,
          FString::Printf(
              TEXT("'%s' declares an unrepresentable uncompressed size (%lld)."),
              *InPayloadPath, Header.UncompressedPayloadSize));
      return nullptr;
    }
    if (InOptions.MaxUncompressedBytes > 0 &&
        Header.UncompressedPayloadSize > InOptions.MaxUncompressedBytes)
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::UncompressedSizeLimit,
          FString::Printf(TEXT("'%s' would decompress to %lld bytes, over the "
                               "%lld byte limit."),
                          *InPayloadPath, Header.UncompressedPayloadSize,
                          InOptions.MaxUncompressedBytes));
      return nullptr;
    }
    if (Header.ActorCount < 0)
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::HeaderParseFailed,
          FString::Printf(TEXT("'%s' declares a negative actor count."),
                          *InPayloadPath));
      return nullptr;
    }

    OutHeader = MoveTemp(Header);
    OutResult = FUIWTCheckpointResult::Ok();
    return Reader;
  }

  bool ValidateHeader(const FString &InPayloadPath,
                      const FUIWTCheckpointLoadOptions &InOptions,
                      FUIWTCheckpointHeader &OutHeader,
                      FUIWTCheckpointResult &OutResult)
  {
    TUniquePtr<FArchive> Reader =
        OpenAndValidateHeader(InPayloadPath, InOptions, OutHeader, OutResult);
    if (!Reader)
    {
      return false;
    }
    Reader->Close();
    return true;
  }

  bool Load(const FString &InPayloadPath,
            const FUIWTCheckpointLoadOptions &InOptions,
            FUIWTCheckpointHeader &OutHeader, FUIWTCheckpointPayload &OutPayload,
            FUIWTCheckpointResult &OutResult)
  {
    FUIWTCheckpointHeader Header;
    TUniquePtr<FArchive> Reader =
        OpenAndValidateHeader(InPayloadPath, InOptions, Header, OutResult);
    if (!Reader)
    {
      return false;
    }

    const int32 CompressedSize = static_cast<int32>(Header.CompressedPayloadSize);
    const int32 UncompressedSize =
        static_cast<int32>(Header.UncompressedPayloadSize);

    TArray<uint8> Compressed;
    Compressed.SetNumUninitialized(CompressedSize);
    Reader->Serialize(Compressed.GetData(), CompressedSize);

    uint32 StoredCrc = 0;
    *Reader << StoredCrc;
    const bool bReadError = Reader->IsError();
    Reader->Close();
    Reader.Reset();

    if (bReadError)
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::ReadFailed,
          FString::Printf(TEXT("Failed to read the payload of '%s'."),
                          *InPayloadPath));
      return false;
    }

    const uint32 ActualCrc =
        FCrc::MemCrc32(Compressed.GetData(), Compressed.Num());
    if (ActualCrc != StoredCrc || ActualCrc != Header.PayloadCrc)
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::CrcMismatch,
          FString::Printf(TEXT("'%s' failed its CRC check (computed %u, trailer "
                               "%u, header %u)."),
                          *InPayloadPath, ActualCrc, StoredCrc,
                          Header.PayloadCrc));
      return false;
    }

    TArray<uint8> Uncompressed;
    Uncompressed.SetNumUninitialized(UncompressedSize);
    if (!FCompression::UncompressMemory(NAME_Zlib, Uncompressed.GetData(),
                                        UncompressedSize, Compressed.GetData(),
                                        CompressedSize))
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::DecompressionFailed,
          FString::Printf(TEXT("'%s' did not decompress."), *InPayloadPath));
      return false;
    }

    FUIWTCheckpointPayload Payload;
    {
      FMemoryReader PayloadReader(Uncompressed, true);
      SerializePayload(PayloadReader, Payload);
      if (PayloadReader.IsError())
      {
        OutResult = FUIWTCheckpointResult::Fail(
            EUIWTCheckpointError::PayloadParseFailed,
            FString::Printf(TEXT("The payload of '%s' did not parse."),
                            *InPayloadPath));
        return false;
      }
      if (PayloadReader.Tell() != Uncompressed.Num())
      {
        OutResult = FUIWTCheckpointResult::Fail(
            EUIWTCheckpointError::PayloadParseFailed,
            FString::Printf(TEXT("The payload of '%s' has %lld trailing bytes."),
                            *InPayloadPath,
                            (int64)Uncompressed.Num() - PayloadReader.Tell()));
        return false;
      }
    }

    if (Payload.Actors.Num() != Header.ActorCount)
    {
      OutResult = FUIWTCheckpointResult::Fail(
          EUIWTCheckpointError::ActorCountMismatch,
          FString::Printf(TEXT("'%s' declares %d actors but contains %d."),
                          *InPayloadPath, Header.ActorCount,
                          Payload.Actors.Num()));
      return false;
    }

    OutHeader = MoveTemp(Header);
    OutPayload = MoveTemp(Payload);
    OutResult = FUIWTCheckpointResult::Ok();
    return true;
  }

  TArray<FString> FindSidecarFiles(const FString &InDirectory)
  {
    TArray<FString> Result;
    if (InDirectory.IsEmpty() ||
        !IFileManager::Get().DirectoryExists(*InDirectory))
    {
      return Result;
    }

    TArray<FString> FileNames;
    const FString Wildcard = FPaths::Combine(
        InDirectory, FString::Printf(TEXT("*.%s"), UIWT_CHECKPOINT_SIDECAR_EXT));
    IFileManager::Get().FindFiles(FileNames, *Wildcard, true,
                                  false);

    Result.Reserve(FileNames.Num());
    for (const FString &FileName : FileNames)
    {
      if (FileName.EndsWith(UIWT_CHECKPOINT_PART_SUFFIX))
      {
        continue;
      }
      Result.Add(FPaths::Combine(InDirectory, FileName));
    }
    return Result;
  }

  bool DeleteCheckpointPair(const FString &InSidecarPath,
                            const FString &InPayloadPath)
  {
    IFileManager &FileManager = IFileManager::Get();

    bool bOk = true;
    if (FileManager.FileExists(*InSidecarPath))
    {
      bOk &= FileManager.Delete(*InSidecarPath, false, true, true);
    }
    if (!InPayloadPath.IsEmpty() && FileManager.FileExists(*InPayloadPath))
    {
      bOk &= FileManager.Delete(*InPayloadPath, false, true, true);
    }

    const FString SnapshotPath = MakeSnapshotPathFromSidecar(InSidecarPath);
    if (!SnapshotPath.IsEmpty() && FileManager.FileExists(*SnapshotPath))
    {
      if (!FileManager.Delete(*SnapshotPath, false, true, true))
      {
        UE_LOG(LogUIWidgetToolPlugin, Warning,
               TEXT("Deleted checkpoint '%s' but could not delete its widget "
                    "snapshot '%s'."),
               *InSidecarPath, *SnapshotPath);
      }
    }
    return bOk;
  }

  FUIWTRetentionReport Prune(const FString &InDirectory,
                             const FString &InMapPackagePath, int32 InMaxPerMap,
                             const TSet<FGuid> &InAssignedIds)
  {
    FUIWTRetentionReport Report;
    if (InMaxPerMap <= 0)
    {
      return Report;
    }

    struct FCandidate
    {
      FString SidecarPath;
      FString PayloadPath;
      FGuid CheckpointId;
      FDateTime CapturedAtUtc;
    };
    TArray<FCandidate> Candidates;

    for (const FString &SidecarPath : FindSidecarFiles(InDirectory))
    {
      FUIWTCheckpointSidecar Sidecar;
      FUIWTCheckpointResult ReadResult;
      if (!ReadSidecar(SidecarPath, Sidecar, ReadResult))
      {
        continue;
      }
      if (!Sidecar.Header.MapPackagePath.Equals(InMapPackagePath,
                                                ESearchCase::IgnoreCase))
      {
        continue;
      }

      FCandidate Candidate;
      Candidate.SidecarPath = SidecarPath;
      Candidate.PayloadPath =
          FPaths::Combine(FPaths::GetPath(SidecarPath), Sidecar.PayloadFileName);
      Candidate.CheckpointId = Sidecar.Header.CheckpointId;
      Candidate.CapturedAtUtc = Sidecar.Header.CapturedAtUtc;
      Candidates.Add(MoveTemp(Candidate));
    }

    Report.Examined = Candidates.Num();
    if (Candidates.Num() <= InMaxPerMap)
    {
      return Report;
    }

    Candidates.Sort([](const FCandidate &A, const FCandidate &B)
                    { return A.CapturedAtUtc > B.CapturedAtUtc; });

    int32 Kept = 0;
    for (const FCandidate &Candidate : Candidates)
    {
      if (Kept < InMaxPerMap)
      {
        ++Kept;
        continue;
      }
      if (InAssignedIds.Contains(Candidate.CheckpointId))
      {
        ++Report.SkippedAssigned;
        continue;
      }
      if (DeleteCheckpointPair(Candidate.SidecarPath, Candidate.PayloadPath))
      {
        ++Report.Deleted;
      }
      else
      {
        ++Report.FailedToDelete;
      }
    }

    if (Report.Deleted > 0 || Report.FailedToDelete > 0)
    {
      UE_LOG(LogUIWidgetToolPlugin, Log,
             TEXT("Checkpoint retention on %s: examined %d, deleted %d, kept %d "
                  "assigned, %d failed."),
             *InMapPackagePath, Report.Examined, Report.Deleted,
             Report.SkippedAssigned, Report.FailedToDelete);
    }
    return Report;
  }

}
