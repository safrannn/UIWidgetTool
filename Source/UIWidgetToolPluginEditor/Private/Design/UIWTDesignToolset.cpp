#include "UIWTDesignToolset.h"

#include "Blueprint/UserWidget.h"
#include "Containers/Ticker.h"
#include "Core/UIWTDesignFonts.h"
#include "Core/UIWTDesignImport.h"
#include "Core/UIWTDesignSettings.h"
#include "Core/UIWTJson.h"
#include "Core/UIWTNotify.h"
#include "Core/UIWTPaths.h"
#include "Design/UIWTDesignRefine.h"
#include "Design/UIWTDesignSources.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Figma/UIWTFigmaClient.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "WidgetBlueprint.h"

DEFINE_LOG_CATEGORY_STATIC(LogUIWTDesignToolset, Log, All);

namespace
{
  using namespace UIWTDesignTree;

  // What to do with a fetched frame; returns the tool's value, or sets
  // OutError.
  using FAfterFetch =
      TFunction<FString(const UIWTFigmaClient::FFetchResult &, FString &OutError)>;

  // An async tool result completed once the fetch (and InAfter) are done.
  // With bInFonts, the design's fonts are found or downloaded between the
  // two (UIWTDesignFonts::EnsureFonts), and the value becomes
  // {"fonts": ..., "result": <InAfter's value>}.
  UToolCallAsyncResultString *FetchThen(const FString &InUrl, bool bInRefresh, bool bInFonts,
                                        FAfterFetch InAfter)
  {
    UToolCallAsyncResultString *Result = NewObject<UToolCallAsyncResultString>();
    TStrongObjectPtr<UToolCallAsyncResultString> Keep(Result);
    UIWTFigmaClient::Fetch(
        InUrl, bInRefresh,
        [Keep, bInFonts,
         After = MoveTemp(InAfter)](const UIWTFigmaClient::FFetchResult &InFetched)
        {
          if (!InFetched.Error.IsEmpty())
          {
            Keep->SetError(InFetched.Error);
            return;
          }
          auto Finish = [Keep, After, InFetched](const FString &InFontsJson)
          {
            FString Error;
            const FString Value = After(InFetched, Error);
            if (!Error.IsEmpty())
            {
              Keep->SetError(Error);
            }
            else if (InFontsJson.IsEmpty())
            {
              Keep->SetValue(Value);
            }
            else
            {
              Keep->SetValue(FString::Printf(TEXT("{\"fonts\": %s, \"result\": %s}"),
                                             *InFontsJson, *Value));
            }
          };
          if (!bInFonts)
          {
            Finish(FString());
            return;
          }
          UIWTDesignFonts::EnsureFonts(InFetched.DesignFile,
                                       [Finish](const UIWTDesignFonts::FFontsResult &InFonts)
                                       { Finish(UIWTDesignFonts::ResultToJson(InFonts)); });
        });
    return Result;
  }

  using UIWTDesignSources::FPsdExport;
  using UIWTDesignSources::PreparePsd;
  using UIWTDesignRefine::LoadDesign;

  // The reply when the design hasn't changed since the last import: InField
  // (version or exportedAt) and why nothing was re-imported.
  FString UpToDateJson(const TCHAR *InField, const FString &InValue, const FString &InNote)
  {
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetBoolField(TEXT("upToDate"), true);
    Object->SetStringField(InField, InValue);
    Object->SetStringField(TEXT("note"), InNote);
    return UIWTJson::Pretty(Object);
  }

  // A notification of the fonts a design import took from this computer,
  // downloaded or couldn't find; none when there were none.
  void NotifyFonts(const UIWTDesignFonts::FFontsResult &InFonts)
  {
    const FString Fonts = UIWTDesignFonts::Summary(InFonts);
    if (!Fonts.IsEmpty())
    {
      UIWTNotify::Show(FText::FromString(Fonts), InFonts.Failed.IsEmpty());
    }
  }

  // (JSON, error) once a re-import is planned, applied or refused.
  using FOnReimported = TFunction<void(const FString &, const FString &)>;

