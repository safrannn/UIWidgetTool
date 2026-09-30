#include "UIWTDesignToolset.h"

#include "Blueprint/UserWidget.h"
#include "Containers/Ticker.h"
#include "Core/UIWTDesignImport.h"
#include "Core/UIWTDesignSettings.h"
#include "Core/UIWTNotify.h"
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
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "WidgetBlueprint.h"

DEFINE_LOG_CATEGORY_STATIC(LogUIWTDesignToolset, Log, All);

namespace
{
  using namespace UIWTDesignTree;

  FString PrettyJson(const TSharedRef<FJsonObject> &InObject)
  {
    FString Json;
    TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);
    FJsonSerializer::Serialize(InObject, Writer);
    return Json;
  }

  bool IsInsideDirectory(const FString &InPath, const FString &InDirectory)
  {
    FString Path = FPaths::ConvertRelativePathToFull(InPath);
    FString Directory = FPaths::ConvertRelativePathToFull(InDirectory);
    FPaths::NormalizeFilename(Path);
    FPaths::NormalizeDirectoryName(Directory);
    FPaths::CollapseRelativeDirectories(Path);
    return Path.StartsWith(Directory + TEXT("/"), ESearchCase::IgnoreCase);
  }

  // What to do with a fetched frame; returns the tool's value, or sets
  // OutError.
  using FAfterFetch =
      TFunction<FString(const UIWTFigmaClient::FFetchResult &, FString &OutError)>;

  // An async tool result completed once the fetch (and InAfter) are done.
  UToolCallAsyncResultString *FetchThen(const FString &InUrl, bool bInRefresh,
                                        FAfterFetch InAfter)
  {
    UToolCallAsyncResultString *Result = NewObject<UToolCallAsyncResultString>();
    TStrongObjectPtr<UToolCallAsyncResultString> Keep(Result);
    UIWTFigmaClient::Fetch(
        InUrl, bInRefresh,
        [Keep, After = MoveTemp(InAfter)](const UIWTFigmaClient::FFetchResult &InFetched)
        {
          if (!InFetched.Error.IsEmpty())
          {
            Keep->SetError(InFetched.Error);
            return;
          }
          FString Error;
          const FString Value = After(InFetched, Error);
          if (Error.IsEmpty())
          {
            Keep->SetValue(Value);
          }
          else
          {
            Keep->SetError(Error);
          }
        });
    return Result;
  }

  using UIWTDesignSources::FPsdExport;
  using UIWTDesignSources::PreparePsd;

  // The design InBlueprint was imported from, and its sidecar.
  bool LoadDesign(UWidgetBlueprint *InBlueprint, UIWTDesignImport::FSidecar &OutSidecar,
                  FDocument &OutDocument, FString &OutError)
  {
    if (!InBlueprint)
    {
      OutError = TEXT("No Widget Blueprint given.");
      return false;
    }
    if (!UIWTDesignImport::ReadSidecar(InBlueprint->GetOutermost()->GetName(), OutSidecar,
                                       OutError))
    {
      return false;
    }
    FString Json;
    TArray<FString> Errors;
    if (!FFileHelper::LoadFileToString(Json, *OutSidecar.DesignFile) ||
        !ReadDocument(Json, OutDocument, Errors))
    {
      OutError = FString::Printf(TEXT("The design %s can't be read. %s"),
                                 *OutSidecar.DesignFile, *FString::Join(Errors, TEXT("; ")));
      return false;
    }
    return true;
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
              InOnDone(FString::Printf(
                           TEXT("{\"upToDate\": true, \"exportedAt\": \"%s\", \"note\": \"This "
                                "Photoshop export was already imported. Export for Unreal again "
                                "after changing the PSD. Force re-imports anyway, for example to "
                                "bring back widgets deleted in UE.\"}"),
                           *ExportedAt.ReplaceCharWithEscapedChar()),
                       FString());
              return false;
            }
            PlanApply(Blueprint.Get(), Export.DesignFile, Export.ReferenceImage, bInApply,
                      bInAccept, InOnDone);
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
            InOnDone(FString::Printf(
                         TEXT("{\"upToDate\": true, \"version\": \"%s\", \"note\": \"The Figma "
                              "file hasn't changed since the last import, so there is nothing "
                              "to re-import. Force re-imports anyway, for example to bring back "
                              "widgets deleted in UE.\"}"),
                         *Version),
                     FString());
            return;
          }
          PlanApply(Blueprint.Get(), InFetched.DesignFile, InFetched.ReferenceImage, bInApply,
                    bInAccept, InOnDone);
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

  // UIWT.ImportFigma <frame link> [/Game/Folder] [BlueprintName]
  FAutoConsoleCommand ImportFigmaCommand(
      TEXT("UIWT.ImportFigma"),
      TEXT("Imports a Figma frame as a new Widget Blueprint. "
           "UIWT.ImportFigma <frame link> [/Game/Folder] [BlueprintName]"),
      FConsoleCommandWithArgsDelegate::CreateLambda(
          [](const TArray<FString> &InArgs)
          {
            if (InArgs.IsEmpty())
            {
              UE_LOG(LogUIWTDesignToolset, Display,
                     TEXT("UIWT.ImportFigma <frame link> [/Game/Folder] [BlueprintName]"));
              return;
            }
            UIWTDesignImport::FRequest Request;
            Request.TargetFolder = InArgs.IsValidIndex(1) ? InArgs[1].TrimQuotes() : FString();
            Request.BlueprintName = InArgs.IsValidIndex(2) ? InArgs[2].TrimQuotes() : FString();
            UIWTNotify::Show(FText::FromString(TEXT("Fetching the Figma frame...")), true);
            UIWTFigmaClient::Fetch(
                InArgs[0].TrimQuotes(), false,
                [Request](const UIWTFigmaClient::FFetchResult &InFetched) mutable
                {
                  FString Error = InFetched.Error;
                  UIWTDesignImport::FResult Imported;
                  if (Error.IsEmpty())
                  {
                    Request.DesignFile = InFetched.DesignFile;
                    Request.ReferenceImage = InFetched.ReferenceImage;
                    UIWTDesignImport::Import(Request, Imported, Error);
                  }
                  if (!Error.IsEmpty())
                  {
                    UE_LOG(LogUIWTDesignToolset, Error, TEXT("Figma import failed: %s"), *Error);
                    UIWTNotify::Show(FText::FromString(TEXT("Figma import failed: ") + Error),
                                     false);
                    return;
                  }
                  UE_LOG(LogUIWTDesignToolset, Display, TEXT("%s\n%s"),
                         *UIWTFigmaClient::ResultToJson(InFetched),
                         *UIWTDesignImport::ResultToJson(Imported));
                  UIWTNotify::Show(FText::FromString(FString::Printf(
                                       TEXT("Imported %s (%d widgets, %d report entries)."),
                                       *Imported.BlueprintPath, Imported.WidgetCount,
                                       Imported.Report.Num())),
                                   true);
                });
          }));

  // UIWT.ImportPsd <manifest.json> [/Game/Folder] [BlueprintName]
  FAutoConsoleCommand ImportPsdCommand(
      TEXT("UIWT.ImportPsd"),
      TEXT("Imports a Photoshop export (Export for Unreal) as a new Widget Blueprint. "
           "UIWT.ImportPsd <manifest.json> [/Game/Folder] [BlueprintName]"),
      FConsoleCommandWithArgsDelegate::CreateLambda(
          [](const TArray<FString> &InArgs)
          {
            if (InArgs.IsEmpty())
            {
              UE_LOG(LogUIWTDesignToolset, Display,
                     TEXT("UIWT.ImportPsd <manifest.json> [/Game/Folder] [BlueprintName]"));
              return;
            }
            FPsdExport Export;
            UIWTDesignImport::FResult Imported;
            FString Error;
            if (PreparePsd(InArgs[0], Export, Error))
            {
              UIWTDesignImport::FRequest Request;
              Request.DesignFile = Export.DesignFile;
              Request.ReferenceImage = Export.ReferenceImage;
              Request.TargetFolder = InArgs.IsValidIndex(1) ? InArgs[1].TrimQuotes() : FString();
              Request.BlueprintName = InArgs.IsValidIndex(2) ? InArgs[2].TrimQuotes() : FString();
              UIWTDesignImport::Import(Request, Imported, Error);
            }
            if (!Error.IsEmpty())
            {
              UE_LOG(LogUIWTDesignToolset, Error, TEXT("Photoshop import failed: %s"), *Error);
              UIWTNotify::Show(FText::FromString(TEXT("Photoshop import failed: ") + Error), false);
              return;
            }
            UE_LOG(LogUIWTDesignToolset, Display, TEXT("%s"),
                   *UIWTDesignImport::ResultToJson(Imported));
            UIWTNotify::Show(FText::FromString(FString::Printf(
                                 TEXT("Imported %s (%d widgets, %d report entries)."),
                                 *Imported.BlueprintPath, Imported.WidgetCount,
                                 Imported.Report.Num())),
                             true);
          }));

  TArray<TSharedPtr<FJsonValue>> ReportJson(const TArray<FReportEntry> &InReport)
  {
    TArray<TSharedPtr<FJsonValue>> Entries;
    for (const FReportEntry &Entry : InReport)
    {
      TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
      Object->SetStringField(TEXT("node"), Entry.Node);
      Object->SetStringField(TEXT("category"), Entry.Category);
      Object->SetStringField(TEXT("detail"), Entry.Detail);
      Entries.Add(MakeShared<FJsonValueObject>(Object));
    }
    return Entries;
  }
}

