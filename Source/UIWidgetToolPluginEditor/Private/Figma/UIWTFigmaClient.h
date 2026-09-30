#pragma once

#include "Core/UIWTDesignTree.h"
#include "CoreMinimal.h"

// The Figma REST client (import-figma.md step 1): the token, the requests,
// the cache under Saved/UIWidgetTool/Figma, and fetching one frame into a
// design tree with its images. Requests complete on the game thread and
// nothing here waits on them, so the editor never blocks.
namespace UIWTFigmaClient
{
  struct FFrameUrl
  {
    FString FileKey;
    // With a colon, e.g. 12:34
    FString NodeId;
  };

  // Accepts figma.com /design/, /file/ and /proto/ links (a branch link
  // uses the branch's key), with node-id written 12-34 or 12:34.
  bool ParseUrl(const FString &InUrl, FFrameUrl &OutUrl, FString &OutError);

  // FIGMA_TOKEN, else UUIWTLocalSettings::FigmaToken. Never logged or
  // returned by a tool.
  FString GetToken();

  // Absolute: Saved/UIWidgetTool/Figma/<file key>/<node id>
  FString GetCacheDirectory(const FFrameUrl &InUrl);

  struct FFetchResult
  {
    // Empty on success.
    FString Error;
    // Absolute path of design.json.
    FString DesignFile;
    // Absolute path of reference.png; empty when the frame couldn't be
    // rendered.
    FString ReferenceImage;
    bool bFromCache = false;
    FString FileName;
    FString Version;
    FString LastModified;
    int32 NodeCount = 0;
    int32 ImageFillCount = 0;
    int32 RenderCount = 0;
    // Main components in design.json's components map.
    int32 ComponentCount = 0;
    TArray<UIWTDesignTree::FNote> Notes;
  };

  using FOnFetched = TFunction<void(const FFetchResult &)>;

  // Fetches the frame at InUrl into its cache folder and writes design.json.
  // First one cheap request for the file's version; unless the cache already
  // holds that version (or bInRefresh), then the node, one batched render
  // request per kind of bounds, the image-fill links and the downloads.
  // Then the main components of the instances that become child WBPs
  // (import-figma.md step 1 → Main components): each key's file and node,
  // one /nodes request per file, their images, and nested components, until
  // no new keys appear. A cached frame still checks each library file's
  // version and fetches the components again when one changed.
  // InOnDone runs on the game thread, never before Fetch returns.
  void Fetch(const FString &InUrl, bool bInRefresh, FOnFetched InOnDone);

  // The result as JSON text, for tools and logs.
  FString ResultToJson(const FFetchResult &InResult);
}
