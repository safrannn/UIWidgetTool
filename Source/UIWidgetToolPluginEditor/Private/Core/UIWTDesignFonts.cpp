#include "UIWTDesignFonts.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Core/UIWTDesignSettings.h"
#include "Core/UIWTJson.h"
#include "Core/UIWTRunImages.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Fonts/CompositeFont.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogUIWTDesignFonts, Log, All);

namespace
{
  using namespace UIWTDesignTree;
  using UIWTDesignFonts::FFontsResult;
  using UIWTDesignFonts::FInstalledFace;

  // Google's CSS API answers a client it doesn't know as a browser with
  // static TrueType links (one file per weight and slant, variable families
  // included); browsers get WOFF2, which UE can't import.
  const TCHAR *const GoogleCssUrl = TEXT("https://fonts.googleapis.com/css2");
  const TCHAR *const UserAgent = TEXT("UIWidgetTool");

  // "Semi Bold", "SemiBold" and "semi-bold" match; no style is Regular.
  FString StyleKey(const FString &InStyle)
  {
    FString Key = InStyle.Replace(TEXT(" "), TEXT("")).Replace(TEXT("-"), TEXT(""));
    Key.ToLowerInline();
    return Key.IsEmpty() ? FString(TEXT("regular")) : Key;
  }

  FString StyleName(const FFontRef &InFont)
  {
    const FString Style = InFont.Style.TrimStartAndEnd();
    return Style.IsEmpty() ? FString(TEXT("Regular")) : Style;
  }

  FString Label(const FFontRef &InFont)
  {
    return InFont.Family.TrimStartAndEnd() + TEXT(" ") + StyleName(InFont);
  }

  // Letters and digits only: "Open Sans" → OpenSans, for asset and file
  // names. InFallback when that leaves nothing.
  FString AssetSafe(const FString &InName, const TCHAR *InFallback)
  {
    FString Safe;
    for (const TCHAR Char : InName)
    {
      if (FChar::IsAlnum(Char) && Char < 128)
      {
        Safe.AppendChar(Char);
      }
    }
    return Safe.IsEmpty() ? FString(InFallback) : Safe;
  }

  void CollectRuns(const FNode &InNode, TArray<FFontRef> &OutFonts, TSet<FString> &InOutSeen)
  {
    if (InNode.Text.IsValid())
    {
      for (const FTextRun &Run : InNode.Text->Runs)
      {
        if (Run.Font.Family.TrimStartAndEnd().IsEmpty())
        {
          continue;
        }
        const FString Key = Run.Font.Family.TrimStartAndEnd().ToLower() + TEXT("|") +
                            StyleKey(Run.Font.Style);
        if (!InOutSeen.Contains(Key))
        {
          InOutSeen.Add(Key);
          OutFonts.Add(Run.Font);
        }
      }
    }
    for (const FNode &Child : InNode.Children)
    {
      CollectRuns(Child, OutFonts, InOutSeen);
    }
  }

  // The font map has the family in this style, or in every style: the
  // resolver uses that and never looks in the library.
  bool IsInFontMap(const FFontRef &InFont)
  {
    const FString Style = StyleKey(InFont.Style);
    for (const FUIWTFontMapping &Mapping : UUIWTDesignSettings::Get()->Fonts)
    {
      if (!Mapping.Font.IsNull() &&
          Mapping.Family.TrimStartAndEnd().Equals(InFont.Family.TrimStartAndEnd(),
                                                  ESearchCase::IgnoreCase) &&
          (Mapping.Style.TrimStartAndEnd().IsEmpty() || StyleKey(Mapping.Style) == Style))
      {
        return true;
      }
    }
    return false;
  }

  // ---------------------------------------------------------------------
  // Font files (the OpenType table directory and the few tables read here)

  constexpr uint32 MakeTag(char A, char B, char C, char D)
  {
    return (uint32(uint8(A)) << 24) | (uint32(uint8(B)) << 16) | (uint32(uint8(C)) << 8) |
           uint32(uint8(D));
  }

  uint16 U16(const uint8 *InData) { return uint16((InData[0] << 8) | InData[1]); }

  uint32 U32(const uint8 *InData)
  {
    return (uint32(InData[0]) << 24) | (uint32(InData[1]) << 16) | (uint32(InData[2]) << 8) |
           uint32(InData[3]);
  }

  bool IsFaceVersion(uint32 InVersion)
  {
    return InVersion == 0x00010000 || InVersion == MakeTag('t', 'r', 'u', 'e') ||
           InVersion == MakeTag('O', 'T', 'T', 'O');
  }