  // Plans the re-import of InBlueprint from InDesignFile, applies it when
  // bInApply, and accepts (saves) it too when bInAccept.
  void PlanApply(UWidgetBlueprint *InBlueprint, const FString &InDesignFile,
                 const FString &InReference, bool bInApply, bool bInAccept,
                 const FOnReimported &InOnDone)
  {
    UIWTDesignImport::FReimportRequest Request;
    Request.Blueprint = InBlueprint;
    Request.DesignFile = InDesignFile;
    Request.ReferenceImage = InReference;
    UIWTDesignImport::FReimportPlan Plan;
    FString Error;
    if (!UIWTDesignImport::PlanReimport(Request, Plan, Error))
    {
      InOnDone(FString(), Error);
      return;
    }
    const FString PlanJson = UIWTDesignImport::PlanToJson(Plan);
    if (!bInApply)
    {
      InOnDone(PlanJson, FString());
      return;
    }
    UIWTDesignImport::FReimportResult Applied;
    if (!UIWTDesignImport::ApplyReimport(Plan, Applied, Error))
    {
      InOnDone(FString(), Error);
      return;
    }
    if (bInAccept && !UIWTDesignImport::AcceptReimport(InBlueprint, true, Error))
    {
      InOnDone(FString(), TEXT("Applied, but accepting failed: ") + Error);
      return;
    }
    InOnDone(FString::Printf(TEXT("{\"plan\": %s, \"applied\": %s, \"accepted\": %s}"), *PlanJson,
                             *UIWTDesignImport::ReimportResultToJson(Applied),
                             bInAccept ? TEXT("true") : TEXT("false")),
             FString());
  }