UToolCallAsyncResultString *UUIWTDesignToolset::FetchFigmaNode(const FString &Url, bool bRefresh)
{
  return FetchThen(Url, bRefresh,
                   [](const UIWTFigmaClient::FFetchResult &InFetched, FString &)
                   { return UIWTFigmaClient::ResultToJson(InFetched); });
}

UToolCallAsyncResultString *UUIWTDesignToolset::ConvertFigmaToSpec(const FString &Url)
{
  return FetchThen(
      Url, false,
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
        const FString Name = UIWTDesignImport::DefaultBlueprintName(Doc);
        const FConvertResult Converted =
            Convert(Doc, UIWTDesignImport::FirstImportOptions(
                             UUIWTDesignSettings::Get()->GetImportFolder(), Name,
                             UUserWidget::StaticClass()));
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("designFile"), InFetched.DesignFile);
        Object->SetStringField(TEXT("blueprintName"), Name);
        if (Converted.Spec.IsValid())
        {
          Object->SetObjectField(TEXT("spec"), Converted.Spec);
        }
        Object->SetArrayField(TEXT("report"), ReportJson(Converted.Report));
        Object->SetNumberField(TEXT("widgets"), Converted.WidgetCount);
        Object->SetNumberField(TEXT("depth"), Converted.MaxDepth + 1);
        return PrettyJson(Object);
      });
}