  // TrueType or OpenType, one face or a collection (not WOFF, not an HTML
  // error page).
  bool IsFontFile(const TArray<uint8> &InData)
  {
    if (InData.Num() < 12)
    {
      return false;
    }
    const uint32 Version = U32(InData.GetData());
    return IsFaceVersion(Version) || Version == MakeTag('t', 't', 'c', 'f');
  }

  // Bounded reads from a font file, without loading all of it (a CJK font
  // is tens of megabytes; the tables read here are a few kilobytes).
  class FFontFileReader
  {
  public:
    explicit FFontFileReader(FArchive &InArchive)
        : Archive(InArchive), Size(InArchive.TotalSize())
    {
    }

    bool Read(int64 InOffset, int64 InLength, TArray<uint8> &OutData)
    {
      constexpr int64 MaxRead = 1024 * 1024;
      if (InOffset < 0 || InLength <= 0 || InLength > MaxRead || InOffset + InLength > Size)
      {
        return false;
      }
      OutData.SetNumUninitialized(static_cast<int32>(InLength));
      Archive.Seek(InOffset);
      Archive.Serialize(OutData.GetData(), InLength);
      return !Archive.IsError();
    }

  private:
    FArchive &Archive;
    int64 Size;
  };

  // The name table's strings for the IDs read here, preferring Windows
  // English (US), then other Windows or Unicode entries, then Mac Roman.
  void ReadNames(const TArray<uint8> &InTable, FString (&OutNames)[18])
  {
    if (InTable.Num() < 6)
    {
      return;
    }
    int32 Scores[18] = {};
    const int32 Count = U16(&InTable[2]);
    const int32 Strings = U16(&InTable[4]);
    for (int32 Record = 0; Record < Count; ++Record)
    {
      const int32 At = 6 + Record * 12;
      if (At + 12 > InTable.Num())
      {
        break;
      }
      const uint8 *Entry = &InTable[At];
      const uint16 Platform = U16(Entry);
      const uint16 Encoding = U16(Entry + 2);
      const uint16 Language = U16(Entry + 4);
      const uint16 Id = U16(Entry + 6);
      const int32 Length = U16(Entry + 8);
      const int32 Start = Strings + U16(Entry + 10);
      if (Id >= 18 || (Id != 1 && Id != 2 && Id != 6 && Id != 16 && Id != 17))
      {
        continue;
      }
      const int32 Score = Platform == 3 && (Encoding == 1 || Encoding == 10)
                              ? (Language == 0x0409 ? 4 : 3)
                          : Platform == 0                    ? 2
                          : Platform == 1 && Encoding == 0   ? 1
                                                             : 0;
      if (Score <= Scores[Id] || Start + Length > InTable.Num())
      {
        continue;
      }
      FString Text;
      if (Platform == 1)
      {
        for (int32 Index = 0; Index < Length; ++Index)
        {
          Text.AppendChar(TCHAR(InTable[Start + Index]));
        }
      }
      else
      {
        // UTF-16, big-endian.
        for (int32 Index = 0; Index + 1 < Length; Index += 2)
        {
          Text.AppendChar(TCHAR(U16(&InTable[Start + Index])));
        }
      }
      OutNames[Id] = Text.TrimStartAndEnd();
      Scores[Id] = Score;
    }
  }

  // One face whose table directory starts at InOffset.
  void ReadFace(FFontFileReader &InReader, int64 InOffset, int32 InIndex, const FString &InFile,
                TArray<FInstalledFace> &OutFaces)
  {
    TArray<uint8> Header;
    if (!InReader.Read(InOffset, 12, Header) || !IsFaceVersion(U32(Header.GetData())))
    {
      return;
    }
    const int32 NumTables = U16(&Header[4]);
    TArray<uint8> Directory;
    if (!InReader.Read(InOffset + 12, NumTables * 16, Directory))
    {
      return;
    }
    FInstalledFace Face;
    Face.File = InFile;
    Face.Index = InIndex;
    TArray<uint8> Names;
    for (int32 Table = 0; Table < NumTables; ++Table)
    {
      const uint8 *Entry = &Directory[Table * 16];
      const uint32 Tag = U32(Entry);
      const uint32 Offset = U32(Entry + 8);
      const uint32 Length = U32(Entry + 12);
      TArray<uint8> Data;
      if (Tag == MakeTag('n', 'a', 'm', 'e'))
      {
        InReader.Read(Offset, Length, Names);
      }
      else if (Tag == MakeTag('O', 'S', '/', '2') && InReader.Read(Offset, 10, Data))
      {
        Face.FsType = U16(&Data[8]);
      }
      else if (Tag == MakeTag('f', 'v', 'a', 'r'))
      {
        Face.bVariable = true;
      }
    }
    FString Strings[18];
    ReadNames(Names, Strings);
    Face.LegacyFamily = Strings[1];
    Face.LegacyStyle = Strings[2];
    Face.PostScript = Strings[6];
    Face.Family = Strings[16].IsEmpty() ? Strings[1] : Strings[16];
    Face.Style = Strings[17].IsEmpty() ? Strings[2] : Strings[17];
    if (!Face.Family.IsEmpty())
    {
      OutFaces.Add(MoveTemp(Face));
    }
  }