  // Gets the design InBlueprint was imported from as it is now (fetching a
  // Figma frame again, reading a Photoshop export again), then plans and
  // maybe applies the re-import. A Figma file whose version hasn't changed,
  // or the Photoshop export already imported, is reported up to date unless
  // bInForce. InOnDone runs later, on the game thread.
  void RunReimport(UWidgetBlueprint *InBlueprint, bool bInApply, bool bInForce, bool bInAccept,
                   FOnReimported InOnDone)
  {
    auto Later = [](FOnReimported InDone, FString InJson, FString InError)
    {
      FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
          [InDone, InJson, InError](float)
          {
            InDone(InJson, InError);
            return false;
          }));
    };
    UIWTDesignImport::FSidecar Sidecar;
    FString Error;
    if (!InBlueprint)
    {
      Later(MoveTemp(InOnDone), FString(), TEXT("No Widget Blueprint given."));
      return;
    }
    if (!UIWTDesignImport::ReadSidecar(InBlueprint->GetOutermost()->GetName(), Sidecar, Error))
    {
      Later(MoveTemp(InOnDone), FString(), Error);
      return;
    }
    if (!Sidecar.ComponentKey.IsEmpty())
    {
      Later(MoveTemp(InOnDone), FString(),
            TEXT("This is a child WBP made from a component; it is updated by re-importing a "
                 "screen that uses the component, when the component's hash changed."));
      return;
    }
    FString FileKey;
    FString NodeId;
    FString Version;
    if (Sidecar.SourceRef.IsValid())
    {
      Sidecar.SourceRef->TryGetStringField(TEXT("fileKey"), FileKey);
      Sidecar.SourceRef->TryGetStringField(TEXT("nodeId"), NodeId);
      Sidecar.SourceRef->TryGetStringField(TEXT("version"), Version);
    }
    const TWeakObjectPtr<UWidgetBlueprint> Blueprint(InBlueprint);
    FString Manifest;
    FString ExportedAt;
    if (Sidecar.Source == TEXT("psd") && Sidecar.SourceRef.IsValid() &&
        Sidecar.SourceRef->TryGetStringField(TEXT("manifest"), Manifest))
    {
      // A Photoshop export: its manifest folder as it is now.
      Sidecar.SourceRef->TryGetStringField(TEXT("exportedAt"), ExportedAt);
      FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
          [Blueprint, Manifest, ExportedAt, bInApply, bInForce, bInAccept, InOnDone](float)
          {
            if (!Blueprint.IsValid())
            {
              InOnDone(FString(), TEXT("The blueprint is gone."));
              return false;
            }
            FPsdExport Export;
            FString Error;
            if (!PreparePsd(Manifest, Export, Error))
            {
              InOnDone(FString(), Error);
              return false;
            }
            if (!bInForce && !ExportedAt.IsEmpty() && Export.Read.ExportedAt == ExportedAt)
            {
              InOnDone(UpToDateJson(TEXT("exportedAt"), ExportedAt,
                                    TEXT("This Photoshop export was already imported. Export "
                                         "for Unreal again after changing the PSD. Force "
                                         "re-imports anyway, for example to bring back widgets "
                                         "deleted in UE.")),
                       FString());
              return false;
            }
            // The design's fonts first, so the conversion uses them.
            UIWTDesignFonts::EnsureFonts(
                Export.DesignFile,
                [Blueprint, DesignFile = Export.DesignFile, Reference = Export.ReferenceImage,
                 bInApply, bInAccept, InOnDone](const UIWTDesignFonts::FFontsResult &InFonts)
                {
                  if (!Blueprint.IsValid())
                  {
                    InOnDone(FString(), TEXT("The blueprint is gone."));
                    return;
                  }
                  NotifyFonts(InFonts);
                  PlanApply(Blueprint.Get(), DesignFile, Reference, bInApply, bInAccept,
                            InOnDone);
                });
            return false;
          }));
      return;
    }
    if (Sidecar.Source != TEXT("figma") || FileKey.IsEmpty() || NodeId.IsEmpty())
    {
      // Another source: its design file as it is on disk now.
      FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
          [Blueprint, DesignFile = Sidecar.DesignFile, bInApply, bInAccept, InOnDone](float)
          {
            if (!Blueprint.IsValid())
            {
              InOnDone(FString(), TEXT("The blueprint is gone."));
              return false;
            }
            PlanApply(Blueprint.Get(), DesignFile, FString(), bInApply, bInAccept, InOnDone);
            return false;
          }));
      return;
    }
    const FString Url = FString::Printf(TEXT("https://www.figma.com/design/%s/reimport?node-id=%s"),
                                        *FileKey, *NodeId);
    UIWTFigmaClient::Fetch(
        Url, false,
        [Blueprint, Version, bInApply, bInForce, bInAccept,
         InOnDone](const UIWTFigmaClient::FFetchResult &InFetched)
        {
          if (!InFetched.Error.IsEmpty())
          {
            InOnDone(FString(), InFetched.Error);
            return;
          }
          if (!Blueprint.IsValid())
          {
            InOnDone(FString(), TEXT("The blueprint is gone."));
            return;
          }
          if (!bInForce && !Version.IsEmpty() && InFetched.Version == Version)
          {
            InOnDone(UpToDateJson(TEXT("version"), Version,
                                  TEXT("The Figma file hasn't changed since the last import, "
                                       "so there is nothing to re-import. Force re-imports "
                                       "anyway, for example to bring back widgets deleted in "
                                       "UE.")),
                     FString());
            return;
          }
          // The design's fonts first, so the conversion uses them.
          UIWTDesignFonts::EnsureFonts(
              InFetched.DesignFile,
              [Blueprint, InFetched, bInApply, bInAccept,
               InOnDone](const UIWTDesignFonts::FFontsResult &InFonts)
              {
                if (!Blueprint.IsValid())
                {
                  InOnDone(FString(), TEXT("The blueprint is gone."));
                  return;
                }
                NotifyFonts(InFonts);
                PlanApply(Blueprint.Get(), InFetched.DesignFile, InFetched.ReferenceImage,
                          bInApply, bInAccept, InOnDone);
              });
        });
  }

  // UIWT.Reimport <blueprint path> [apply]
  FAutoConsoleCommand ReimportCommand(
      TEXT("UIWT.Reimport"),
      TEXT("Re-imports a blueprint a design import made, keeping what was changed in UE. "
           "Without 'apply' it only logs the preview; with it, it applies and saves. "
           "UIWT.Reimport <blueprint path> [apply] [force]"),
      FConsoleCommandWithArgsDelegate::CreateLambda(
          [](const TArray<FString> &InArgs)
          {
            if (InArgs.IsEmpty())
            {
              UE_LOG(LogUIWTDesignToolset, Display,
                     TEXT("UIWT.Reimport <blueprint path> [apply] [force]"));
              return;
            }
            FString Path = InArgs[0].TrimQuotes();
            if (!Path.Contains(TEXT(".")))
            {
              Path += TEXT(".") + FPackageName::GetShortName(Path);
            }
            UWidgetBlueprint *Blueprint = LoadObject<UWidgetBlueprint>(nullptr, *Path);
            const bool bApply = InArgs.Contains(TEXT("apply"));
            const bool bForce = InArgs.Contains(TEXT("force"));
            RunReimport(Blueprint, bApply, bForce, bApply,
                        [](const FString &InJson, const FString &InError)
                        {
                          if (!InError.IsEmpty())
                          {
                            UE_LOG(LogUIWTDesignToolset, Error, TEXT("Re-import failed: %s"),
                                   *InError);
                            UIWTNotify::Show(FText::FromString(TEXT("Re-import failed: ") + InError),
                                             false);
                            return;
                          }
                          UE_LOG(LogUIWTDesignToolset, Display, TEXT("%s"), *InJson);
                          UIWTNotify::Show(FText::FromString(TEXT("Re-import done; see the log.")),
                                           true);
                        });
          }));

  // UIWT.EnsureFonts <design.json>
  FAutoConsoleCommand EnsureFontsCommand(
      TEXT("UIWT.EnsureFonts"),
      TEXT("Finds the fonts a design tree uses in the font map or font library and downloads "
           "the missing ones from Google Fonts into the library, without importing. A Figma "
           "frame's design.json is under Saved/UIWidgetTool/Figma. "
           "UIWT.EnsureFonts <design.json>"),
      FConsoleCommandWithArgsDelegate::CreateLambda(
          [](const TArray<FString> &InArgs)
          {
            if (InArgs.IsEmpty())
            {
              UE_LOG(LogUIWTDesignToolset, Display, TEXT("UIWT.EnsureFonts <design.json>"));
              return;
            }
            UIWTDesignFonts::EnsureFonts(
                FPaths::ConvertRelativePathToFull(InArgs[0].TrimQuotes()),
                [](const UIWTDesignFonts::FFontsResult &InFonts)
                {
                  UE_LOG(LogUIWTDesignToolset, Display, TEXT("Fonts: %s"),
                         *UIWTDesignFonts::ResultToJson(InFonts));
                  const FString Summary = UIWTDesignFonts::Summary(InFonts);
                  UIWTNotify::Show(FText::FromString(Summary.IsEmpty()
                                                         ? FString(TEXT("Every font the design "
                                                                        "uses is in the project."))
                                                         : Summary),
                                   InFonts.Failed.IsEmpty());
                });
          }));

  // What a first import of InDoc would build, without creating anything:
  // blueprintName, spec, report, widgets and depth, added to OutObject.
  void AddSpecPreview(const FDocument &InDoc, FJsonObject &OutObject)
  {
    const FString Name = UIWTDesignImport::DefaultBlueprintName(InDoc);
    const FConvertResult Converted =
        Convert(InDoc, UIWTDesignImport::FirstImportOptions(
                           UUIWTDesignSettings::Get()->GetImportFolder(), Name,
                           UUserWidget::StaticClass()));
    OutObject.SetStringField(TEXT("blueprintName"), Name);
    if (Converted.Spec.IsValid())
    {
      OutObject.SetObjectField(TEXT("spec"), Converted.Spec);
    }
    TArray<TSharedPtr<FJsonValue>> Report;
    for (const FReportEntry &Entry : Converted.Report)
    {
      TSharedRef<FJsonObject> EntryObject = MakeShared<FJsonObject>();
      EntryObject->SetStringField(TEXT("node"), Entry.Node);
      EntryObject->SetStringField(TEXT("category"), Entry.Category);
      EntryObject->SetStringField(TEXT("detail"), Entry.Detail);
      Report.Add(MakeShared<FJsonValueObject>(EntryObject));
    }
    OutObject.SetArrayField(TEXT("report"), Report);
    OutObject.SetNumberField(TEXT("widgets"), Converted.WidgetCount);
    OutObject.SetNumberField(TEXT("depth"), Converted.MaxDepth + 1);
  }
}

