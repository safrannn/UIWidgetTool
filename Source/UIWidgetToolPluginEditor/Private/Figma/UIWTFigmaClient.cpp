#include "UIWTFigmaClient.h"

#include "Containers/Ticker.h"
#include "Core/UIWTDesignSettings.h"
#include "Core/UIWTJson.h"
#include "Core/UIWTLocalSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Figma/UIWTFigmaNormalize.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
  using namespace UIWTFigmaClient;
  using UIWTFigmaNormalize::FImageJob;

  const TCHAR *const ApiBase = TEXT("https://api.figma.com/v1");

  // Node ids per /v1/images request, so the URL stays a sensible length.
  constexpr int32 IdsPerRequest = 100;

  // fetch.json's format. A cache written by an older format rebuilds its
  // components (screen.json is reused). 2: library stubs without layers are
  // built from an instance.
  constexpr int32 CacheFormat = 2;

  // Cache folders with a fetch in flight; two fetches of one frame would
  // write the same files.
  TSet<FString> InFlight;

  TSharedPtr<FJsonObject> ParseJson(const TArray<uint8> &InBody)
  {
    const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR *>(InBody.GetData()),
                                 InBody.Num());
    const FString Text(Converted.Length(), Converted.Get());
    TSharedPtr<FJsonObject> Object;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
    return Object;
  }

  TSharedPtr<FJsonObject> ReadJsonFile(const FString &InPath)
  {
    FString Text;
    TSharedPtr<FJsonObject> Object;
    if (FFileHelper::LoadFileToString(Text, *InPath))
    {
      FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
    }
    return Object;
  }

  bool WriteText(const FString &InText, const FString &InPath)
  {
    return FFileHelper::SaveStringToFile(InText, *InPath,
                                         FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
  }

  // Figma's error bodies: {"status":403,"err":"..."} or {"message":"..."}.
  FString ErrorText(int32 InCode, const TArray<uint8> &InBody)
  {
    FString Message;
    if (const TSharedPtr<FJsonObject> Body = ParseJson(InBody))
    {
      if (!Body->TryGetStringField(TEXT("err"), Message))
      {
        Body->TryGetStringField(TEXT("message"), Message);
      }
    }
    if (InCode == 403)
    {
      return FString::Printf(
          TEXT("Figma refused the request (403%s). Check the token (FIGMA_TOKEN, or Editor "
               "Preferences → Plugins → UI Widget Tool (local) → Figma Token) and that it can "
               "open this file."),
          Message.IsEmpty() ? TEXT("") : *(TEXT(": ") + Message));
    }
    if (InCode == 404)
    {
      return TEXT("Figma has no such file or node (404). Check the link.");
    }
    return FString::Printf(TEXT("Figma answered HTTP %d%s"), InCode,
                           Message.IsEmpty() ? TEXT("") : *(TEXT(": ") + Message));
  }

  using FOnResponse =
      TFunction<void(int32 InCode, const TArray<uint8> &InBody, const FString &InError)>;

  // A GET whose completion runs on the game thread. The token goes only to
  // the API, never to the pre-signed download links.
  void Get(const FString &InUrl, const FString &InToken, FOnResponse InOnDone)
  {
    const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request =
        FHttpModule::Get().CreateRequest();
    Request->SetURL(InUrl);
    Request->SetVerb(TEXT("GET"));
    if (!InToken.IsEmpty())
    {
      Request->SetHeader(TEXT("X-Figma-Token"), InToken);
    }
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
          const TArray<uint8> &Body = InResponse->GetContent();
          if (Code == 429)
          {
            const FString RetryAfter = InResponse->GetHeader(TEXT("Retry-After"));
            OnDone(Code, Body,
                   FString::Printf(TEXT("Figma's rate limit was reached; try again%s."),
                                   RetryAfter.IsEmpty()
                                       ? TEXT(" later")
                                       : *FString::Printf(TEXT(" in %s seconds"), *RetryAfter)));
            return;
          }
          OnDone(Code, Body, Code >= 200 && Code < 300 ? FString() : ErrorText(Code, Body));
        });
    Request->ProcessRequest();
  }

  // Everything one fetch does, in order; kept alive by its own callbacks.
  //
  // Requests run in stages. Begin sets what runs once the stage's requests
  // are all done; each request adds one to Pending and calls Done when it
  // completes; End releases the stage's own hold.
  class FFetchJob : public TSharedFromThis<FFetchJob>
  {
  public:
    FFetchJob(const FFrameUrl &InUrl, bool bInRefresh, FOnFetched InOnDone)
        : Url(InUrl), bRefresh(bInRefresh), OnDone(MoveTemp(InOnDone))
    {
    }

    void Start();
    void Fail(const FString &InError) { Finish(InError); }

  private:
    // A component.key found on an instance that becomes a child WBP.
    struct FKeyInfo
    {
      // The file the instance was seen in, and its componentId there: the
      // main component when the key isn't published (a local component).
      FString SeenFile;
      FString ComponentId;
      FString Name;
      // Where the main component lives.
      FString FileKey;
      FString NodeId;
      // The last library publish, from /v1/components.
      FString UpdatedAt;
      bool bLookedUp = false;
      bool bRequested = false;
      bool bFailed = false;
    };

    struct FLibrary
    {
      FString Version;
      FString LastTouched;
    };

    FFrameUrl Url;
    bool bRefresh = false;
    FOnFetched OnDone;
    FString Token;
    FString Directory;
    FFetchResult Result;
    UIWTFigmaNormalize::FOptions Options;
    // The frame: its tree, image jobs, notes and instance component ids.
    UIWTFigmaNormalize::FResult Screen;
    int32 Pending = 0;
    TFunction<void()> Next;
    // A failure that makes the rest pointless (rate limit, access).
    FString Fatal;
    bool bFinished = false;
    // This job holds Directory in InFlight.
    bool bInFlight = false;

    TMap<FString, FKeyInfo> Keys;
    TArray<FString> KeyOrder;
    TMap<FString, FLibrary> Libraries;
    // Library versions the cached components were built from.
    TMap<FString, FString> CachedLibraries;
    // The cache's CacheFormat; 0 before formats were written.
    int32 CachedFormat = 0;
    TMap<FString, UIWTDesignTree::FComponentDef> Defs;
    TArray<FImageJob> ComponentJobs;
    TArray<UIWTDesignTree::FNote> ComponentNotes;
    int32 ComponentNodes = 0;

    FString FileUrl(const FString &InFileKey, const FString &InPath) const
    {
      return FString(ApiBase) / TEXT("files") / InFileKey / InPath;
    }

    void Note(const FString &InNode, const TCHAR *InCategory, const FString &InDetail)
    {
      ComponentNotes.Add({InNode, InCategory, InDetail});
    }

    void Begin(TFunction<void()> InNext)
    {
      Next = MoveTemp(InNext);
      Pending = 1;
    }

    void Done()
    {
      if (--Pending > 0)
      {
        return;
      }
      TFunction<void()> Then = MoveTemp(Next);
      Next = nullptr;
      if (!Fatal.IsEmpty())
      {
        Finish(Fatal);
        return;
      }
      Then();
    }

    void Finish(const FString &InError);
    void OnMeta(const TArray<uint8> &InBody);
    bool LoadScreenCache();
    void CheckLibraries();
    void CacheHit();
    void OnNodes(const TArray<uint8> &InBody);
    void ScreenDone();
    void StartComponents();
    void AddKeys(const TArray<FString> &InKeys, const TMap<FString, FString> &InNames,
                 const FString &InFile, const TMap<FString, FString> &InComponentIds);
    void LookUpKeys();
    void FetchDefinitions();
    void AfterDefinitions();
    void ComponentImages();
    void Assemble();
    void RequestMeta(const FString &InFileKey);
    void RequestImages(const FString &InFileKey, const TArray<FImageJob> &InJobs,
                       bool bInWithFrame);
    void RequestRenders(const FString &InFileKey, const TArray<TPair<FString, FString>> &InRenders,
                        bool bInAbsoluteBounds);
    void RequestImageFills(const FString &InFileKey, const TSet<FString> &InRefs);
    void Download(const FString &InUrl, const FString &InFile, const FString &InNode);
    FString CacheKey() const
    {
      return Result.Version.IsEmpty() ? Result.LastModified : Result.Version;
    }
  };

  void FFetchJob::Start()
  {
    Token = GetToken();
    Directory = GetCacheDirectory(Url);
    if (Token.IsEmpty())
    {
      Finish(TEXT("There is no Figma token. Set the FIGMA_TOKEN environment variable, or "
                  "Editor Preferences → Plugins → UI Widget Tool (local) → Figma Token."));
      return;
    }
    if (InFlight.Contains(Directory))
    {
      Finish(TEXT("This frame is already being fetched; wait for that to finish."));
      return;
    }
    InFlight.Add(Directory);
    bInFlight = true;
    TSharedRef<FFetchJob> Self = AsShared();
    Get(FileUrl(Url.FileKey, TEXT("meta")), Token,
        [Self](int32, const TArray<uint8> &InBody, const FString &InError)
        {
          if (!InError.IsEmpty())
          {
            Self->Finish(TEXT("Reading the file's version failed: ") + InError);
            return;
          }
          Self->OnMeta(InBody);
        });
  }

  void FFetchJob::Finish(const FString &InError)
  {
    if (bFinished)
    {
      return;
    }
    bFinished = true;
    if (bInFlight)
    {
      InFlight.Remove(Directory);
    }
    Result.Error = InError;
    // Never before Fetch returns: a tool subscribes to its result after that.
    TSharedRef<FFetchJob> Self = AsShared();
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
        [Self](float)
        {
          Self->OnDone(Self->Result);
          return false;
        }));
  }

  void FFetchJob::OnMeta(const TArray<uint8> &InBody)
  {
    const TSharedPtr<FJsonObject> Meta = ParseJson(InBody);
    const TSharedPtr<FJsonObject> *File = nullptr;
    if (!Meta.IsValid() || !Meta->TryGetObjectField(TEXT("file"), File))
    {
      Finish(TEXT("Figma's file information couldn't be read."));
      return;
    }
    (*File)->TryGetStringField(TEXT("name"), Result.FileName);
    (*File)->TryGetStringField(TEXT("version"), Result.Version);
    (*File)->TryGetStringField(TEXT("last_touched_at"), Result.LastModified);
    Libraries.Add(Url.FileKey, {Result.Version, Result.LastModified});

    Options.FileKey = Url.FileKey;
    Options.NodeId = Url.NodeId;
    Options.FileName = Result.FileName;
    Options.Version = Result.Version;
    Options.LastModified = Result.LastModified;
    Options.ReferenceSize = UUIWTDesignSettings::Get()->ReferenceResolution;
    Options.RenderScale = UUIWTDesignSettings::Get()->RenderScale;
    Options.MaxImageScale = UUIWTDesignSettings::Get()->MaxImageScale;

    if (!bRefresh && LoadScreenCache())
    {
      CheckLibraries();
      return;
    }

    // geometry=paths is what adds relativeTransform and size to each node;
    // the vector paths it also adds are unused but cost no extra request.
    TSharedRef<FFetchJob> Self = AsShared();
    Get(FileUrl(Url.FileKey, TEXT("nodes?ids=") + FGenericPlatformHttp::UrlEncode(Url.NodeId) +
                                 TEXT("&geometry=paths")),
        Token,
        [Self](int32, const TArray<uint8> &InBody, const FString &InError)
        {
          if (!InError.IsEmpty())
          {
            Self->Finish(TEXT("Reading the frame failed: ") + InError);
            return;
          }
          Self->OnNodes(InBody);
        });
  }

  // The cache holds this version of the frame's file: its tree (screen.json,
  // without the components) is reused.
  bool FFetchJob::LoadScreenCache()
  {
    const TSharedPtr<FJsonObject> Fetch = ReadJsonFile(Directory / TEXT("fetch.json"));
    FString CachedKey;
    FString ScreenJson;
    TArray<FString> Errors;
    if (CacheKey().IsEmpty() || !Fetch.IsValid() ||
        !Fetch->TryGetStringField(TEXT("cacheKey"), CachedKey) || CachedKey != CacheKey() ||
        !FFileHelper::LoadFileToString(ScreenJson, *(Directory / TEXT("screen.json"))) ||
        !UIWTDesignTree::ReadDocument(ScreenJson, Screen.Document, Errors))
    {
      Screen = UIWTFigmaNormalize::FResult();
      return false;
    }
    Fetch->TryGetNumberField(TEXT("format"), CachedFormat);
    Fetch->TryGetNumberField(TEXT("screenNodes"), Screen.NodeCount);
    Fetch->TryGetNumberField(TEXT("imageFills"), Result.ImageFillCount);
    Fetch->TryGetNumberField(TEXT("renders"), Result.RenderCount);
    const TSharedPtr<FJsonObject> *Object = nullptr;
    if (Fetch->TryGetObjectField(TEXT("libraries"), Object))
    {
      for (const auto &Pair : (*Object)->Values)
      {
        CachedLibraries.Add(FString(*Pair.Key), Pair.Value->AsString());
      }
    }
    if (Fetch->TryGetObjectField(TEXT("componentIds"), Object))
    {
      for (const auto &Pair : (*Object)->Values)
      {
        Screen.ComponentNodeIds.Add(FString(*Pair.Key), Pair.Value->AsString());
      }
    }
    return true;
  }

  // A cached frame is only current if the libraries its components came
  // from haven't changed either.
  void FFetchJob::CheckLibraries()
  {
    TSharedRef<FFetchJob> Self = AsShared();
    Begin(
        [Self]()
        {
          bool bSame = Self->CachedFormat == CacheFormat &&
                       IFileManager::Get().FileExists(*(Self->Directory / TEXT("design.json")));
          for (const TPair<FString, FString> &Pair : Self->CachedLibraries)
          {
            const FLibrary *Library = Self->Libraries.Find(Pair.Key);
            bSame &= Library && !Library->Version.IsEmpty() && Library->Version == Pair.Value;
          }
          if (bSame)
          {
            Self->CacheHit();
          }
          else
          {
            Self->StartComponents();
          }
        });
    for (const TPair<FString, FString> &Pair : CachedLibraries)
    {
      RequestMeta(Pair.Key);
    }
    Done();
  }

  void FFetchJob::CacheHit()
  {
    FString DesignJson;
    UIWTDesignTree::FDocument Doc;
    TArray<FString> Errors;
    FFileHelper::LoadFileToString(DesignJson, *(Directory / TEXT("design.json")));
    UIWTDesignTree::ReadDocument(DesignJson, Doc, Errors);
    const TSharedPtr<FJsonObject> Fetch = ReadJsonFile(Directory / TEXT("fetch.json"));
    if (Fetch.IsValid())
    {
      Fetch->TryGetNumberField(TEXT("nodes"), Result.NodeCount);
    }
    Result.bFromCache = true;
    Result.DesignFile = Directory / TEXT("design.json");
    const FString Reference = Directory / TEXT("reference.png");
    Result.ReferenceImage = IFileManager::Get().FileExists(*Reference) ? Reference : FString();
    Result.ComponentCount = Doc.Components.Num();
    Result.Notes = MoveTemp(Doc.Notes);
    Finish(FString());
  }

  void FFetchJob::OnNodes(const TArray<uint8> &InBody)
  {
    IFileManager &Files = IFileManager::Get();
    Files.MakeDirectory(*Directory, true);
    // The raw response, for debugging the normalizer.
    FFileHelper::SaveArrayToFile(InBody, *(Directory / TEXT("nodes.json")));
    const TSharedPtr<FJsonObject> Response = ParseJson(InBody);
    if (!Response.IsValid())
    {
      Finish(TEXT("Figma's node response couldn't be read."));
      return;
    }
    FString Error;
    if (!UIWTFigmaNormalize::Normalize(Response.ToSharedRef(), Options, Screen, Error))
    {
      Finish(Error);
      return;
    }

    // Renders and cropped images belong to one version; downloaded image
    // fills are named by content and stay.
    Files.DeleteDirectory(*(Directory / TEXT("render")), false, true);
    TArray<FString> OldImages;
    Files.FindFiles(OldImages, *(Directory / TEXT("images/*.png")), true, false);
    for (const FString &Name : OldImages)
    {
      Files.Delete(*(Directory / TEXT("images") / Name), false, true, true);
    }
    Files.Delete(*(Directory / TEXT("reference.png")), false, true, true);
    Files.Delete(*(Directory / UIWTFigmaNormalize::ReferenceRenderFile()), false, true, true);

    TSet<FString> Refs;
    for (const FImageJob &Job : Screen.Images)
    {
      if (Job.RenderId.IsEmpty() && !Job.ImageRef.IsEmpty())
      {
        Refs.Add(Job.ImageRef);
      }
    }
    Result.ImageFillCount = Refs.Num();

    TSharedRef<FFetchJob> Self = AsShared();
    Begin([Self]() { Self->ScreenDone(); });
    RequestImages(Url.FileKey, Screen.Images, true);
    Done();
  }

  // The frame's images are in: its tree is complete except for the
  // components map.
  void FFetchJob::ScreenDone()
  {
    UIWTFigmaNormalize::FinishImages(Screen, Directory, Options);
    // Once per fetch: the frame's render arrives with the screen's images.
    UIWTFigmaNormalize::WriteReference(Directory, Options);
    if (!WriteText(UIWTDesignTree::WriteDocumentString(Screen.Document),
                   Directory / TEXT("screen.json")))
    {
      Finish(FString::Printf(TEXT("Could not write %s."), *(Directory / TEXT("screen.json"))));
      return;
    }
    StartComponents();
  }

  void FFetchJob::StartComponents()
  {
    // Earlier component renders are per file, under render/<file key>/.
    IFileManager &Files = IFileManager::Get();
    TArray<FString> Folders;
    Files.FindFiles(Folders, *(Directory / TEXT("render/*")), false, true);
    for (const FString &Folder : Folders)
    {
      Files.DeleteDirectory(*(Directory / TEXT("render") / Folder), false, true);
    }
    TArray<FString> Found;
    TMap<FString, FString> Names;
    UIWTFigmaNormalize::CollectComponentKeys(Screen.Document.Root, Found, &Names);
    AddKeys(Found, Names, Url.FileKey, Screen.ComponentNodeIds);
    if (Keys.IsEmpty())
    {
      Assemble();
      return;
    }
    LookUpKeys();
  }

  void FFetchJob::AddKeys(const TArray<FString> &InKeys, const TMap<FString, FString> &InNames,
                          const FString &InFile, const TMap<FString, FString> &InComponentIds)
  {
    for (const FString &Key : InKeys)
    {
      if (Keys.Contains(Key))
      {
        continue;
      }
      FKeyInfo Info;
      Info.SeenFile = InFile;
      Info.ComponentId = InComponentIds.FindRef(Key);
      Info.Name = InNames.FindRef(Key);
      Keys.Add(Key, MoveTemp(Info));
      KeyOrder.Add(Key);
    }
  }

  // Where each key's main component lives: /v1/components/{key} for a
  // published one; a 404 means it's local to the file the instance is in.
  void FFetchJob::LookUpKeys()
  {
    TSharedRef<FFetchJob> Self = AsShared();
    Begin([Self]() { Self->FetchDefinitions(); });
    for (const FString &Key : KeyOrder)
    {
      FKeyInfo &Info = Keys[Key];
      if (Info.bLookedUp)
      {
        continue;
      }
      Info.bLookedUp = true;
      if (Key.StartsWith(TEXT("local:")))
      {
        Info.FileKey = Info.SeenFile;
        Info.NodeId = Key.RightChop(6);
        continue;
      }
      ++Pending;
      Get(FString(ApiBase) / TEXT("components") / Key, Token,
          [Self, Key](int32 InCode, const TArray<uint8> &InBody, const FString &InError)
          {
            FKeyInfo &Found = Self->Keys[Key];
            const TSharedPtr<FJsonObject> Response = InError.IsEmpty() ? ParseJson(InBody) : nullptr;
            const TSharedPtr<FJsonObject> *Meta = nullptr;
            if (Response.IsValid() && Response->TryGetObjectField(TEXT("meta"), Meta))
            {
              (*Meta)->TryGetStringField(TEXT("file_key"), Found.FileKey);
              (*Meta)->TryGetStringField(TEXT("node_id"), Found.NodeId);
              (*Meta)->TryGetStringField(TEXT("updated_at"), Found.UpdatedAt);
            }
            else if (InCode == 404)
            {
              // Not published: the main component is in the instance's file.
              Found.FileKey = Found.SeenFile;
              Found.NodeId = Found.ComponentId;
            }
            else if (InCode == 429)
            {
              Self->Fatal = InError;
            }
            if (Found.FileKey.IsEmpty() || Found.NodeId.IsEmpty())
            {
              Found.bFailed = true;
              Self->Note(Found.ComponentId, TEXT("componentMissing"),
                         FString::Printf(TEXT("the main component of '%s' couldn't be found (%s); "
                                              "a child WBP can only be built from an instance"),
                                         *Found.Name,
                                         InError.IsEmpty() ? TEXT("no file or node") : *InError));
            }
            Self->Done();
          });
    }
    Done();
  }

  // The main components, one /nodes request per file.
  void FFetchJob::FetchDefinitions()
  {
    TMap<FString, TArray<FString>> ByFile;
    for (const FString &Key : KeyOrder)
    {
      FKeyInfo &Info = Keys[Key];
      if (Info.bLookedUp && !Info.bFailed && !Info.bRequested)
      {
        Info.bRequested = true;
        ByFile.FindOrAdd(Info.FileKey).Add(Key);
      }
    }
    TSharedRef<FFetchJob> Self = AsShared();
    Begin([Self]() { Self->AfterDefinitions(); });
    for (const TPair<FString, TArray<FString>> &Pair : ByFile)
    {
      const FString FileKey = Pair.Key;
      const TArray<FString> FileKeys = Pair.Value;
      if (!Libraries.Contains(FileKey))
      {
        RequestMeta(FileKey);
      }
      TArray<FString> Ids;
      for (const FString &Key : FileKeys)
      {
        Ids.AddUnique(Keys[Key].NodeId);
      }
      ++Pending;
      Get(FileUrl(FileKey, TEXT("nodes?ids=") +
                               FGenericPlatformHttp::UrlEncode(FString::Join(Ids, TEXT(","))) +
                               TEXT("&geometry=paths")),
          Token,
          [Self, FileKey, FileKeys](int32 InCode, const TArray<uint8> &InBody,
                                    const FString &InError)
          {
            const TSharedPtr<FJsonObject> Response = InError.IsEmpty() ? ParseJson(InBody) : nullptr;
            if (InCode == 429)
            {
              Self->Fatal = InError;
            }
            for (const FString &Key : FileKeys)
            {
              FKeyInfo &Info = Self->Keys[Key];
              UIWTFigmaNormalize::FOptions ComponentOptions = Self->Options;
              ComponentOptions.FileKey = FileKey;
              ComponentOptions.NodeId = Info.NodeId;
              ComponentOptions.RenderFileKey = FileKey == Self->Url.FileKey ? FString() : FileKey;
              UIWTFigmaNormalize::FResult Normalized;
              UIWTDesignTree::FComponentDef Def;
              FString Error = InError;
              if (Error.IsEmpty() &&
                  (!Response.IsValid() ||
                   !UIWTFigmaNormalize::NormalizeComponent(Response.ToSharedRef(), ComponentOptions,
                                                           Normalized, Def.Root, Error)))
              {
                Error = Error.IsEmpty() ? TEXT("the response couldn't be read") : Error;
              }
              if (!Error.IsEmpty())
              {
                Info.bFailed = true;
                Self->Note(Info.ComponentId, TEXT("componentMissing"),
                           FString::Printf(TEXT("the main component of '%s' couldn't be read "
                                                "(%s); a child WBP can only be built from an "
                                                "instance"),
                                           *Info.Name, *Error));
                continue;
              }
              // A library component the token can't open (unpublished, or no
              // access) comes back as a stub without its layers. Its
              // instances in the frame still carry them.
              const bool bStub = Def.Root.Children.IsEmpty() &&
                                 UIWTFigmaNormalize::ComponentFromInstance(
                                     Self->Screen.Document.Root, Key, Def.Root);
              if (bStub)
              {
                Self->Note(Info.ComponentId, TEXT("componentStub"),
                           FString::Printf(TEXT("Figma returned the main component of '%s' "
                                                "without its layers (a library component this "
                                                "token can't open); the child WBP is built from "
                                                "instance %s"),
                                           *Info.Name, *Def.Root.Id));
              }
              Def.Name = Info.Name.IsEmpty() ? Def.Root.Name : Info.Name;
              Def.FileKey = FileKey;
              Def.NodeId = Info.NodeId;
              if (!bStub)
              {
                // An instance's images and notes are already the frame's.
                Self->ComponentJobs.Append(MoveTemp(Normalized.Images));
                Self->ComponentNotes.Append(MoveTemp(Normalized.Document.Notes));
                Self->ComponentNodes += Normalized.NodeCount;
              }
              // Components inside this one, found in its file.
              TArray<FString> Nested;
              TMap<FString, FString> Names;
              UIWTFigmaNormalize::CollectComponentKeys(Def.Root, Nested, &Names);
              Self->AddKeys(Nested, Names, bStub ? Self->Url.FileKey : FileKey,
                            bStub ? Self->Screen.ComponentNodeIds : Normalized.ComponentNodeIds);
              Self->Defs.Add(Key, MoveTemp(Def));
            }
            Self->Done();
          });
    }
    Done();
  }

  // Nested components may have brought new keys: repeat until none.
  void FFetchJob::AfterDefinitions()
  {
    for (const FString &Key : KeyOrder)
    {
      if (!Keys[Key].bLookedUp)
      {
        LookUpKeys();
        return;
      }
    }
    for (const FString &Key : KeyOrder)
    {
      const FKeyInfo &Info = Keys[Key];
      if (!Info.bFailed && !Info.bRequested)
      {
        FetchDefinitions();
        return;
      }
    }
    ComponentImages();
  }

  void FFetchJob::ComponentImages()
  {
    TMap<FString, TArray<FImageJob>> ByFile;
    for (const FImageJob &Job : ComponentJobs)
    {
      ByFile.FindOrAdd(Job.FileKey).Add(Job);
    }
    TSharedRef<FFetchJob> Self = AsShared();
    Begin([Self]() { Self->Assemble(); });
    for (const TPair<FString, TArray<FImageJob>> &Pair : ByFile)
    {
      RequestImages(Pair.Key, Pair.Value, false);
    }
    Done();
  }

  void FFetchJob::Assemble()
  {
    // The components' pixels, then their hashes.
    UIWTFigmaNormalize::FResult Images;
    Images.Images = MoveTemp(ComponentJobs);
    UIWTFigmaNormalize::FinishImages(Images, Directory, Options);
    ComponentNotes.Append(Images.Document.Notes);
    TMap<FString, FString> UsedLibraries;
    for (TPair<FString, UIWTDesignTree::FComponentDef> &Pair : Defs)
    {
      UIWTDesignTree::FComponentDef &Def = Pair.Value;
      const FLibrary *Library = Libraries.Find(Def.FileKey);
      Def.Version = Library ? Library->Version : FString();
      Def.Hash = UIWTFigmaNormalize::HashComponent(Def.Root, Directory);
      if (Def.FileKey != Url.FileKey)
      {
        UsedLibraries.Add(Def.FileKey, Def.Version);
      }
      // Edited since the last publish: the build includes unpublished
      // changes (import-tree.md → Components).
      const FKeyInfo &Info = Keys[Pair.Key];
      FDateTime Published;
      FDateTime Touched;
      if (Library && !Info.UpdatedAt.IsEmpty() &&
          FDateTime::ParseIso8601(*Info.UpdatedAt, Published) &&
          FDateTime::ParseIso8601(*Library->LastTouched, Touched) && Touched > Published)
      {
        ComponentNotes.Add({Def.NodeId, TEXT("componentUnpublished"),
                            FString::Printf(TEXT("'%s': its library was edited after the last "
                                                 "publish, so the child WBP may include "
                                                 "unpublished changes"),
                                            *Def.Name)});
      }
    }

    UIWTDesignTree::FDocument Doc = Screen.Document;
    Doc.Notes.Append(ComponentNotes);
    Doc.Components = Defs;
    const FString DesignFile = Directory / TEXT("design.json");
    if (!WriteText(UIWTDesignTree::WriteDocumentString(Doc), DesignFile))
    {
      Finish(FString::Printf(TEXT("Could not write %s."), *DesignFile));
      return;
    }
    Result.DesignFile = DesignFile;
    const FString Reference = Directory / TEXT("reference.png");
    Result.ReferenceImage = IFileManager::Get().FileExists(*Reference) ? Reference : FString();
    Result.NodeCount = Screen.NodeCount + ComponentNodes;
    Result.ComponentCount = Defs.Num();
    Result.Notes = Doc.Notes;

    TSharedRef<FJsonObject> Fetch = MakeShared<FJsonObject>();
    Fetch->SetStringField(TEXT("cacheKey"), CacheKey());
    Fetch->SetNumberField(TEXT("format"), CacheFormat);
    Fetch->SetStringField(TEXT("version"), Result.Version);
    Fetch->SetStringField(TEXT("lastModified"), Result.LastModified);
    Fetch->SetStringField(TEXT("fileName"), Result.FileName);
    Fetch->SetNumberField(TEXT("screenNodes"), Screen.NodeCount);
    Fetch->SetNumberField(TEXT("nodes"), Result.NodeCount);
    Fetch->SetNumberField(TEXT("imageFills"), Result.ImageFillCount);
    Fetch->SetNumberField(TEXT("renders"), Result.RenderCount);
    TSharedRef<FJsonObject> LibrariesObject = MakeShared<FJsonObject>();
    for (const TPair<FString, FString> &Pair : UsedLibraries)
    {
      LibrariesObject->SetStringField(Pair.Key, Pair.Value);
    }
    Fetch->SetObjectField(TEXT("libraries"), LibrariesObject);
    TSharedRef<FJsonObject> Ids = MakeShared<FJsonObject>();
    for (const TPair<FString, FString> &Pair : Screen.ComponentNodeIds)
    {
      Ids->SetStringField(Pair.Key, Pair.Value);
    }
    Fetch->SetObjectField(TEXT("componentIds"), Ids);
    WriteText(UIWTJson::Pretty(Fetch), Directory / TEXT("fetch.json"));
    Finish(FString());
  }

  void FFetchJob::RequestMeta(const FString &InFileKey)
  {
    Libraries.Add(InFileKey);
    ++Pending;
    TSharedRef<FFetchJob> Self = AsShared();
    Get(FileUrl(InFileKey, TEXT("meta")), Token,
        [Self, InFileKey](int32 InCode, const TArray<uint8> &InBody, const FString &InError)
        {
          const TSharedPtr<FJsonObject> Meta = InError.IsEmpty() ? ParseJson(InBody) : nullptr;
          const TSharedPtr<FJsonObject> *File = nullptr;
          FLibrary &Library = Self->Libraries.FindOrAdd(InFileKey);
          if (Meta.IsValid() && Meta->TryGetObjectField(TEXT("file"), File))
          {
            (*File)->TryGetStringField(TEXT("version"), Library.Version);
            (*File)->TryGetStringField(TEXT("last_touched_at"), Library.LastTouched);
          }
          else if (InCode == 429)
          {
            Self->Fatal = InError;
          }
          Self->Done();
        });
  }

  // One file's renders (layers an auto layout places, and the frame when
  // bInWithFrame, with the layer's own box; the rest with their render
  // bounds) and image fills, downloaded to the paths the jobs name.
  void FFetchJob::RequestImages(const FString &InFileKey, const TArray<FImageJob> &InJobs,
                                bool bInWithFrame)
  {
    IFileManager &Files = IFileManager::Get();
    TArray<TPair<FString, FString>> Absolute;
    TArray<TPair<FString, FString>> RenderBounds;
    if (bInWithFrame)
    {
      Absolute.Add({Url.NodeId, UIWTFigmaNormalize::ReferenceRenderFile()});
    }
    TSet<FString> Refs;
    TSet<FString> Seen;
    for (const FImageJob &Job : InJobs)
    {
      if (!Job.RenderId.IsEmpty())
      {
        if (!Seen.Contains(Job.RenderId))
        {
          Seen.Add(Job.RenderId);
          (Job.bAbsoluteBounds ? Absolute : RenderBounds).Add({Job.RenderId, Job.Image->Path});
        }
      }
      else if (!Job.ImageRef.IsEmpty() &&
               !Files.FileExists(*(Directory / UIWTFigmaNormalize::SourceImageFile(Job.ImageRef))))
      {
        Refs.Add(Job.ImageRef);
      }
    }
    Result.RenderCount += Seen.Num();
    RequestRenders(InFileKey, Absolute, true);
    RequestRenders(InFileKey, RenderBounds, false);
    if (!Refs.IsEmpty())
    {
      RequestImageFills(InFileKey, Refs);
    }
  }

  void FFetchJob::RequestRenders(const FString &InFileKey,
                                 const TArray<TPair<FString, FString>> &InRenders,
                                 bool bInAbsoluteBounds)
  {
    const FString Scale = FString::SanitizeFloat(Options.RenderScale);
    for (int32 First = 0; First < InRenders.Num(); First += IdsPerRequest)
    {
      TArray<TPair<FString, FString>> Chunk(InRenders.GetData() + First,
                                            FMath::Min(IdsPerRequest, InRenders.Num() - First));
      TArray<FString> Ids;
      for (const TPair<FString, FString> &Render : Chunk)
      {
        Ids.Add(Render.Key);
      }
      const FString Path =
          FString(ApiBase) / TEXT("images") / InFileKey + TEXT("?ids=") +
          FGenericPlatformHttp::UrlEncode(FString::Join(Ids, TEXT(","))) +
          TEXT("&format=png&scale=") + Scale +
          (bInAbsoluteBounds ? TEXT("&use_absolute_bounds=true") : TEXT(""));
      ++Pending;
      TSharedRef<FFetchJob> Self = AsShared();
      Get(Path, Token,
          [Self, Chunk](int32 InCode, const TArray<uint8> &InBody, const FString &InError)
          {
            const TSharedPtr<FJsonObject> Response = InError.IsEmpty() ? ParseJson(InBody) : nullptr;
            const TSharedPtr<FJsonObject> *Images = nullptr;
            if (!Response.IsValid() || !Response->TryGetObjectField(TEXT("images"), Images))
            {
              if (InCode == 429 || InCode == 403)
              {
                Self->Fatal = InError;
              }
              for (const TPair<FString, FString> &Render : Chunk)
              {
                Self->Note(Render.Key, TEXT("renderFailed"),
                           InError.IsEmpty() ? TEXT("Figma's render response couldn't be read")
                                             : InError);
              }
              Self->Done();
              return;
            }
            for (const TPair<FString, FString> &Render : Chunk)
            {
              FString Link;
              if (!(*Images)->TryGetStringField(Render.Key, Link) || Link.IsEmpty())
              {
                Self->Note(Render.Key, TEXT("renderFailed"),
                           TEXT("Figma rendered nothing for this layer (empty or invisible)"));
                continue;
              }
              Self->Download(Link, Render.Value, Render.Key);
            }
            Self->Done();
          });
    }
  }

  void FFetchJob::RequestImageFills(const FString &InFileKey, const TSet<FString> &InRefs)
  {
    ++Pending;
    TSharedRef<FFetchJob> Self = AsShared();
    Get(FileUrl(InFileKey, TEXT("images")), Token,
        [Self, InRefs](int32 InCode, const TArray<uint8> &InBody, const FString &InError)
        {
          const TSharedPtr<FJsonObject> Response = InError.IsEmpty() ? ParseJson(InBody) : nullptr;
          const TSharedPtr<FJsonObject> *Meta = nullptr;
          const TSharedPtr<FJsonObject> *Images = nullptr;
          if (!Response.IsValid() || !Response->TryGetObjectField(TEXT("meta"), Meta) ||
              !(*Meta)->TryGetObjectField(TEXT("images"), Images))
          {
            if (InCode == 429 || InCode == 403)
            {
              Self->Fatal = InError;
            }
            Self->Note(Self->Url.NodeId, TEXT("imageFillsFailed"),
                       InError.IsEmpty() ? TEXT("Figma's image-fill links couldn't be read")
                                         : InError);
            Self->Done();
            return;
          }
          for (const FString &Ref : InRefs)
          {
            FString Link;
            if ((*Images)->TryGetStringField(Ref, Link) && !Link.IsEmpty())
            {
              Self->Download(Link, UIWTFigmaNormalize::SourceImageFile(Ref), FString());
            }
          }
          Self->Done();
        });
  }

  void FFetchJob::Download(const FString &InUrl, const FString &InFile, const FString &InNode)
  {
    ++Pending;
    TSharedRef<FFetchJob> Self = AsShared();
    // Pre-signed links: no token.
    Get(InUrl, FString(),
        [Self, InFile, InNode](int32, const TArray<uint8> &InBody, const FString &InError)
        {
          if (!InError.IsEmpty() ||
              !FFileHelper::SaveArrayToFile(InBody, *(Self->Directory / InFile)))
          {
            Self->Note(InNode, TEXT("downloadFailed"),
                       FString::Printf(TEXT("%s: %s"), *InFile,
                                       InError.IsEmpty() ? TEXT("couldn't be written") : *InError));
          }
          Self->Done();
        });
  }
}