  // "Inter" + "Semi Bold" and "Inter SemiBold" + "Regular" match.
  FString NameKey(const FString &InName)
  {
    FString Key = InName.Replace(TEXT(" "), TEXT(""))
                      .Replace(TEXT("-"), TEXT(""))
                      .Replace(TEXT("_"), TEXT(""));
    Key.ToLowerInline();
    return Key.Replace(TEXT("regular"), TEXT(""));
  }

  // The installed faces by file, kept between imports: a file is read again
  // only when its time stamp changes. Scans run on a worker thread.
  struct FInstalledCache
  {
    FCriticalSection Lock;
    TMap<FString, TPair<FDateTime, TArray<FInstalledFace>>> Files;
  };

  FInstalledCache &GetInstalledCache()
  {
    static FInstalledCache Cache;
    return Cache;
  }

  TArray<FInstalledFace> ScanInstalled()
  {
    FInstalledCache &Cache = GetInstalledCache();
    FScopeLock Lock(&Cache.Lock);
    IFileManager &Files = IFileManager::Get();
    TMap<FString, TPair<FDateTime, TArray<FInstalledFace>>> Scanned;
    TArray<FInstalledFace> Faces;
    for (const FString &Folder : UIWTDesignFonts::GetInstalledFontFolders())
    {
      TArray<FString> Paths;
      Files.FindFilesRecursive(Paths, *Folder, TEXT("*"), true, false);
      for (const FString &Path : Paths)
      {
        const FString Extension = FPaths::GetExtension(Path).ToLower();
        if (Extension != TEXT("ttf") && Extension != TEXT("otf") && Extension != TEXT("ttc") &&
            Extension != TEXT("otc"))
        {
          continue;
        }
        const FDateTime Stamp = Files.GetTimeStamp(*Path);
        TPair<FDateTime, TArray<FInstalledFace>> *Known = Cache.Files.Find(Path);
        TPair<FDateTime, TArray<FInstalledFace>> Entry =
            Known && Known->Key == Stamp ? MoveTemp(*Known)
                                         : MakeTuple(Stamp, UIWTDesignFonts::ReadFontFile(Path));
        Faces.Append(Entry.Value);
        Scanned.Add(Path, MoveTemp(Entry));
      }
    }
    Cache.Files = MoveTemp(Scanned);
    return Faces;
  }

