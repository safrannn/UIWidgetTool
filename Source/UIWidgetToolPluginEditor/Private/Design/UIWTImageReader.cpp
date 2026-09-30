#include "UIWTImageReader.h"

#include "Claude/UIWTChatTypes.h"
#include "Claude/UIWTClaudeService.h"
#include "Core/UIWTDesignImport.h"
#include "Core/UIWTDesignSettings.h"
#include "Core/UIWTRunImages.h"
#include "Design/UIWTDesignRefine.h"
#include "Design/UIWTPsdManifest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

namespace
{
  using namespace UIWTDesignTree;

  // Report entries WriteDesignTree's reply lists; the rest are counted.
  constexpr int32 MaxReplyEntries = 40;

  FString SafeName(const FString &InName)
  {
    FString Name;
    for (const TCHAR Char : InName)
    {
      Name.AppendChar(FChar::IsAlnum(Char) || Char == TEXT('-') ? Char : TEXT('_'));
    }
    return Name.IsEmpty() ? FString(TEXT("image")) : Name;
  }

  double Round2(double InValue)
  {
    return FMath::RoundToDouble(InValue * 100.0) / 100.0;
  }

  // Divides the number fields InFields of InObject by InScale.
  void ScaleFields(const TSharedPtr<FJsonObject> &InObject, std::initializer_list<const TCHAR *> InFields,
                   double InScale)
  {
    if (!InObject.IsValid())
    {
      return;
    }
    for (const TCHAR *Field : InFields)
    {
      double Value = 0.0;
      if (InObject->TryGetNumberField(Field, Value))
      {
        InObject->SetNumberField(Field, Round2(Value / InScale));
      }
    }
  }

  TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject> &InObject, const TCHAR *InField)
  {
    const TSharedPtr<FJsonObject> *Value = nullptr;
    return InObject.IsValid() && InObject->TryGetObjectField(InField, Value) ? *Value : nullptr;
  }

  struct FLocalizer
  {
    FString CacheDir;
    double Scale = 1.0;
    TArray<FString> &Errors;

    // InNode's box from absolute image pixels to design pixels relative to
    // its parent (at InParentX, InParentY in image pixels), and every other
    // length with it.
    void Node(const TSharedPtr<FJsonObject> &InNode, double InParentX, double InParentY,
              bool bInRoot, const FString &InPath)
    {
      FString Id;
      InNode->TryGetStringField(TEXT("id"), Id);
      const FString Where = Id.IsEmpty() ? InPath : Id;
      const TSharedPtr<FJsonObject> Box = ObjectField(InNode, TEXT("box"));
      double X = 0.0, Y = 0.0, W = 0.0, H = 0.0;
      if (!Box.IsValid() || !Box->TryGetNumberField(TEXT("x"), X) ||
          !Box->TryGetNumberField(TEXT("y"), Y) || !Box->TryGetNumberField(TEXT("w"), W) ||
          !Box->TryGetNumberField(TEXT("h"), H))
      {
        Errors.Add(FString::Printf(TEXT("%s: box needs x, y, w and h"), *Where));
        return;
      }
      Box->SetNumberField(TEXT("x"), bInRoot ? 0.0 : Round2((X - InParentX) / Scale));
      Box->SetNumberField(TEXT("y"), bInRoot ? 0.0 : Round2((Y - InParentY) / Scale));
      Box->SetNumberField(TEXT("w"), Round2(W / Scale));
      Box->SetNumberField(TEXT("h"), Round2(H / Scale));

      const TArray<TSharedPtr<FJsonValue>> *Radii = nullptr;
      if (InNode->TryGetArrayField(TEXT("radii"), Radii))
      {
        TArray<TSharedPtr<FJsonValue>> Scaled;
        for (const TSharedPtr<FJsonValue> &Radius : *Radii)
        {
          Scaled.Add(MakeShared<FJsonValueNumber>(Round2(Radius->AsNumber() / Scale)));
        }
        InNode->SetArrayField(TEXT("radii"), Scaled);
      }
      ScaleFields(ObjectField(ObjectField(InNode, TEXT("stroke")), TEXT("weights")),
                  {TEXT("l"), TEXT("t"), TEXT("r"), TEXT("b")}, Scale);
      const TSharedPtr<FJsonObject> Layout = ObjectField(InNode, TEXT("layout"));
      ScaleFields(Layout, {TEXT("spacing"), TEXT("crossSpacing")}, Scale);
      ScaleFields(ObjectField(Layout, TEXT("padding")), {TEXT("l"), TEXT("t"), TEXT("r"), TEXT("b")},
                  Scale);
      ScaleFields(ObjectField(InNode, TEXT("sizeLimits")),
                  {TEXT("minW"), TEXT("maxW"), TEXT("minH"), TEXT("maxH")}, Scale);

      const TSharedPtr<FJsonObject> Text = ObjectField(InNode, TEXT("text"));
      const TArray<TSharedPtr<FJsonValue>> *Runs = nullptr;
      if (Text.IsValid() && Text->TryGetArrayField(TEXT("runs"), Runs))
      {
        for (const TSharedPtr<FJsonValue> &Run : *Runs)
        {
          const TSharedPtr<FJsonObject> RunObject = Run->AsObject();
          // A lineHeight string is a percentage and stays.
          ScaleFields(RunObject, {TEXT("size"), TEXT("lineHeight")}, Scale);
        }
      }

      const TSharedPtr<FJsonObject> Image = ObjectField(InNode, TEXT("image"));
      if (Image.IsValid())
      {
        FString Path;
        Image->TryGetStringField(TEXT("path"), Path);
        FIntPoint Size = FIntPoint::ZeroValue;
        if (!Path.StartsWith(TEXT("images/")) || Path.Contains(TEXT("..")) ||
            !UIWTPsdManifest::ReadPngSize(CacheDir / Path, Size))
        {
          Errors.Add(FString::Printf(TEXT("%s: image.path must be a PNG CutImageNode made "
                                          "(images/<id>.png); '%s' isn't one"),
                                     *Where, *Path));
        }
        else
        {
          // The crops are at the image's resolution.
          Image->SetNumberField(TEXT("scale"), Scale);
          if (!Image->HasField(TEXT("origin")))
          {
            Image->SetStringField(TEXT("origin"), TEXT("rendered"));
          }
          TSharedRef<FJsonObject> SizeObject = MakeShared<FJsonObject>();
          SizeObject->SetNumberField(TEXT("w"), Size.X);
          SizeObject->SetNumberField(TEXT("h"), Size.Y);
          Image->SetObjectField(TEXT("size"), SizeObject);
        }
      }

      const TArray<TSharedPtr<FJsonValue>> *Children = nullptr;
      if (InNode->TryGetArrayField(TEXT("children"), Children))
      {
        for (int32 Index = 0; Index < Children->Num(); ++Index)
        {
          const TSharedPtr<FJsonObject> Child = (*Children)[Index]->AsObject();
          if (Child.IsValid())
          {
            Node(Child, X, Y, false, FString::Printf(TEXT("%s/children[%d]"), *Where, Index));
          }
        }
      }
    }
  };

  // The reply's report lines.
  FString ReportLines(const TArray<FReportEntry> &InReport)
  {
    FString Lines;
    for (int32 Index = 0; Index < FMath::Min(InReport.Num(), MaxReplyEntries); ++Index)
    {
      const FReportEntry &Entry = InReport[Index];
      Lines += FString::Printf(TEXT("\n- [%s] %s: %s"), *Entry.Category, *Entry.Node,
                               *Entry.Detail);
    }
    if (InReport.Num() > MaxReplyEntries)
    {
      Lines += FString::Printf(TEXT("\n- ... and %d more"), InReport.Num() - MaxReplyEntries);
    }
    return Lines;
  }
}