// ---------------------------------------------------------------------------

bool UIWTFigmaClient::ParseUrl(const FString &InUrl, FFrameUrl &OutUrl, FString &OutError)
{
  OutUrl = FFrameUrl();
  FString Rest = InUrl.TrimStartAndEnd();
  const int32 Scheme = Rest.Find(TEXT("://"));
  if (Scheme != INDEX_NONE)
  {
    Rest.RightChopInline(Scheme + 3);
  }
  FString Query;
  Rest.Split(TEXT("?"), &Rest, &Query);
  Query.Split(TEXT("#"), &Query, nullptr);
  Rest.Split(TEXT("#"), &Rest, nullptr);

  TArray<FString> Segments;
  Rest.ParseIntoArray(Segments, TEXT("/"));
  const FString Host = Segments.IsEmpty() ? FString() : Segments[0].ToLower();
  if (Host != TEXT("figma.com") && !Host.EndsWith(TEXT(".figma.com")))
  {
    OutError = TEXT("That isn't a figma.com link.");
    return false;
  }
  const bool bKnownKind = Segments.Num() >= 3 &&
                          (Segments[1] == TEXT("design") || Segments[1] == TEXT("file") ||
                           Segments[1] == TEXT("proto"));
  if (!bKnownKind)
  {
    OutError = TEXT("The link isn't a Figma design file (figma.com/design/<key>/...).");
    return false;
  }
  OutUrl.FileKey = Segments.Num() >= 5 && Segments[3] == TEXT("branch") ? Segments[4] : Segments[2];

  TArray<FString> Params;
  Query.ParseIntoArray(Params, TEXT("&"));
  for (const FString &Param : Params)
  {
    FString Key;
    FString Value;
    if (Param.Split(TEXT("="), &Key, &Value) && Key == TEXT("node-id"))
    {
      OutUrl.NodeId = FGenericPlatformHttp::UrlDecode(Value).Replace(TEXT("-"), TEXT(":"));
    }
  }
  if (OutUrl.NodeId.IsEmpty())
  {
    OutError = TEXT("The link has no node-id. Select the frame in Figma and copy its link "
                    "(right-click → Copy link to selection).");
    return false;
  }
  for (const TCHAR Char : OutUrl.FileKey)
  {
    if (!FChar::IsAlnum(Char))
    {
      OutError = TEXT("The link's file key isn't valid.");
      return false;
    }
  }
  for (const TCHAR Char : OutUrl.NodeId)
  {
    if (!FChar::IsDigit(Char) && Char != TEXT(':') && Char != TEXT(';') && Char != TEXT('I'))
    {
      OutError = FString::Printf(TEXT("The link's node-id \"%s\" isn't valid."), *OutUrl.NodeId);
      return false;
    }
  }
  return true;
}