  // The first url(...) of Google's CSS: the one @font-face it returns for
  // one weight and slant.
  FString FirstFontUrl(const FString &InCss)
  {
    const int32 Start = InCss.Find(TEXT("url("));
    if (Start == INDEX_NONE)
    {
      return FString();
    }
    const int32 End = InCss.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromStart,
                                 Start);
    if (End == INDEX_NONE)
    {
      return FString();
    }
    FString Url = InCss.Mid(Start + 4, End - Start - 4).TrimStartAndEnd().TrimQuotes();
    return Url.StartsWith(TEXT("https://")) ? Url : FString();
  }

  using FOnResponse =
      TFunction<void(int32 InCode, const TArray<uint8> &InBody, const FString &InError)>;

  // A GET whose completion runs on the game thread.
  void Get(const FString &InUrl, FOnResponse InOnDone)
  {
    const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request =
        FHttpModule::Get().CreateRequest();
    Request->SetURL(InUrl);
    Request->SetVerb(TEXT("GET"));
    Request->SetHeader(TEXT("User-Agent"), UserAgent);
    Request->SetTimeout(static_cast<float>(UUIWTDesignSettings::Get()->RequestTimeoutSeconds));
    Request->OnProcessRequestComplete().BindLambda(
        [OnDone = MoveTemp(InOnDone)](FHttpRequestPtr, FHttpResponsePtr InResponse,
                                      bool bInConnected)
        {
          static const TArray<uint8> NoBody;
          if (!bInConnected || !InResponse.IsValid())
          {
            OnDone(0, NoBody, TEXT("the request failed (no connection, or it timed out)"));
            return;
          }
          const int32 Code = InResponse->GetResponseCode();
          OnDone(Code, InResponse->GetContent(),
                 Code >= 200 && Code < 300 ? FString()
                                           : FString::Printf(TEXT("HTTP %d"), Code));
        });
    Request->ProcessRequest();
  }

  // Imports InData as the Font Face <Family>-<Style> and adds it to the
  // family's library Font (created when the library has none) as a typeface
  // named like the style, using face InSubFace of a collection. Both are
  // saved.
  bool AddToLibrary(const FFontRef &InFont, TArray<uint8> &&InData, const FString &InFile,
                    int32 InSubFace, FFontsResult &InOutResult, FString &OutError)
  {
    const FString Style = StyleName(InFont);
    const FString Safe = AssetSafe(InFont.Family, TEXT("Font"));
    // Again: another import may have added it while this one downloaded.
    UFont *Font = UIWTDesignFonts::FindLibraryFont(InFont.Family);
    if (Font && !UIWTDesignFonts::FindTypeface(Font, Style).IsNone())
    {
      return true;
    }
    const FString Folder =
        Font ? FPackageName::GetLongPackagePath(Font->GetOutermost()->GetName())
             : UUIWTDesignSettings::Get()->GetFontLibraryFolder() / Safe;

    const FString FaceName = Safe + TEXT("-") + AssetSafe(Style, TEXT("Regular"));
    const FString FacePackageName = Folder / FaceName;
    UFontFace *Face = LoadObject<UFontFace>(nullptr, *(FacePackageName + TEXT(".") + FaceName),
                                            nullptr, LOAD_NoWarn | LOAD_Quiet);
    if (!Face)
    {
      if (FPackageName::DoesPackageExist(FacePackageName))
      {
        OutError = FacePackageName + TEXT(" is taken by another asset");
        return false;
      }
      UPackage *Package = CreatePackage(*FacePackageName);
      Face = NewObject<UFontFace>(Package, *FaceName,
                                  RF_Public | RF_Standalone | RF_Transactional);
      Face->SourceFilename = InFile;
      Face->FontFaceData->SetData(MoveTemp(InData));
      Face->CacheSubFaces();
      FAssetRegistryModule::AssetCreated(Face);
      Face->MarkPackageDirty();
      if (!UIWTRunImages::SaveAsset(Face, OutError))
      {
        return false;
      }
      InOutResult.Assets.Add(Face->GetPathName());
    }

    if (!Font)
    {
      const FString FontPackageName = Folder / Safe;
      if (FPackageName::DoesPackageExist(FontPackageName))
      {
        OutError = FontPackageName + TEXT(" is taken by another asset");
        return false;
      }
      UPackage *Package = CreatePackage(*FontPackageName);
      Font = NewObject<UFont>(Package, *Safe, RF_Public | RF_Standalone | RF_Transactional);
      Font->FontCacheType = EFontCacheType::Runtime;
      FAssetRegistryModule::AssetCreated(Font);
    }
    Font->Modify();
    FTypefaceEntry &Entry =
        Font->GetMutableInternalCompositeFont().DefaultTypeface.Fonts.AddDefaulted_GetRef();
    Entry.Name = FName(*Style);
    Entry.Font = FFontData(Face, InSubFace);
    // Flushes Slate's font cache, which holds the typeface list.
    Font->PostEditChange();
    Font->MarkPackageDirty();
    if (!UIWTRunImages::SaveAsset(Font, OutError))
    {
      return false;
    }
    InOutResult.Assets.AddUnique(Font->GetPathName());
    return true;
  }

  // One EnsureFonts call; kept alive by its own callbacks.
  class FFontsJob : public TSharedFromThis<FFontsJob>
  {
  public:
    FFontsJob(FString InDesignFile, UIWTDesignFonts::FOnFontsReady InOnDone)
        : DesignFile(MoveTemp(InDesignFile)), OnDone(MoveTemp(InOnDone))
    {
    }

    void Run()
    {
      FString Json;
      FDocument Document;
      TArray<FString> Errors;
      if (!FFileHelper::LoadFileToString(Json, *DesignFile) ||
          !ReadDocument(Json, Document, Errors))
      {
        // The import reads it again and reports why.
        Finish();
        return;
      }
      for (const FFontRef &Font : UIWTDesignFonts::CollectFonts(Document))
      {
        if (IsInFontMap(Font))
        {
          Result.Mapped.Add(Label(Font));
          continue;
        }
        const UFont *Library = UIWTDesignFonts::FindLibraryFont(Font.Family);
        if (Library && !UIWTDesignFonts::FindTypeface(Library, StyleName(Font)).IsNone())
        {
          Result.InLibrary.Add(Label(Font));
          continue;
        }
        Missing.Add(Font);
      }
      if (Missing.IsEmpty())
      {
        Finish();
        return;
      }
      if (!UUIWTDesignSettings::Get()->bUseInstalledFonts)
      {
        Resolve(nullptr);
        return;
      }
      // Reading every installed font's names takes a moment the first time;
      // later scans only read new or changed files.
      Async(EAsyncExecution::ThreadPool,
            [Job = AsShared()]
            {
              const TSharedRef<const TArray<FInstalledFace>> Faces =
                  MakeShared<const TArray<FInstalledFace>>(ScanInstalled());
              AsyncTask(ENamedThreads::GameThread, [Job, Faces] { Job->Resolve(&Faces.Get()); });
            });
    }

  private:
    // Each missing font from the installed ones (InFaces, null when that is
    // off), else from Google Fonts.
    void Resolve(const TArray<FInstalledFace> *InFaces)
    {
      const UUIWTDesignSettings *Settings = UUIWTDesignSettings::Get();
      for (const FFontRef &Font : Missing)
      {
        // Why the installed fonts didn't have it, for the failure message.
        FString Installed;
        if (InFaces)
        {
          FString WhyNot;
          if (const FInstalledFace *Face =
                  UIWTDesignFonts::FindInstalledFace(*InFaces, Font, WhyNot))
          {
            TArray<uint8> Data;
            if (FFileHelper::LoadFileToArray(Data, *Face->File, FILEREAD_Silent) &&
                IsFontFile(Data))
            {
              Add(Font, MoveTemp(Data), Face->File, Face->Index, Result.Installed,
                  FString::Printf(TEXT(" (%s)"), *FPaths::ConvertRelativePathToFull(Face->File)));
              continue;
            }
            WhyNot = Face->File + TEXT(" can't be read");
          }
          Installed = WhyNot.IsEmpty() ? FString(TEXT("not installed on this computer")) : WhyNot;
        }
        if (!Settings->bDownloadGoogleFonts)
        {
          Record(Font, Installed,
                 InFaces ? TEXT("Google Fonts downloads are off")
                         : TEXT("not in the font map or the font library, and Google Fonts "
                                "downloads are off"));
          continue;
        }
        int32 Weight = 400;
        bool bItalic = false;
        if (!UIWTDesignFonts::ParseStyle(Font.Style, Weight, bItalic))
        {
          Record(Font, Installed,
                 TEXT("Google Fonts can only be asked for a weight and italic, not this style"));
          continue;
        }
        Download(Font, Weight, bItalic, Installed);
      }
      if (Pending == 0)
      {
        Finish();
      }
    }

    // The cached file, else Google's CSS for the one style, then the file
    // it links to. InInstalled goes into the failure message.
    void Download(const FFontRef &InFont, int32 InWeight, bool bInItalic,
                  const FString &InInstalled)
    {
      const FString Safe = AssetSafe(InFont.Family, TEXT("Font"));
      const FString File = FPaths::ConvertRelativePathToFull(
          FPaths::ProjectSavedDir() / TEXT("UIWidgetTool/Fonts") / Safe /
          (Safe + TEXT("-") + AssetSafe(StyleName(InFont), TEXT("Regular")) + TEXT(".ttf")));
      TArray<uint8> Cached;
      if (FFileHelper::LoadFileToArray(Cached, *File, FILEREAD_Silent) && IsFontFile(Cached))
      {
        Add(InFont, MoveTemp(Cached), File, 0, Result.Downloaded, FString());
        return;
      }
      ++Pending;
      const FString Url = FString::Printf(TEXT("%s?family=%s:ital,wght@%d,%d"), GoogleCssUrl,
                                          *FGenericPlatformHttp::UrlEncode(
                                              InFont.Family.TrimStartAndEnd()),
                                          bInItalic ? 1 : 0, InWeight);
      Get(Url,
          [Job = AsShared(), InFont, File, InInstalled](int32 InCode, const TArray<uint8> &InBody,
                                           const FString &InError)
          {
            if (InCode == 400)
            {
              Job->Fail(InFont, InInstalled, TEXT("not on Google Fonts in this weight and slant"));
              return;
            }
            if (!InError.IsEmpty())
            {
              Job->Fail(InFont, InInstalled, TEXT("Google Fonts: ") + InError);
              return;
            }
            const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR *>(InBody.GetData()),
                                         InBody.Num());
            const FString FileUrl = FirstFontUrl(FString(Converted.Length(), Converted.Get()));
            if (FileUrl.IsEmpty())
            {
              Job->Fail(InFont, InInstalled, TEXT("Google Fonts sent no font file link"));
              return;
            }
            Get(FileUrl, [Job, InFont, File, InInstalled](int32, const TArray<uint8> &InData,
                                             const FString &InFileError)
                {
                  if (!InFileError.IsEmpty() || !IsFontFile(InData))
                  {
                    Job->Fail(InFont, InInstalled,
                              InFileError.IsEmpty()
                                          ? FString(TEXT("the download isn't a TrueType file"))
                                          : TEXT("download: ") + InFileError);
                    return;
                  }
                  // Kept so the asset can be made again offline, and as the
                  // Font Face's source file.
                  FFileHelper::SaveArrayToFile(InData, *File);
                  TArray<uint8> Data = InData;
                  Job->Add(InFont, MoveTemp(Data), File, 0, Job->Result.Downloaded, FString());
                  Job->Done();
                });
          });
    }

    // Adds the font to the library; lists it in OutList (Installed or
    // Downloaded) with InSuffix, or in Failed.
    void Add(const FFontRef &InFont, TArray<uint8> &&InData, const FString &InFile,
             int32 InSubFace, TArray<FString> &OutList, const FString &InSuffix)
    {
      FString Error;
      if (AddToLibrary(InFont, MoveTemp(InData), InFile, InSubFace, Result, Error))
      {
        OutList.Add(Label(InFont) + InSuffix);
      }
      else
      {
        Result.Failed.Add(Label(InFont) + TEXT(": ") + Error);
      }
    }

    // InInstalled (why this computer didn't have it, empty when that wasn't
    // looked at) and InWhy.
    void Record(const FFontRef &InFont, const FString &InInstalled, const FString &InWhy)
    {
      Result.Failed.Add(Label(InFont) + TEXT(": ") +
                        (InInstalled.IsEmpty() ? InWhy : InInstalled + TEXT("; ") + InWhy));
    }

    // Record, for a download that ended.
    void Fail(const FFontRef &InFont, const FString &InInstalled, const FString &InWhy)
    {
      Record(InFont, InInstalled, InWhy);
      Done();
    }

    void Done()
    {
      if (--Pending == 0)
      {
        Finish();
      }
    }

    void Finish()
    {
      if (!Result.Installed.IsEmpty() || !Result.Downloaded.IsEmpty() ||
          !Result.Failed.IsEmpty())
      {
        UE_LOG(LogUIWTDesignFonts, Log, TEXT("Design fonts: %s"),
               *UIWTDesignFonts::ResultToJson(Result));
      }
      OnDone(Result);
    }

    FString DesignFile;
    UIWTDesignFonts::FOnFontsReady OnDone;
    FFontsResult Result;
    // Fonts in neither the font map nor the library.
    TArray<FFontRef> Missing;
    int32 Pending = 0;
  };

  TSharedRef<FJsonValue> Strings(const TArray<FString> &InValues)
  {
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FString &Value : InValues)
    {
      Values.Add(MakeShared<FJsonValueString>(Value));
    }
    return MakeShared<FJsonValueArray>(Values);
  }
}