bool UIWTImageReader::PrepareSource(const FString &InFile, EScale InScale, FSource &OutSource,
                                    FString &OutError)
{
  OutSource = FSource();
  OutSource.SourceFile = FPaths::ConvertRelativePathToFull(InFile.TrimStartAndEnd().TrimQuotes());
  TArray<uint8> Bytes;
  FImage Image;
  if (!FFileHelper::LoadFileToArray(Bytes, *OutSource.SourceFile) ||
      !UIWTRunImages::LoadImageFile(OutSource.SourceFile, Image, OutError))
  {
    if (OutError.IsEmpty())
    {
      OutError = FString::Printf(TEXT("Could not read %s."), *OutSource.SourceFile);
    }
    return false;
  }
  OutSource.Crc = FString::Printf(TEXT("%08x"), FCrc::MemCrc32(Bytes.GetData(), Bytes.Num()));
  OutSource.ImageSize = FIntPoint(Image.SizeX, Image.SizeY);
  const FIntPoint Reference = UUIWTDesignSettings::Get()->ReferenceResolution;
  OutSource.Scale = InScale == EScale::Two  ? 2.0
                    : InScale == EScale::Fit && Reference.X > 0
                        ? double(Image.SizeX) / Reference.X
                        : 1.0;
  OutSource.DesignSize = FVector2D(Image.SizeX / OutSource.Scale, Image.SizeY / OutSource.Scale);
  OutSource.CacheDir = FPaths::ConvertRelativePathToFull(
      FPaths::ProjectSavedDir() / TEXT("UIWidgetTool/Image") /
      (SafeName(FPaths::GetBaseFilename(OutSource.SourceFile)) + TEXT("_") + OutSource.Crc));
  // A fresh start: an earlier run's crops and tree don't carry over.
  IFileManager &Files = IFileManager::Get();
  Files.DeleteDirectory(*OutSource.CacheDir, false, true);
  Files.MakeDirectory(*(OutSource.CacheDir / TEXT("images")), true);
  return UIWTRunImages::SavePng(Image, OutSource.CacheDir / TEXT("reference.png"), OutError);
}

bool UIWTImageReader::ToDocument(const FString &InJson, const FString &InCacheDir, double InScale,
                                 const FString &InSourceFile, const FString &InCrc,
                                 FIntPoint InReferenceSize, FDocument &OutDocument,
                                 TArray<FString> &OutErrors)
{
  OutErrors.Reset();
  TSharedPtr<FJsonObject> Parsed;
  if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(InJson), Parsed) ||
      !Parsed.IsValid())
  {
    OutErrors.Add(TEXT("the tree isn't a JSON object"));
    return false;
  }
  TSharedPtr<FJsonObject> Root = ObjectField(Parsed, TEXT("root"));
  if (!Root.IsValid())
  {
    Root = Parsed;
  }
  FLocalizer Localizer{InCacheDir, FMath::Max(InScale, 0.01), OutErrors};
  Localizer.Node(Root, 0.0, 0.0, true, TEXT("root"));
  if (!OutErrors.IsEmpty())
  {
    return false;
  }

  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
  Document->SetNumberField(TEXT("version"), 1);
  Document->SetStringField(TEXT("source"), TEXT("image"));
  TSharedRef<FJsonObject> SourceRef = MakeShared<FJsonObject>();
  SourceRef->SetStringField(TEXT("file"), InSourceFile);
  SourceRef->SetStringField(TEXT("crc"), InCrc);
  // The merge's same-source check compares it: every tree from this run is
  // the same source.
  SourceRef->SetStringField(TEXT("nodeId"), TEXT("img"));
  Document->SetObjectField(TEXT("sourceRef"), SourceRef);
  TSharedRef<FJsonObject> ReferenceSize = MakeShared<FJsonObject>();
  ReferenceSize->SetNumberField(TEXT("w"), InReferenceSize.X);
  ReferenceSize->SetNumberField(TEXT("h"), InReferenceSize.Y);
  Document->SetObjectField(TEXT("referenceSize"), ReferenceSize);
  Document->SetObjectField(TEXT("root"), Root);
  return ReadDocument(Document, OutDocument, OutErrors);
}

bool UIWTImageReader::CutNode(FUIWTActiveRun &InOutRun, const FString &InId, const FIntRect &InRect,
                              const FString &InMode, const FString &InShape, FString &OutPath,
                              FString &OutError)
{
  const FString Id = SafeName(InId);
  UIWTRunImages::ECropMode Mode;
  UIWTRunImages::ECropShape Shape;
  if (!UIWTRunImages::ParseCropMode(InMode.IsEmpty() ? TEXT("Copy") : InMode, Mode))
  {
    OutError = TEXT("Mode must be \"Copy\", \"KeyDark\" or \"Mask\".");
    return false;
  }
  if (!UIWTRunImages::ParseCropShape(InShape.IsEmpty() ? TEXT("Rect") : InShape, Shape))
  {
    OutError = TEXT("Shape must be \"Rect\" or \"Circle\".");
    return false;
  }
  FImage Reference;
  FImage Pixels;
  if (!UIWTRunImages::LoadImageFile(InOutRun.ReferenceImagePath, Reference, OutError) ||
      !UIWTRunImages::CropForTexture(Reference, InRect, Mode, Shape, 12, FIntPoint::ZeroValue,
                                     Pixels, OutError))
  {
    return false;
  }
  // Identical art is one file, so it becomes one texture.
  const FString Hash =
      FString::Printf(TEXT("%dx%d_"), Pixels.SizeX, Pixels.SizeY) +
      FMD5::HashBytes(Pixels.RawData.GetData(), Pixels.RawData.Num());
  if (const FString *Existing = InOutRun.ImageCrops.Find(Hash))
  {
    OutPath = *Existing;
    return true;
  }
  OutPath = FString::Printf(TEXT("images/%s.png"), *Id);
  // The id's earlier crop, if any, is replaced.
  for (auto It = InOutRun.ImageCrops.CreateIterator(); It; ++It)
  {
    if (It.Value() == OutPath)
    {
      It.RemoveCurrent();
    }
  }
  if (!UIWTRunImages::SavePng(Pixels, InOutRun.ImageCacheDir / OutPath, OutError))
  {
    return false;
  }
  InOutRun.ImageCrops.Add(Hash, OutPath);
  return true;
}