FString UIWTFigmaClient::GetToken()
{
  const FString FromEnvironment =
      FPlatformMisc::GetEnvironmentVariable(TEXT("FIGMA_TOKEN")).TrimStartAndEnd();
  return FromEnvironment.IsEmpty() ? UUIWTLocalSettings::Get()->FigmaToken.TrimStartAndEnd()
                                   : FromEnvironment;
}

FString UIWTFigmaClient::GetCacheDirectory(const FFrameUrl &InUrl)
{
  return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() /
                                           TEXT("UIWidgetTool/Figma") / InUrl.FileKey /
                                           UIWTFigmaNormalize::FileSafeId(InUrl.NodeId));
}

void UIWTFigmaClient::Fetch(const FString &InUrl, bool bInRefresh, FOnFetched InOnDone)
{
  FFrameUrl Url;
  FString Error;
  const bool bParsed = ParseUrl(InUrl, Url, Error);
  const TSharedRef<FFetchJob> Job = MakeShared<FFetchJob>(Url, bInRefresh, MoveTemp(InOnDone));
  if (bParsed)
  {
    Job->Start();
  }
  else
  {
    // Through the job, so the callback still comes after Fetch returns.
    Job->Fail(Error);
  }
}

FString UIWTFigmaClient::ResultToJson(const FFetchResult &InResult)
{
  TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
  if (!InResult.Error.IsEmpty())
  {
    Object->SetStringField(TEXT("error"), InResult.Error);
    return UIWTJson::Pretty(Object);
  }
  Object->SetStringField(TEXT("designFile"), InResult.DesignFile);
  Object->SetStringField(TEXT("referenceImage"), InResult.ReferenceImage);
  Object->SetBoolField(TEXT("fromCache"), InResult.bFromCache);
  Object->SetStringField(TEXT("fileName"), InResult.FileName);
  Object->SetStringField(TEXT("version"), InResult.Version);
  Object->SetStringField(TEXT("lastModified"), InResult.LastModified);
  Object->SetNumberField(TEXT("nodes"), InResult.NodeCount);
  Object->SetNumberField(TEXT("imageFills"), InResult.ImageFillCount);
  Object->SetNumberField(TEXT("renders"), InResult.RenderCount);
  Object->SetNumberField(TEXT("components"), InResult.ComponentCount);
  TArray<TSharedPtr<FJsonValue>> Notes;
  for (const UIWTDesignTree::FNote &Note : InResult.Notes)
  {
    TSharedRef<FJsonObject> NoteObject = MakeShared<FJsonObject>();
    NoteObject->SetStringField(TEXT("node"), Note.Node);
    NoteObject->SetStringField(TEXT("category"), Note.Category);
    NoteObject->SetStringField(TEXT("detail"), Note.Detail);
    Notes.Add(MakeShared<FJsonValueObject>(NoteObject));
  }
  Object->SetArrayField(TEXT("notes"), Notes);
  return UIWTJson::Pretty(Object);
}