TArray<FFontRef> UIWTDesignFonts::CollectFonts(const FDocument &InDocument)
{
  TArray<FFontRef> Fonts;
  TSet<FString> Seen;
  CollectRuns(InDocument.Root, Fonts, Seen);
  for (const TPair<FString, FComponentDef> &Component : InDocument.Components)
  {
    CollectRuns(Component.Value.Root, Fonts, Seen);
  }
  return Fonts;
}

UFont *UIWTDesignFonts::FindLibraryFont(const FString &InFamily)
{
  const FString Safe = AssetSafe(InFamily, TEXT(""));
  if (Safe.IsEmpty())
  {
    return nullptr;
  }
  const FString Folder = UUIWTDesignSettings::Get()->GetFontLibraryFolder();
  const FString Conventional = Folder / Safe / Safe;
  if (UFont *Font = FindObject<UFont>(nullptr, *(Conventional + TEXT(".") + Safe)))
  {
    return Font;
  }
  FARFilter Filter;
  Filter.PackagePaths.Add(FName(*Folder));
  Filter.bRecursivePaths = true;
  Filter.ClassPaths.Add(UFont::StaticClass()->GetClassPathName());
  TArray<FAssetData> Assets;
  IAssetRegistry::GetChecked().GetAssets(Filter, Assets);
  const FString Key = Safe.ToLower();
  const FAssetData *Exact = nullptr;
  const FAssetData *Affixed = nullptr;
  for (const FAssetData &Asset : Assets)
  {
    const FString Name = AssetSafe(Asset.AssetName.ToString(), TEXT("")).ToLower();
    if (Name == Key)
    {
      if (!Exact || Asset.PackageName.ToString() == Conventional)
      {
        Exact = &Asset;
      }
    }
    else if (!Affixed && (Name == TEXT("font") + Key || Name == Key + TEXT("font")))
    {
      Affixed = &Asset;
    }
  }
  const FAssetData *Found = Exact ? Exact : Affixed;
  return Found ? Cast<UFont>(Found->GetAsset()) : nullptr;
}