bool UIWTImageReader::WriteTree(FUIWTActiveRun &InOutRun, const FString &InJson,
                                FString &OutSummary, TArray<FString> &OutErrors)
{
  FDocument Doc;
  if (!ToDocument(InJson, InOutRun.ImageCacheDir, InOutRun.ImageScale, InOutRun.ImageSourceFile,
                  InOutRun.ImageCrc, UUIWTDesignSettings::Get()->ReferenceResolution, Doc,
                  OutErrors))
  {
    return false;
  }
  const FString DesignFile = InOutRun.ImageCacheDir / TEXT("design.json");
  if (!FFileHelper::SaveStringToFile(WriteDocumentString(Doc), *DesignFile,
                                     FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
  {
    OutErrors.Add(FString::Printf(TEXT("could not write %s"), *DesignFile));
    return false;
  }
  const FString Reference = InOutRun.ImageCacheDir / TEXT("reference.png");
  FString Error;

  // The first tree: a new blueprint for the entry.
  if (InOutRun.BlueprintPath.IsNull())
  {
    UIWTDesignImport::FRequest Request;
    Request.DesignFile = DesignFile;
    Request.ReferenceImage = Reference;
    Request.TargetFolder = InOutRun.ImageTargetFolder;
    Request.BlueprintName = InOutRun.ImageBlueprintName;
    Request.bSave = InOutRun.bImageSave;
    UIWTDesignImport::FResult Imported;
    if (!UIWTDesignImport::Import(Request, Imported, Error) || !Imported.Blueprint)
    {
      OutErrors.Add(TEXT("the import failed: ") + Error);
      return false;
    }
    UWidgetBlueprint *Blueprint = Imported.Blueprint;
    InOutRun.BlueprintPath = FSoftObjectPath(Blueprint);
    InOutRun.ContentFolder = FPackageName::GetLongPackagePath(Blueprint->GetOutermost()->GetName());
    InOutRun.WidgetsBefore = UIWTDesignRefine::SnapshotWidgets(Blueprint);
    UUIWidgetPreviewObjectManagerSettings *Settings = UUIWidgetPreviewObjectManagerSettings::Get();
    if (FWidgetPreviewObject *Entry = Settings->FindWidgetPreviewObject(InOutRun.EntryId))
    {
      Entry->WidgetClass = TSoftClassPtr<UUserWidget>(Blueprint->GeneratedClass);
      Entry->WidgetName = Blueprint->GetName();
      Entry->Note = FString::Printf(TEXT("Imported from the image %s."),
                                    *FPaths::GetCleanFilename(InOutRun.ImageSourceFile));
      Settings->SaveWidgetPreviewObjects();
    }
    if (FUIWTClaudeService *Service = FUIWTClaudeService::TryGet())
    {
      Service->OnEntriesChanged().Broadcast();
    }
    OutSummary = FString::Printf(
        TEXT("Imported %s: %d widgets. It's now the run's blueprint: GetContext returns it, and "
             "RenderWidgetBlueprint renders it. Later WriteDesignTree calls update it, keeping "
             "the widgets of nodes whose ids stay."),
        *InOutRun.BlueprintPath.ToString(), Imported.WidgetCount);
    for (const FString &ApplyError : Imported.ApplyErrors)
    {
      OutSummary += TEXT("\n- [applyError] ") + ApplyError;
    }
    if (!Imported.Report.IsEmpty())
    {
      OutSummary += FString::Printf(TEXT("\nReport (%d):"), Imported.Report.Num()) +
                    ReportLines(Imported.Report);
    }
    return true;
  }

  // Later trees: the re-import merge, after writing the renames made since
  // into the sidecar, so it recognizes the renamed widgets.
  UWidgetBlueprint *Blueprint = Cast<UWidgetBlueprint>(InOutRun.BlueprintPath.TryLoad());
  if (!Blueprint)
  {
    OutErrors.Add(TEXT("the run's blueprint can't be loaded"));
    return false;
  }
  int32 Renamed = 0;
  if (!UIWTDesignRefine::SyncRenames(Blueprint, InOutRun.WidgetsBefore, Renamed, Error))
  {
    OutErrors.Add(Error);
    return false;
  }
  UIWTDesignImport::FReimportRequest Request;
  Request.Blueprint = Blueprint;
  Request.DesignFile = DesignFile;
  Request.ReferenceImage = Reference;
  UIWTDesignImport::FReimportPlan Plan;
  UIWTDesignImport::FReimportResult Applied;
  if (!UIWTDesignImport::PlanReimport(Request, Plan, Error) ||
      !UIWTDesignImport::ApplyReimport(Plan, Applied, Error) ||
      !UIWTDesignImport::AcceptReimport(Blueprint, InOutRun.bImageSave, Error))
  {
    if (UIWTDesignImport::HasPendingReimport(Blueprint))
    {
      FString DiscardError;
      UIWTDesignImport::DiscardReimport(Blueprint, DiscardError);
    }
    OutErrors.Add(TEXT("the update failed: ") + Error);
    return false;
  }
  InOutRun.WidgetsBefore = UIWTDesignRefine::SnapshotWidgets(Blueprint);

  int32 Added = 0, Removed = 0, Changed = 0;
  for (const UIWTDesignMerge::FChange &Change : Plan.Changes)
  {
    if (Change.Change == UIWTDesignMerge::EChange::Added)
    {
      ++Added;
    }
    else if (Change.Change == UIWTDesignMerge::EChange::Removed)
    {
      ++Removed;
    }
    else
    {
      ++Changed;
    }
  }
  OutSummary = FString::Printf(
      TEXT("Updated %s: %d nodes added, %d removed, %d changed; %d conflicts, %d changes made in "
           "UE kept, %d widgets added in UE kept."),
      *InOutRun.BlueprintPath.ToString(), Added, Removed, Changed, Plan.Conflicts,
      Plan.KeptUEChanges, Plan.UEWidgets);
  for (const FString &ApplyError : Applied.ApplyErrors)
  {
    OutSummary += TEXT("\n- [applyError] ") + ApplyError;
  }
  if (!Plan.Report.IsEmpty())
  {
    OutSummary +=
        FString::Printf(TEXT("\nReport (%d):"), Plan.Report.Num()) + ReportLines(Plan.Report);
  }
  return true;
}

FString UIWTImageReader::BuildRequest(const FSource &InSource, int32 InRounds, bool bInAIPass)
{
  const FIntPoint Reference = UUIWTDesignSettings::Get()->ReferenceResolution;
  FString Request = FString::Printf(
      TEXT("Image import (import-image.md): read the attached image into a design tree. This "
           "request replaces steps 2 to 4 of the procedure above; steps 1, 5, 6 and 7 and the "
           "rules still apply. There is no blueprint yet: your first WriteDesignTree creates "
           "it, and GetContext returns it from then on.\n\n"
           "The image is %d x %d pixels, saved as %s. Work in its pixels. It is %g image "
           "pixels per design pixel, so the blueprint will be %g x %g design pixels (the "
           "project's screen is %d x %d); the tools do that conversion, never you.\n\n"),
      InSource.ImageSize.X, InSource.ImageSize.Y, *(InSource.CacheDir / TEXT("reference.png")),
      InSource.Scale, InSource.DesignSize.X, InSource.DesignSize.Y, Reference.X, Reference.Y);
  const int32 Rounds = FMath::Max(InRounds, 1);
  Request += FString::Printf(
      TEXT("Steps:\n"
           "1. List every region with its box (x, y, width, height in image pixels): "
           "backgrounds, panels, plates, bars, buttons, icons and pictures, texts, lines. Use "
           "ZoomImage (Source \"Reference\") and the Read tool to measure small or crowded "
           "regions and to read exact colours.\n"
           "2. Cut the art brushes can't draw (icons, portraits, item art, illustrations, "
           "textured backgrounds) with UIWTToolset.CutImageNode(NodeId, X, Y, Width, Height, "
           "Mode, Shape): Mode \"Copy\", or \"KeyDark\" / \"Mask\" for art on a dark "
           "background; Shape \"Rect\" or \"Circle\". It returns the path for the node's "
           "image. Never cut texts, flat panels or controls: those are nodes.\n"
           "3. Write the whole tree with UIWTToolset.WriteDesignTree(TreeJson) (format below). "
           "It rejects an invalid tree with every problem listed; fix them and write again. "
           "Its reply lists what the import reported (unmapped fonts, approximations).\n"
           "4. RenderWidgetBlueprint (0 x 0 renders at the reference size) and compare the "
           "side-by-side image; ZoomImage \"Both\" on regions that look off. Fix positions, "
           "sizes, colours and font sizes in the tree and write it again, keeping every "
           "node's id, so its widgets are kept. At most %d rounds; stop earlier when only small "
           "differences are left.\n"),
      Rounds);
  if (bInAIPass)
  {
    Request += TEXT(
        "5. After the last WriteDesignTree, refine the blueprint (don't write the tree after "
        "this: it would undo it):\n");
    Request += UIWTDesignRefine::PassTasks(TEXT("image"));
    Request += TEXT(
        "   Work with ExportWidgetSubtree / ApplyWidgetSubtree, keep every widget you keep with "
        "the same name and class, rename only with UIWTToolset.RenameWidgets, and don't change "
        "texts, colours or images. Render and check after each apply.\n"
        "6. SaveWidgetBlueprint after a clean compile. Reply with what you built, what you "
        "refined and what still differs from the image.\n\n");
  }
  else
  {
    Request += TEXT("5. Reply with what you built and what still differs from the image. "
                    "WriteDesignTree saves the blueprint; save again only after other edits.\n\n");
  }
  Request += TEXT(
      "Tree format ({\"root\": node}). Boxes and every length are in image pixels, absolute "
      "(from the image's top-left corner), including children's:\n"
      "node = {\"id\": unique short string, \"name\": a layer name (Btn_Buy, Title, Icon_Coin), "
      "\"kind\": \"frame\" | \"group\" | \"shape\" | \"text\" | \"image\", \"box\": {\"x\", "
      "\"y\", \"w\", \"h\"}, ...}\n"
      "- frame: a container with an optional look: \"fill\": {\"color\": \"#RRGGBB\" or "
      "\"#RRGGBBAA\"}, \"radii\": [tl, tr, br, bl], \"stroke\": {\"color\", \"weights\": "
      "{\"l\", \"t\", \"r\", \"b\"}, \"align\": \"inside\" | \"center\" | \"outside\"}, "
      "\"children\": [...]. The root is a frame covering the image, with the background "
      "fill.\n"
      "- group: a container with no look, \"children\": [...].\n"
      "- shape: a flat rectangle or rounded rectangle with no children: fill, radii, stroke.\n"
      "- text: \"text\": {\"content\": the exact text, \"runs\": [{\"font\": {\"family\": your "
      "best guess (Inter, Roboto, Segoe UI...), \"style\": \"Regular\" | \"Bold\" | "
      "\"SemiBold\" | \"Italic\" | \"Light\"}, \"size\": the font size in pixels (about the "
      "capital-letter height / 0.7), \"color\": \"#RRGGBB\"}], \"align\": \"left\" | "
      "\"center\" | \"right\", \"valign\": \"top\" | \"center\" | \"bottom\", \"sizing\": "
      "\"fixed\" (single lines, box = the line's box) | \"fixedWidth\" (wrapping "
      "paragraphs)}.\n"
      "- image: \"image\": {\"path\": what CutImageNode returned}.\n"
      "- any node: \"opacity\": 0-1, \"visible\": false, \"hints\": [...].\n"
      "Colours are the image's sRGB values; the import converts them. Children draw in "
      "order, later on top.\n\n"
      "Hints give the blueprint its structure; use them wherever they're clearly true:\n"
      "- \"layout:row\" / \"layout:column\" on a frame or group whose children sit in one row "
      "or column (a toolbar, a list, a button row, a form): it becomes a HorizontalBox or "
      "VerticalBox, with the gaps and margins measured from your boxes. Children must not "
      "overlap along the row or column.\n"
      "- \"role:button\" on a frame that is clickable (a plate with a label): it becomes a "
      "Button with the frame's look.\n"
      "- \"role:list\" on a layout:row / layout:column frame whose items scroll: a "
      "ScrollBox.\n");
  return Request;
}