UToolCallAsyncResultString *UUIWTDesignToolset::FetchFigmaNode(const FString &Url, bool bRefresh)
{
  return FetchThen(Url, bRefresh, false,
                   [](const UIWTFigmaClient::FFetchResult &InFetched, FString &)
                   { return UIWTFigmaClient::ResultToJson(InFetched); });
}

UToolCallAsyncResultString *UUIWTDesignToolset::ConvertFigmaToSpec(const FString &Url)
{
  // A preview: nothing is downloaded, so fonts the import would download are
  // reported as fontUnmapped here.
  return FetchThen(
      Url, false, false,
      [](const UIWTFigmaClient::FFetchResult &InFetched, FString &OutError) -> FString
      {
        FString Json;
        FDocument Doc;
        TArray<FString> Errors;
        if (!FFileHelper::LoadFileToString(Json, *InFetched.DesignFile) ||
            !ReadDocument(Json, Doc, Errors))
        {
          OutError = TEXT("The fetched design.json couldn't be read: ") +
                     FString::Join(Errors, TEXT("; "));
          return FString();
        }
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("designFile"), InFetched.DesignFile);
        AddSpecPreview(Doc, *Object);
        return UIWTJson::Pretty(Object);
      });
}

UToolCallAsyncResultString *UUIWTDesignToolset::ImportFigmaFrame(const FString &Url,
                                                                 const FString &TargetFolder,
                                                                 const FString &BlueprintName)
{
  return FetchThen(
      Url, false, true,
      [TargetFolder, BlueprintName](const UIWTFigmaClient::FFetchResult &InFetched,
                                    FString &OutError) -> FString
      {
        UIWTDesignImport::FRequest Request;
        Request.DesignFile = InFetched.DesignFile;
        Request.TargetFolder = TargetFolder;
        Request.BlueprintName = BlueprintName;
        Request.ReferenceImage = InFetched.ReferenceImage;
        UIWTDesignImport::FResult Imported;
        if (!UIWTDesignImport::Import(Request, Imported, OutError))
        {
          return FString();
        }
        return UIWTDesignImport::ResultToJson(Imported);
      });
}