FName UIWTDesignFonts::FindTypeface(const UFont *InFont, const FString &InStyle)
{
  const FCompositeFont *Composite = InFont ? InFont->GetCompositeFont() : nullptr;
  if (!Composite)
  {
    return NAME_None;
  }
  const FString Wanted = StyleKey(InStyle);
  for (const FTypefaceEntry &Face : Composite->DefaultTypeface.Fonts)
  {
    if (StyleKey(Face.Name.ToString()) == Wanted)
    {
      return Face.Name;
    }
  }
  return NAME_None;
}

bool UIWTDesignFonts::ParseStyle(const FString &InStyle, int32 &OutWeight, bool &bOutItalic)
{
  FString Key = StyleKey(InStyle);
  bOutItalic = false;
  for (const TCHAR *Slant : {TEXT("italic"), TEXT("oblique")})
  {
    if (Key.Contains(Slant))
    {
      bOutItalic = true;
      Key.ReplaceInline(Slant, TEXT(""));
    }
  }
  struct FWeightName
  {
    const TCHAR *Name;
    int32 Weight;
  };
  static const FWeightName Weights[] = {
      {TEXT(""), 400},           {TEXT("regular"), 400},    {TEXT("normal"), 400},
      {TEXT("book"), 400},       {TEXT("thin"), 100},       {TEXT("hairline"), 100},
      {TEXT("extralight"), 200}, {TEXT("ultralight"), 200}, {TEXT("light"), 300},
      {TEXT("medium"), 500},     {TEXT("semibold"), 600},   {TEXT("demibold"), 600},
      {TEXT("bold"), 700},       {TEXT("extrabold"), 800},  {TEXT("ultrabold"), 800},
      {TEXT("black"), 900},      {TEXT("heavy"), 900}};
  for (const FWeightName &Entry : Weights)
  {
    if (Key == Entry.Name)
    {
      OutWeight = Entry.Weight;
      return true;
    }
  }
  // "700", "300 Italic"
  if (Key.IsNumeric())
  {
    OutWeight = FCString::Atoi(*Key);
    return OutWeight >= 1 && OutWeight <= 1000;
  }
  return false;
}