UToolCallAsyncResultString *UUIWTDesignToolset::ImportFigmaFrame(const FString &Url,
                                                                 const FString &TargetFolder,
                                                                 const FString &BlueprintName)
{
  return FetchThen(
      Url, false,
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
  if (!IsInsideDirectory(DesignFile, FPaths::ProjectDir()))
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
  const FDocument &Doc = Export.Read.Document;
  const FString Name = UIWTDesignImport::DefaultBlueprintName(Doc);
  const FConvertResult Converted =
      Convert(Doc, UIWTDesignImport::FirstImportOptions(
                       UUIWTDesignSettings::Get()->GetImportFolder(), Name,
                       UUserWidget::StaticClass()));
  TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetStringField(TEXT("designFile"), Export.DesignFile);
  Object->SetStringField(TEXT("referenceImage"), Export.ReferenceImage);
  Object->SetStringField(TEXT("exportedAt"), Export.Read.ExportedAt);
  Object->SetNumberField(TEXT("nodes"), Export.Read.NodeCount);
  Object->SetStringField(TEXT("blueprintName"), Name);
  if (Converted.Spec.IsValid())
  {
    Object->SetObjectField(TEXT("spec"), Converted.Spec);
  }
  Object->SetArrayField(TEXT("report"), ReportJson(Converted.Report));
  Object->SetNumberField(TEXT("widgets"), Converted.WidgetCount);
  Object->SetNumberField(TEXT("depth"), Converted.MaxDepth + 1);
  return PrettyJson(Object);
}

FString UUIWTDesignToolset::ImportPsdManifest(const FString &ManifestFile,
                                              const FString &TargetFolder,
                                              const FString &BlueprintName)
{
  FPsdExport Export;
  FString Error;
  UIWTDesignImport::FResult Imported;
  if (PreparePsd(ManifestFile, Export, Error))
  {
    UIWTDesignImport::FRequest Request;
    Request.DesignFile = Export.DesignFile;
    Request.ReferenceImage = Export.ReferenceImage;
    Request.TargetFolder = TargetFolder;
    Request.BlueprintName = BlueprintName;
    UIWTDesignImport::Import(Request, Imported, Error);
  }
  if (!Error.IsEmpty())
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
    return FString();
  }
  return UIWTDesignImport::ResultToJson(Imported);
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