FString UUIWTDesignToolset::ImportDesignTree(const FString &DesignFile,
                                             const FString &TargetFolder,
                                             const FString &BlueprintName)
{
  if (!UIWTPaths::IsInsideDirectory(DesignFile, FPaths::ProjectDir()))
  {
    UKismetSystemLibrary::RaiseScriptError(
        TEXT("DesignFile must be inside the project folder (the Figma cache is under "
             "Saved/UIWidgetTool/Figma)."));
    return FString();
  }
  UIWTDesignImport::FRequest Request;
  Request.DesignFile = DesignFile;
  Request.TargetFolder = TargetFolder;
  Request.BlueprintName = BlueprintName;
  UIWTDesignImport::FResult Imported;
  FString Error;
  if (!UIWTDesignImport::Import(Request, Imported, Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
    return FString();
  }
  return UIWTDesignImport::ResultToJson(Imported);
}

FString UUIWTDesignToolset::ConvertPsdManifestToSpec(const FString &ManifestFile)
{
  FPsdExport Export;
  FString Error;
  if (!PreparePsd(ManifestFile, Export, Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
    return FString();
  }
  TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetStringField(TEXT("designFile"), Export.DesignFile);
  Object->SetStringField(TEXT("referenceImage"), Export.ReferenceImage);
  Object->SetStringField(TEXT("exportedAt"), Export.Read.ExportedAt);
  Object->SetNumberField(TEXT("nodes"), Export.Read.NodeCount);
  AddSpecPreview(Export.Read.Document, *Object);
  return UIWTJson::Pretty(Object);
}

UToolCallAsyncResultString *UUIWTDesignToolset::ImportPsdManifest(const FString &ManifestFile,
                                                                  const FString &TargetFolder,
                                                                  const FString &BlueprintName)
{
  UToolCallAsyncResultString *Result = NewObject<UToolCallAsyncResultString>();
  FPsdExport Export;
  FString Error;
  if (!PreparePsd(ManifestFile, Export, Error))
  {
    Result->SetError(Error);
    return Result;
  }
  UIWTDesignImport::FRequest Request;
  Request.DesignFile = Export.DesignFile;
  Request.ReferenceImage = Export.ReferenceImage;
  Request.TargetFolder = TargetFolder;
  Request.BlueprintName = BlueprintName;
  // The design's fonts first, so the conversion uses them.
  UIWTDesignFonts::EnsureFonts(
      Request.DesignFile,
      [Keep = TStrongObjectPtr<UToolCallAsyncResultString>(Result),
       Request](const UIWTDesignFonts::FFontsResult &InFonts)
      {
        UIWTDesignImport::FResult Imported;
        FString ImportError;
        if (!UIWTDesignImport::Import(Request, Imported, ImportError))
        {
          Keep->SetError(ImportError);
          return;
        }
        Keep->SetValue(FString::Printf(TEXT("{\"fonts\": %s, \"result\": %s}"),
                                       *UIWTDesignFonts::ResultToJson(InFonts),
                                       *UIWTDesignImport::ResultToJson(Imported)));
      });
  return Result;
}

FString UUIWTDesignToolset::GetDesignNode(UWidgetBlueprint *WidgetBlueprint,
                                          const FString &NodeId, int32 Depth)
{
  UIWTDesignImport::FSidecar Sidecar;
  FDocument Doc;
  FString Error;
  if (!LoadDesign(WidgetBlueprint, Sidecar, Doc, Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
    return FString();
  }
  const FNode *Node = UIWTDesignRefine::FindNode(Doc, NodeId);
  if (!Node)
  {
    UKismetSystemLibrary::RaiseScriptError(
        FString::Printf(TEXT("The design has no node %s."), *NodeId));
    return FString();
  }
  return UIWTDesignRefine::NodeJson(*Node, FMath::Max(Depth, 0));
}

FString UUIWTDesignToolset::GetDesignOutline(UWidgetBlueprint *WidgetBlueprint,
                                             const FString &NodeId)
{
  UIWTDesignImport::FSidecar Sidecar;
  FDocument Doc;
  FString Error;
  if (!LoadDesign(WidgetBlueprint, Sidecar, Doc, Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
    return FString();
  }
  const FNode *Node = NodeId.IsEmpty() ? &Doc.Root : UIWTDesignRefine::FindNode(Doc, NodeId);
  if (!Node)
  {
    UKismetSystemLibrary::RaiseScriptError(
        FString::Printf(TEXT("The design has no node %s."), *NodeId));
    return FString();
  }
  return UIWTDesignRefine::MakeOutline(*Node, Sidecar.Owned);
}

UToolCallAsyncResultString *UUIWTDesignToolset::ReimportDesign(UWidgetBlueprint *WidgetBlueprint,
                                                               bool bApply, bool bForce)
{
  UToolCallAsyncResultString *Result = NewObject<UToolCallAsyncResultString>();
  TStrongObjectPtr<UToolCallAsyncResultString> Keep(Result);
  RunReimport(WidgetBlueprint, bApply, bForce, false,
              [Keep](const FString &InJson, const FString &InError)
              {
                if (InError.IsEmpty())
                {
                  Keep->SetValue(InJson);
                }
                else
                {
                  Keep->SetError(InError);
                }
              });
  return Result;
}

FString UUIWTDesignToolset::AcceptReimport(UWidgetBlueprint *WidgetBlueprint)
{
  FString Error;
  if (!UIWTDesignImport::AcceptReimport(WidgetBlueprint, true, Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
    return FString();
  }
  FString Result = FString::Printf(TEXT("Saved %s and its textures, and updated its design sidecar."),
                                   *WidgetBlueprint->GetPathName());
  UIWTDesignImport::FSidecar Sidecar;
  if (UIWTDesignImport::ReadSidecar(WidgetBlueprint->GetOutermost()->GetName(), Sidecar, Error) &&
      !Sidecar.ChangedNodes.IsEmpty())
  {
    Result += FString::Printf(
        TEXT(" The re-import added or changed %d design nodes; the user can refine those parts "
             "with the chat panel's + menu > AI pass: changed parts."),
        Sidecar.ChangedNodes.Num());
  }
  return Result;
}

FString UUIWTDesignToolset::DiscardReimport(UWidgetBlueprint *WidgetBlueprint)
{
  FString Error;
  if (!UIWTDesignImport::DiscardReimport(WidgetBlueprint, Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
    return FString();
  }
  return TEXT("Restored the blueprint and its textures from disk; the re-import is undone.");
}