TArray<FInstalledFace> UIWTDesignFonts::ReadFontFile(const FString &InFile)
{
  TArray<FInstalledFace> Faces;
  const TUniquePtr<FArchive> Archive(IFileManager::Get().CreateFileReader(*InFile, FILEREAD_Silent));
  if (!Archive)
  {
    return Faces;
  }
  FFontFileReader Reader(*Archive);
  TArray<uint8> Header;
  if (!Reader.Read(0, 12, Header))
  {
    return Faces;
  }
  if (U32(Header.GetData()) != MakeTag('t', 't', 'c', 'f'))
  {
    ReadFace(Reader, 0, 0, InFile, Faces);
    return Faces;
  }
  // A collection: its faces' table directories.
  const int32 Count = static_cast<int32>(FMath::Min<uint32>(U32(&Header[8]), 256));
  TArray<uint8> Offsets;
  if (Count > 0 && Reader.Read(12, Count * 4, Offsets))
  {
    for (int32 Index = 0; Index < Count; ++Index)
    {
      ReadFace(Reader, U32(&Offsets[Index * 4]), Index, InFile, Faces);
    }
  }
  return Faces;
}

TArray<FString> UIWTDesignFonts::GetInstalledFontFolders()
{
  TArray<FString> Folders;
#if PLATFORM_WINDOWS
  const FString Windows = FPlatformMisc::GetEnvironmentVariable(TEXT("WINDIR"));
  if (!Windows.IsEmpty())
  {
    Folders.Add(Windows / TEXT("Fonts"));
  }
  const FString Local = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
  if (!Local.IsEmpty())
  {
    Folders.Add(Local / TEXT("Microsoft/Windows/Fonts"));
  }
#elif PLATFORM_MAC
  Folders.Add(TEXT("/Library/Fonts"));
  Folders.Add(TEXT("/System/Library/Fonts"));
  Folders.Add(FString(FPlatformProcess::UserHomeDir()) / TEXT("Library/Fonts"));
#elif PLATFORM_LINUX
  Folders.Add(TEXT("/usr/share/fonts"));
  Folders.Add(TEXT("/usr/local/share/fonts"));
  Folders.Add(FString(FPlatformProcess::UserHomeDir()) / TEXT(".local/share/fonts"));
#endif
  return Folders;
}

const FInstalledFace *UIWTDesignFonts::FindInstalledFace(const TArray<FInstalledFace> &InFaces,
                                                         const FFontRef &InFont,
                                                         FString &OutWhyNot)
{
  const FString Family = InFont.Family.TrimStartAndEnd();
  const FString Style = StyleKey(InFont.Style);
  const FString Whole = NameKey(Family + InFont.Style);
  const FString PostScript = InFont.PostScript.TrimStartAndEnd();
  const FInstalledFace *Skipped = nullptr;
  // The PostScript name names one face exactly; family and style names
  // come second.
  for (int32 Pass = 0; Pass < 2; ++Pass)
  {
    for (const FInstalledFace &Face : InFaces)
    {
      const bool bMatch =
          Pass == 0 ? !PostScript.IsEmpty() && Face.PostScript.Equals(PostScript, ESearchCase::IgnoreCase)
                    : (Face.Family.Equals(Family, ESearchCase::IgnoreCase) &&
                       StyleKey(Face.Style) == Style) ||
                          NameKey(Face.LegacyFamily + Face.LegacyStyle) == Whole;
      if (!bMatch)
      {
        continue;
      }
      if (Face.bVariable || Face.IsRestricted())
      {
        Skipped = Skipped ? Skipped : &Face;
        continue;
      }
      return &Face;
    }
  }
  if (!Skipped)
  {
    // A variable font names only its default style (Regular), so another
    // weight of it doesn't match above.
    Skipped = InFaces.FindByPredicate(
        [&Family](const FInstalledFace &Face)
        {
          return Face.bVariable && (Face.Family.Equals(Family, ESearchCase::IgnoreCase) ||
                                    Face.LegacyFamily.Equals(Family, ESearchCase::IgnoreCase));
        });
  }
  if (Skipped)
  {
    OutWhyNot = FString::Printf(
        TEXT("the installed %s is %s"), *FPaths::GetCleanFilename(Skipped->File),
        Skipped->bVariable ? TEXT("a variable font, which UE can't pick a weight from")
                           : TEXT("licensed without embedding (its fsType is Restricted)"));
  }
  return nullptr;
}

void UIWTDesignFonts::EnsureFonts(const FString &InDesignFile, FOnFontsReady InOnDone)
{
  const TSharedRef<FFontsJob> Job = MakeShared<FFontsJob>(InDesignFile, MoveTemp(InOnDone));
  FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
      [Job](float)
      {
        Job->Run();
        return false;
      }));
}

FString UIWTDesignFonts::ResultToJson(const FFontsResult &InResult)
{
  TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetField(TEXT("mapped"), Strings(InResult.Mapped));
  Object->SetField(TEXT("inLibrary"), Strings(InResult.InLibrary));
  Object->SetField(TEXT("installed"), Strings(InResult.Installed));
  Object->SetField(TEXT("downloaded"), Strings(InResult.Downloaded));
  Object->SetField(TEXT("failed"), Strings(InResult.Failed));
  Object->SetField(TEXT("assets"), Strings(InResult.Assets));
  return UIWTJson::Pretty(Object);
}

FString UIWTDesignFonts::Summary(const FFontsResult &InResult)
{
  TArray<FString> Parts;
  if (!InResult.Installed.IsEmpty())
  {
    Parts.Add(FString::Printf(TEXT("Took %d font%s installed on this computer: %s. Check that "
                                   "their licenses let them ship in a game."),
                              InResult.Installed.Num(),
                              InResult.Installed.Num() == 1 ? TEXT("") : TEXT("s"),
                              *FString::Join(InResult.Installed, TEXT(", "))));
  }
  if (!InResult.Downloaded.IsEmpty())
  {
    Parts.Add(FString::Printf(TEXT("Downloaded %d font%s from Google Fonts: %s."),
                              InResult.Downloaded.Num(),
                              InResult.Downloaded.Num() == 1 ? TEXT("") : TEXT("s"),
                              *FString::Join(InResult.Downloaded, TEXT(", "))));
  }
  if (!InResult.Failed.IsEmpty())
  {
    Parts.Add(FString::Printf(TEXT("%d font%s use the default font: %s."),
                              InResult.Failed.Num(),
                              InResult.Failed.Num() == 1 ? TEXT("") : TEXT("s"),
                              *FString::Join(InResult.Failed, TEXT("; "))));
  }
  return FString::Join(Parts, TEXT(" "));
}
