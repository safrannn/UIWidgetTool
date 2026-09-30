#include "UIWTDesignImport.h"

#include "Animation/WidgetAnimation.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Core/UIWTDesignSettings.h"
#include "Core/UIWTGeneratedBlueprints.h"
#include "Core/UIWTNotify.h"
#include "Core/UIWTRunImages.h"
#include "Core/UIWTWidgetSpec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Fonts/CompositeFont.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Rendering/SlateRenderer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditorUtils.h"
#include "WidgetBlueprintOperationUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogUIWTDesignImport, Log, All);

namespace
{
  using namespace UIWTDesignTree;

  // ---------------------------------------------------------------------
  // Fonts

  // "Semi Bold", "SemiBold" and "semi-bold" match.
  FString StyleKey(const FString &InStyle)
  {
    FString Key = InStyle.Replace(TEXT(" "), TEXT("")).Replace(TEXT("-"), TEXT(""));
    Key.ToLowerInline();
    return Key;
  }

  // Font lookups for one set of converter options. The converter already
  // calls ResolveFont once per distinct font and NaturalLineHeight once per
  // font and size; this keeps each font asset loaded once and alive until
  // the options go away.
  class FFontResolver
  {
  public:
    TOptional<FResolvedFont> Resolve(const FFontRef &InFont)
    {
      const FString Style = StyleKey(InFont.Style);
      const FUIWTFontMapping *Exact = nullptr;
      const FUIWTFontMapping *AnyStyle = nullptr;
      for (const FUIWTFontMapping &Mapping : UUIWTDesignSettings::Get()->Fonts)
      {
        if (!Mapping.Family.TrimStartAndEnd().Equals(InFont.Family.TrimStartAndEnd(),
                                                     ESearchCase::IgnoreCase))
        {
          continue;
        }
        const FString MappingStyle = StyleKey(Mapping.Style);
        if (MappingStyle.IsEmpty())
        {
          AnyStyle = AnyStyle ? AnyStyle : &Mapping;
        }
        else if (MappingStyle == Style)
        {
          Exact = &Mapping;
          break;
        }
      }
      const FUIWTFontMapping *Mapping = Exact ? Exact : AnyStyle;
      if (!Mapping || Mapping->Font.IsNull())
      {
        return {};
      }
      UFont *Font = Mapping->Font.LoadSynchronous();
      if (!Font)
      {
        UE_LOG(LogUIWTDesignImport, Warning, TEXT("Font %s (mapped for '%s %s') can't be loaded."),
               *Mapping->Font.ToString(), *InFont.Family, *InFont.Style);
        return {};
      }
      Loaded.Add(Font->GetPathName(), TStrongObjectPtr<UObject>(Font));

      FResolvedFont Resolved;
      Resolved.FontObject = Font->GetPathName();
      Resolved.Typeface = Mapping->Typeface;
      const FCompositeFont *Composite = Font->GetCompositeFont();
      if (Resolved.Typeface.IsNone() && Composite)
      {
        const TArray<FTypefaceEntry> &Faces = Composite->DefaultTypeface.Fonts;
        for (const FString &Wanted : {Style, FString(TEXT("regular"))})
        {
          for (const FTypefaceEntry &Face : Faces)
          {
            if (Resolved.Typeface.IsNone() && StyleKey(Face.Name.ToString()) == Wanted)
            {
              Resolved.Typeface = Face.Name;
            }
          }
        }
        if (Resolved.Typeface.IsNone() && !Faces.IsEmpty())
        {
          Resolved.Typeface = Faces[0].Name;
        }
      }
      return Resolved;
    }

    // Slate's line height for the font at a design pixel size: the height a
    // TextBlock gives one line at LineHeightPercentage 1.
    double NaturalLineHeight(const TOptional<FResolvedFont> &InFont, double InSizePx)
    {
      if (!FSlateApplication::IsInitialized() || !FSlateApplication::Get().GetRenderer())
      {
        return 0.0;
      }
      FSlateFontInfo Info;
      if (InFont.IsSet())
      {
        const TStrongObjectPtr<UObject> *Kept = Loaded.Find(InFont->FontObject);
        const UObject *Font = Kept ? Kept->Get()
                                   : LoadObject<UObject>(nullptr, *InFont->FontObject);
        if (!Font)
        {
          return 0.0;
        }
        Info = FSlateFontInfo(Font, 1.0f, InFont->Typeface);
      }
      else
      {
        // What text with an unmapped font keeps: UTextBlock's default font.
        Info = GetDefault<UTextBlock>()->GetFont();
      }
      // UMG font sizes are points at 96 DPI; design pixels are at 72.
      Info.Size = static_cast<float>(InSizePx * 0.75);
      const TSharedRef<FSlateFontMeasure> Measure =
          FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
      return Measure->GetMaxCharacterHeight(Info, 1.0f);
    }

  private:
    TMap<FString, TStrongObjectPtr<UObject>> Loaded;
  };

  // ---------------------------------------------------------------------
  // Helpers

  bool IsInsideDirectory(const FString &InPath, const FString &InDirectory)
  {
    FString Path = FPaths::ConvertRelativePathToFull(InPath);
    FString Directory = FPaths::ConvertRelativePathToFull(InDirectory);
    FPaths::NormalizeFilename(Path);
    FPaths::NormalizeDirectoryName(Directory);
    FPaths::CollapseRelativeDirectories(Path);
    return Path.StartsWith(Directory + TEXT("/"), ESearchCase::IgnoreCase);
  }

  FString NormalizeFolder(const FString &InFolder)
  {
    FString Folder = InFolder.TrimStartAndEnd();
    Folder.ReplaceInline(TEXT("\\"), TEXT("/"));
    while (Folder.EndsWith(TEXT("/")))
    {
      Folder.LeftChopInline(1);
    }
    return Folder;
  }

  bool CheckTarget(const FString &InFolder, const FString &InName, FString &OutError)
  {
    FText Reason;
    if (!FName::IsValidXName(InName, INVALID_OBJECTNAME_CHARACTERS INVALID_LONGPACKAGE_CHARACTERS,
                             &Reason))
    {
      OutError = FString::Printf(TEXT("\"%s\" is not a valid asset name. %s"), *InName,
                                 *Reason.ToString());
      return false;
    }
    const FString PackageName = InFolder / InName / InName;
    // /Temp is a read-only root, allowed so tests can import without saving.
    const bool bTemp = PackageName.StartsWith(TEXT("/Temp/"));
    if (!FPackageName::IsValidLongPackageName(PackageName, bTemp, &Reason))
    {
      OutError = FString::Printf(TEXT("%s is not a content path an import can use. %s"),
                                 *InFolder, *Reason.ToString());
      return false;
    }
    if (UIWTGenerated::IsGeneratedPath(PackageName))
    {
      OutError = TEXT("Imports can't go into the generated-blueprint folder: it is "
                      "per-developer scratch that nothing under Content may reference.");
      return false;
    }
    const UPackage *InMemory = FindPackage(nullptr, *PackageName);
    if (FPackageName::DoesPackageExist(PackageName) ||
        (InMemory && InMemory->FindAssetInPackage()))
    {
      OutError = FString::Printf(
          TEXT("%s already exists. Re-importing into an existing blueprint needs the merge, "
               "which isn't built yet: import under another name, or delete it first."),
          *PackageName);
      return false;
    }
    return true;
  }

  void DeleteCreated(const TArray<UObject *> &InObjects)
  {
    TArray<UObject *> Objects;
    for (UObject *Object : InObjects)
    {
      if (IsValid(Object))
      {
        Objects.Add(Object);
      }
    }
    if (!Objects.IsEmpty())
    {
      ObjectTools::ForceDeleteObjects(Objects, false);
    }
  }

  const TCHAR *RootModeName(ERootMode InMode)
  {
    return InMode == ERootMode::Widget ? TEXT("widget") : TEXT("screen");
  }

  // Sorted, so the sidecar diffs cleanly between imports.
  TSharedRef<FJsonObject> OwnedJson(const FOwnedMap &InOwned)
  {
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    TArray<FString> Ids;
    InOwned.GetKeys(Ids);
    Ids.Sort();
    for (const FString &Id : Ids)
    {
      TSharedRef<FJsonObject> Roles = MakeShared<FJsonObject>();
      TArray<FString> RoleKeys;
      InOwned[Id].GetKeys(RoleKeys);
      RoleKeys.Sort();
      for (const FString &Role : RoleKeys)
      {
        Roles->SetStringField(Role, InOwned[Id][Role]);
      }
      Object->SetObjectField(Id, Roles);
    }
    return Object;
  }

  // Every widget of the spec with its class, parent and normalized values,
  // and what Export gave for it: the base the re-import merge compares
  // against (import-tree.md → Owned widgets).
  void AddWidgets(const TSharedPtr<FJsonObject> &InNode, const FString &InParent,
                  const TMap<FString, UIWTDesignMerge::FExported> &InExported,
                  TMap<FString, TSharedPtr<FJsonObject>> &OutWidgets)
  {
    if (!InNode.IsValid())
    {
      return;
    }
    const FString Name = InNode->GetStringField(TEXT("name"));
    TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
    Entry->SetStringField(TEXT("class"), InNode->GetStringField(TEXT("class")));
    Entry->SetStringField(TEXT("parent"), InParent);
    for (const TCHAR *Field : {TEXT("props"), TEXT("slot")})
    {
      const TSharedPtr<FJsonObject> *Values = nullptr;
      if (InNode->TryGetObjectField(Field, Values) && !(*Values)->Values.IsEmpty())
      {
        Entry->SetField(Field, UIWTWidgetSpec::NormalizeValue(
                                   MakeShared<FJsonValueObject>(*Values)));
      }
    }
    if (const UIWTDesignMerge::FExported *Exported = InExported.Find(Name))
    {
      TSharedRef<FJsonObject> ExportedObject = MakeShared<FJsonObject>();
      if (Exported->Props.IsValid() && !Exported->Props->Values.IsEmpty())
      {
        ExportedObject->SetObjectField(TEXT("props"), Exported->Props);
      }
      if (Exported->Slot.IsValid() && !Exported->Slot->Values.IsEmpty())
      {
        ExportedObject->SetObjectField(TEXT("slot"), Exported->Slot);
      }
      Entry->SetObjectField(TEXT("exported"), ExportedObject);
    }
    OutWidgets.Add(Name, Entry);
    const TArray<TSharedPtr<FJsonValue>> *Children = nullptr;
    if (InNode->TryGetArrayField(TEXT("children"), Children))
    {
      for (const TSharedPtr<FJsonValue> &Child : *Children)
      {
        AddWidgets(Child->AsObject(), Name, InExported, OutWidgets);
      }
    }
  }

  // The value with every string naming one of InPaths replaced by "None":
  // a brush whose texture couldn't be made draws without one instead of
  // Apply failing on the missing object. Objects change in place.
  TSharedPtr<FJsonValue> WithoutReferences(const TSharedPtr<FJsonValue> &InValue,
                                           const TSet<FString> &InPaths)
  {
    if (!InValue.IsValid())
    {
      return InValue;
    }
    switch (InValue->Type)
    {
    case EJson::String:
      return InPaths.Contains(InValue->AsString()) ? MakeShared<FJsonValueString>(TEXT("None"))
                                                   : InValue;
    case EJson::Object:
      for (auto &Pair : InValue->AsObject()->Values)
      {
        Pair.Value = WithoutReferences(Pair.Value, InPaths);
      }
      return InValue;
    case EJson::Array:
    {
      TArray<TSharedPtr<FJsonValue>> Items = InValue->AsArray();
      for (TSharedPtr<FJsonValue> &Item : Items)
      {
        Item = WithoutReferences(Item, InPaths);
      }
      return MakeShared<FJsonValueArray>(Items);
    }
    default:
      return InValue;
    }
  }

  void ClearReferences(const TSharedPtr<FJsonObject> &InSpec, const TSet<FString> &InPaths)
  {
    if (InSpec.IsValid() && !InPaths.IsEmpty())
    {
      WithoutReferences(MakeShared<FJsonValueObject>(InSpec), InPaths);
    }
  }

  // Export's view of the blueprint as it is now, by widget.
  TMap<FString, UIWTDesignMerge::FExported> ExportNow(UWidgetBlueprint *InBlueprint)
  {
    FString Json;
    FString Error;
    TSharedPtr<FJsonObject> Spec;
    if (!UIWTWidgetSpec::Export(InBlueprint, Json, Error) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Spec))
    {
      return {};
    }
    return UIWTDesignMerge::ExportedWidgets(Spec);
  }

  FString ProjectRelative(const FString &InPath)
  {
    const FString Full = FPaths::ConvertRelativePathToFull(InPath);
    FString Path = Full;
    const FString Project = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    return FPaths::MakePathRelativeTo(Path, *Project) && !Path.StartsWith(TEXT(".."))
               ? Path
               : Full;
  }

  FString PrettyJson(const TSharedRef<FJsonObject> &InObject)
  {
    FString Json;
    TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);
    FJsonSerializer::Serialize(InObject, Writer);
    return Json;
  }

  bool WriteSidecar(const FString &InPath, const FDocument &InDoc, const FString &InDesignFile,
                    const UClass *InParentClass, const FConvertResult &InConverted,
                    const TMap<FString, UIWTDesignMerge::FExported> &InExported,
                    const FString &InComponentsFolder, const FString &InComponentKey,
                    const TArray<FString> &InChangedNodes, FString &OutError)
  {
    TSharedRef<FJsonObject> Sidecar = MakeShared<FJsonObject>();
    Sidecar->SetNumberField(TEXT("version"), 1);
    Sidecar->SetStringField(TEXT("source"), InDoc.Source);
    if (InDoc.SourceRef.IsValid())
    {
      Sidecar->SetObjectField(TEXT("sourceRef"), InDoc.SourceRef);
    }
    Sidecar->SetStringField(TEXT("designFile"), ProjectRelative(InDesignFile));
    TSharedRef<FJsonObject> Reference = MakeShared<FJsonObject>();
    Reference->SetNumberField(TEXT("w"), InDoc.ReferenceSize.X);
    Reference->SetNumberField(TEXT("h"), InDoc.ReferenceSize.Y);
    Sidecar->SetObjectField(TEXT("referenceSize"), Reference);
    Sidecar->SetStringField(TEXT("rootMode"), RootModeName(InConverted.RootMode));
    Sidecar->SetStringField(TEXT("parentClass"), InParentClass->GetPathName());
    Sidecar->SetStringField(TEXT("componentsFolder"), InComponentsFolder);
    if (!InComponentKey.IsEmpty())
    {
      Sidecar->SetStringField(TEXT("component"), InComponentKey);
    }
    Sidecar->SetStringField(TEXT("importedAt"), FDateTime::UtcNow().ToIso8601());
    Sidecar->SetObjectField(TEXT("owned"), OwnedJson(InConverted.Owned));

    TMap<FString, TSharedPtr<FJsonObject>> Widgets;
    AddWidgets(InConverted.Spec->GetObjectField(TEXT("root")), FString(), InExported, Widgets);
    Widgets.KeySort(TLess<FString>());
    TSharedRef<FJsonObject> WidgetsObject = MakeShared<FJsonObject>();
    for (const TPair<FString, TSharedPtr<FJsonObject>> &Pair : Widgets)
    {
      WidgetsObject->SetObjectField(Pair.Key, Pair.Value);
    }
    Sidecar->SetObjectField(TEXT("widgets"), WidgetsObject);

    TArray<FString> Paths;
    InConverted.Textures.GetKeys(Paths);
    Paths.Sort();
    TSharedRef<FJsonObject> Textures = MakeShared<FJsonObject>();
    for (const FString &Path : Paths)
    {
      Textures->SetStringField(Path, InConverted.Textures[Path]);
    }
    Sidecar->SetObjectField(TEXT("textures"), Textures);

    // For AI passes run later (import-tree.md → Claude pass → What Claude
    // gets): the report, and a re-import's changed nodes.
    TArray<TSharedPtr<FJsonValue>> Report;
    for (const FReportEntry &Entry : InConverted.Report)
    {
      TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
      Object->SetStringField(TEXT("node"), Entry.Node);
      Object->SetStringField(TEXT("category"), Entry.Category);
      Object->SetStringField(TEXT("detail"), Entry.Detail);
      Report.Add(MakeShared<FJsonValueObject>(Object));
    }
    Sidecar->SetArrayField(TEXT("report"), Report);
    if (!InChangedNodes.IsEmpty())
    {
      TArray<TSharedPtr<FJsonValue>> Changed;
      for (const FString &Node : InChangedNodes)
      {
        Changed.Add(MakeShared<FJsonValueString>(Node));
      }
      Sidecar->SetArrayField(TEXT("changedNodes"), Changed);
    }

    if (!FFileHelper::SaveStringToFile(PrettyJson(Sidecar), *InPath,
                                       FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
      OutError = FString::Printf(TEXT("Could not write %s."), *InPath);
      return false;
    }
    return true;
  }

  // UIWT.ImportDesign <design.json> [/Game/Folder] [BlueprintName]
  FAutoConsoleCommand ImportDesignCommand(
      TEXT("UIWT.ImportDesign"),
      TEXT("Imports a design tree (design.json) as a new Widget Blueprint. "
           "UIWT.ImportDesign <design.json> [/Game/Folder] [BlueprintName]"),
      FConsoleCommandWithArgsDelegate::CreateLambda(
          [](const TArray<FString> &InArgs)
          {
            if (InArgs.IsEmpty())
            {
              UE_LOG(LogUIWTDesignImport, Display,
                     TEXT("UIWT.ImportDesign <design.json> [/Game/Folder] [BlueprintName]"));
              return;
            }
            UIWTDesignImport::FRequest Request;
            Request.DesignFile = InArgs[0].TrimQuotes();
            Request.TargetFolder = InArgs.IsValidIndex(1) ? InArgs[1].TrimQuotes() : FString();
            Request.BlueprintName = InArgs.IsValidIndex(2) ? InArgs[2].TrimQuotes() : FString();
            UIWTDesignImport::FResult Result;
            FString Error;
            if (!UIWTDesignImport::Import(Request, Result, Error))
            {
              UE_LOG(LogUIWTDesignImport, Error, TEXT("Import failed: %s"), *Error);
              UIWTNotify::Show(FText::FromString(TEXT("Design import failed: ") + Error), false);
              return;
            }
            UE_LOG(LogUIWTDesignImport, Display, TEXT("%s"),
                   *UIWTDesignImport::ResultToJson(Result));
            UIWTNotify::Show(FText::FromString(FString::Printf(
                                 TEXT("Imported %s (%d widgets, %d report entries)."),
                                 *Result.BlueprintPath, Result.WidgetCount,
                                 Result.Report.Num())),
                             true);
          }));
}

// ---------------------------------------------------------------------------

TSet<FString> UIWTDesignImport::ReservedNamesForClass(const UClass *InParentClass)
{
  const UClass *Class = InParentClass ? InParentClass : UUserWidget::StaticClass();
  TSet<FString> Names;
  for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
  {
    // A layer named like a BindWidget property is meant to fill it.
    if (!FWidgetBlueprintEditorUtils::IsBindWidgetProperty(*It))
    {
      Names.Add(It->GetName());
    }
  }
  for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
  {
    Names.Add(It->GetName());
  }
  // The one graph a new Widget Blueprint starts with.
  Names.Add(TEXT("EventGraph"));
  return Names;
}

FConvertOptions UIWTDesignImport::FirstImportOptions(const FString &InFolder, const FString &InName,
                                                     const UClass *InParentClass)
{
  // No sidecar names or textures yet.
  FConvertOptions Options;
  Options.BlueprintName = InName;
  Options.TargetFolder = InFolder;
  Options.ComponentsFolder = UUIWTDesignSettings::Get()->GetComponentsFolder();
  Options.ComponentIndex = ReadComponentIndex(Options.ComponentsFolder);
  Options.ReservedNames = ReservedNamesForClass(InParentClass);
  SetFontResolver(Options);
  IAssetRegistry &Registry = IAssetRegistry::GetChecked();
  TArray<FAssetData> Assets;
  Registry.GetAssetsByPath(FName(*(InFolder / InName)), Assets, false);
  for (const FAssetData &Asset : Assets)
  {
    Options.TakenAssetNames.Add(Asset.AssetName.ToString());
  }
  Assets.Reset();
  Registry.GetAssetsByPath(FName(*Options.ComponentsFolder), Assets, true);
  for (const FAssetData &Asset : Assets)
  {
    Options.TakenComponentNames.Add(Asset.AssetName.ToString());
  }
  return Options;
}

TSet<FString> UIWTDesignImport::ReservedNamesForBlueprint(const UWidgetBlueprint *InBlueprint,
                                                          const FOwnedMap &InOwned,
                                                          const TSet<FString> &InProtected)
{
  TSet<FString> Names = ReservedNamesForClass(InBlueprint->ParentClass);
  for (const FBPVariableDescription &Variable : InBlueprint->NewVariables)
  {
    Names.Add(Variable.VarName.ToString());
  }
  TSet<FName> More;
  FBlueprintEditorUtils::GetFunctionNameList(InBlueprint, More);
  FBlueprintEditorUtils::GetAllGraphNames(InBlueprint, More);
  FBlueprintEditorUtils::GetSCSVariableNameList(InBlueprint, More);
  for (const UWidgetAnimation *Animation : InBlueprint->Animations)
  {
    if (Animation)
    {
      More.Add(Animation->GetFName());
    }
  }
  for (const FName Name : More)
  {
    Names.Add(Name.ToString());
  }
  // Widgets added in UE stay, so their names are taken.
  TSet<FString> OwnedNames;
  for (const TPair<FString, FRoleNames> &Node : InOwned)
  {
    for (const TPair<FString, FString> &Role : Node.Value)
    {
      OwnedNames.Add(Role.Value);
    }
  }
  if (InBlueprint->WidgetTree)
  {
    InBlueprint->WidgetTree->ForEachWidget(
        [&](const UWidget *InWidget)
        {
          if (!OwnedNames.Contains(InWidget->GetName()))
          {
            Names.Add(InWidget->GetName());
          }
        });
  }
  for (const FString &Name : InProtected)
  {
    Names.Remove(Name);
  }
  return Names;
}

void UIWTDesignImport::SetFontResolver(FConvertOptions &InOutOptions)
{
  const TSharedRef<FFontResolver> Resolver = MakeShared<FFontResolver>();
  InOutOptions.ResolveFont = [Resolver](const FFontRef &InFont)
  { return Resolver->Resolve(InFont); };
  InOutOptions.NaturalLineHeight = [Resolver](const TOptional<FResolvedFont> &InFont,
                                              double InSizePx)
  { return Resolver->NaturalLineHeight(InFont, InSizePx); };
}

FString UIWTDesignImport::DefaultBlueprintName(const FDocument &InDocument)
{
  const FString Base = SanitizeName(InDocument.Root.Name, InDocument.Root.Kind);
  return Base.StartsWith(TEXT("WBP_"), ESearchCase::IgnoreCase) ? Base : TEXT("WBP_") + Base;
}

FString UIWTDesignImport::GetSidecarPath(const FString &InBlueprintPackage)
{
  const FString Directory =
      UIWTGenerated::GetFolderOnDisk(FPackageName::GetLongPackagePath(InBlueprintPackage));
  return Directory.IsEmpty()
             ? FString()
             : Directory / FPackageName::GetShortName(InBlueprintPackage) +
                   TEXT(".design.json");
}

// ---------------------------------------------------------------------------
// Child WBPs (import-tree.md → Assets → Child WBPs)

namespace UIWTDesignImport
{
  // One import's child WBPs, shared by the screen and every child converted
  // under it, so each component.key is handled once however deep it is.
  struct FComponentRun
  {
    FString Folder;
    TMap<FString, FComponentIndexEntry> Index;
    TSet<FString> Handled;
    bool bSave = true;
    // A screen re-import: children stay unsaved, and what was made is kept
    // so Accept can save it and Discard undo it.
    bool bPending = false;
    TArray<TWeakObjectPtr<UObject>> CreatedAssets;
    TArray<FString> CreatedSidecars;
    // Children with a pending re-import, nested ones first.
    TArray<FString> UpdatedPackages;
    TArray<FString> Lines;
    TArray<FReportEntry> Report;
  };

  bool ImportInternal(const FRequest &InRequest, FComponentRun &InOutRun, FResult &OutResult,
                      FString &OutError);
  bool PlanReimportInternal(const FReimportRequest &InRequest, FReimportPlan &OutPlan,
                            FComponentRun *InRun, FString &OutError);
  bool ApplyReimportInternal(const FReimportPlan &InPlan, FReimportResult &OutResult,
                             FComponentRun *InRun, FString &OutError);
  bool AcceptSelf(const FString &InPackage, bool bInSave, FString &OutError);
  bool DiscardSelf(const FString &InPackage, FString &OutError);
  // Drops a package's pending re-import without touching the blueprint.
  void ForgetReimport(const FString &InPackage);
}

namespace
{
  bool WriteComponentIndex(const FString &InFolder,
                           const TMap<FString, FComponentIndexEntry> &InIndex)
  {
    const FString Path = UIWTDesignImport::GetComponentIndexPath(InFolder);
    if (Path.IsEmpty())
    {
      return false;
    }
    TArray<FString> Keys;
    InIndex.GetKeys(Keys);
    Keys.Sort();
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    for (const FString &Key : Keys)
    {
      const FComponentIndexEntry &Entry = InIndex[Key];
      TSharedRef<FJsonObject> EntryObject = MakeShared<FJsonObject>();
      EntryObject->SetStringField(TEXT("assetPath"), Entry.AssetPath);
      EntryObject->SetStringField(TEXT("hash"), Entry.Hash);
      EntryObject->SetStringField(TEXT("fileKey"), Entry.FileKey);
      EntryObject->SetStringField(TEXT("nodeId"), Entry.NodeId);
      EntryObject->SetStringField(TEXT("version"), Entry.Version);
      Object->SetObjectField(Key, EntryObject);
    }
    return FFileHelper::SaveStringToFile(PrettyJson(Object), *Path,
                                         FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
  }

  // Every asset name under the components folder, so a new child never
  // lands on an asset the index doesn't know.
  TSet<FString> ComponentFolderNames(const FString &InFolder)
  {
    TSet<FString> Names;
    TArray<FAssetData> Assets;
    IAssetRegistry::GetChecked().GetAssetsByPath(FName(*InFolder), Assets, true);
    for (const FAssetData &Asset : Assets)
    {
      Names.Add(Asset.AssetName.ToString());
    }
    return Names;
  }

  // Undoes a pending run's children: re-imports discarded, new ones deleted.
  void DiscardRun(UIWTDesignImport::FComponentRun &InRun)
  {
    for (int32 Index = InRun.UpdatedPackages.Num() - 1; Index >= 0; --Index)
    {
      FString Error;
      if (!UIWTDesignImport::DiscardSelf(InRun.UpdatedPackages[Index], Error))
      {
        // Never saved (nothing on disk to reload): at least don't leave it
        // waiting for an Accept that won't come.
        UIWTDesignImport::ForgetReimport(InRun.UpdatedPackages[Index]);
      }
    }
    TArray<UObject *> Created;
    for (const TWeakObjectPtr<UObject> &Asset : InRun.CreatedAssets)
    {
      if (Asset.IsValid())
      {
        Created.Add(Asset.Get());
      }
    }
    if (!Created.IsEmpty())
    {
      ObjectTools::ForceDeleteObjects(Created, false);
    }
    for (const FString &Sidecar : InRun.CreatedSidecars)
    {
      IFileManager::Get().Delete(*Sidecar, false, true, true);
    }
    InRun.UpdatedPackages.Reset();
    InRun.CreatedAssets.Reset();
    InRun.CreatedSidecars.Reset();
  }

  // The children a conversion asked for, deepest first: each child's own
  // components are handled inside its import, before its Apply, so no spec
  // ever names a child WBP class that doesn't exist yet.
  bool ProcessComponents(const TArray<FComponentJob> &InJobs, const FString &InDesignDir,
                         UIWTDesignImport::FComponentRun &InOutRun, FString &OutError)
  {
    // New children's paths are taken before any is built, so a component
    // converted under another can't be given the same name.
    for (const FComponentJob &Job : InJobs)
    {
      if (Job.Action == EComponentAction::Create && !InOutRun.Index.Contains(Job.Key))
      {
        FComponentIndexEntry Placeholder;
        Placeholder.AssetPath = Job.AssetPath;
        Placeholder.Hash = Job.Hash;
        InOutRun.Index.Add(Job.Key, Placeholder);
      }
    }
    for (const FComponentJob &Job : InJobs)
    {
      if (InOutRun.Handled.Contains(Job.Key))
      {
        continue;
      }
      InOutRun.Handled.Add(Job.Key);
      const FString ChildName = FPackageName::GetShortName(Job.AssetPath);
      if (Job.Action == EComponentAction::Reuse || !Job.Tree.IsValid())
      {
        InOutRun.Lines.Add(FString::Printf(TEXT("%s: reused %s"), *Job.Key, *Job.AssetPath));
        continue;
      }
      // The child's tree next to the screen's, so its image paths resolve.
      const FString ChildFile =
          InDesignDir / FString::Printf(TEXT("component_%08x.design.json"), FCrc::StrCrc32(*Job.Key));
      if (!FFileHelper::SaveStringToFile(WriteDocumentString(*Job.Tree), *ChildFile,
                                         FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
      {
        OutError = FString::Printf(TEXT("Could not write %s."), *ChildFile);
        return false;
      }
      FComponentIndexEntry Entry;
      Entry.AssetPath = Job.AssetPath;
      Entry.Hash = Job.Hash;
      if (Job.Tree->SourceRef.IsValid())
      {
        Job.Tree->SourceRef->TryGetStringField(TEXT("fileKey"), Entry.FileKey);
        Job.Tree->SourceRef->TryGetStringField(TEXT("nodeId"), Entry.NodeId);
        Job.Tree->SourceRef->TryGetStringField(TEXT("version"), Entry.Version);
      }

      if (Job.Action == EComponentAction::Create)
      {
        UIWTDesignImport::FRequest Child;
        Child.DesignFile = ChildFile;
        Child.TargetFolder = FPackageName::GetLongPackagePath(FPackageName::GetLongPackagePath(Job.AssetPath));
        Child.BlueprintName = ChildName;
        Child.ComponentsFolder = InOutRun.Folder;
        Child.ComponentKey = Job.Key;
        Child.bCopyReference = false;
        Child.bSave = InOutRun.bSave && !InOutRun.bPending;
        UIWTDesignImport::FResult ChildResult;
        if (!UIWTDesignImport::ImportInternal(Child, InOutRun, ChildResult, OutError))
        {
          OutError = FString::Printf(TEXT("Building the child WBP %s failed: %s"), *ChildName,
                                     *OutError);
          return false;
        }
        for (const FReportEntry &Report : ChildResult.Report)
        {
          InOutRun.Report.Add({Report.Node, Report.Category, ChildName + TEXT(": ") + Report.Detail});
        }
        if (InOutRun.bPending)
        {
          InOutRun.CreatedAssets.Append(ChildResult.CreatedAssets);
          InOutRun.CreatedSidecars.Add(ChildResult.SidecarFile);
        }
        InOutRun.Lines.Add(FString::Printf(TEXT("%s: created %s"), *Job.Key, *Job.AssetPath));
      }
      else
      {
        // Updating a child is a re-import of it, with its own sidecar.
        UWidgetBlueprint *ChildBlueprint =
            LoadObject<UWidgetBlueprint>(nullptr, *(Job.AssetPath + TEXT(".") + ChildName));
        if (!ChildBlueprint)
        {
          OutError = FString::Printf(TEXT("The child WBP %s can't be loaded."), *Job.AssetPath);
          return false;
        }
        UIWTDesignImport::FReimportRequest Request;
        Request.Blueprint = ChildBlueprint;
        Request.DesignFile = ChildFile;
        Request.bUseReference = false;
        UIWTDesignImport::FReimportPlan Plan;
        UIWTDesignImport::FReimportResult Applied;
        if (!UIWTDesignImport::PlanReimportInternal(Request, Plan, &InOutRun, OutError) ||
            !UIWTDesignImport::ApplyReimportInternal(Plan, Applied, &InOutRun, OutError))
        {
          OutError = FString::Printf(TEXT("Updating the child WBP %s failed: %s"), *ChildName,
                                     *OutError);
          return false;
        }
        for (const FReportEntry &Report : Plan.Report)
        {
          InOutRun.Report.Add({Report.Node, Report.Category, ChildName + TEXT(": ") + Report.Detail});
        }
        for (const FString &Error : Applied.ApplyErrors)
        {
          InOutRun.Report.Add({FString(), TEXT("applyError"), ChildName + TEXT(": ") + Error});
        }
        if (InOutRun.bPending)
        {
          InOutRun.UpdatedPackages.Add(ChildBlueprint->GetOutermost()->GetName());
        }
        else if (!UIWTDesignImport::AcceptSelf(ChildBlueprint->GetOutermost()->GetName(),
                                               InOutRun.bSave, OutError))
        {
          OutError = FString::Printf(TEXT("Saving the child WBP %s failed: %s"), *ChildName,
                                     *OutError);
          return false;
        }
        InOutRun.Lines.Add(FString::Printf(TEXT("%s: updated %s (%d conflicts, %d kept UE changes)"),
                                           *Job.Key, *Job.AssetPath, Plan.Conflicts,
                                           Plan.KeptUEChanges));
      }
      InOutRun.Index.Add(Job.Key, Entry);
      if (!InOutRun.bPending)
      {
        WriteComponentIndex(InOutRun.Folder, InOutRun.Index);
      }
    }
    return true;
  }
}

FString UIWTDesignImport::GetComponentIndexPath(const FString &InComponentsFolder)
{
  const FString Directory = UIWTGenerated::GetFolderOnDisk(NormalizeFolder(InComponentsFolder));
  return Directory.IsEmpty() ? FString() : Directory / TEXT("components.index.json");
}

TMap<FString, FComponentIndexEntry>
UIWTDesignImport::ReadComponentIndex(const FString &InComponentsFolder)
{
  TMap<FString, FComponentIndexEntry> Index;
  FString Text;
  TSharedPtr<FJsonObject> Json;
  if (!FFileHelper::LoadFileToString(Text, *GetComponentIndexPath(InComponentsFolder)) ||
      !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid())
  {
    return Index;
  }
  for (const auto &Pair : Json->Values)
  {
    const TSharedPtr<FJsonObject> *EntryObject = nullptr;
    if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(EntryObject))
    {
      continue;
    }
    FComponentIndexEntry Entry;
    (*EntryObject)->TryGetStringField(TEXT("assetPath"), Entry.AssetPath);
    (*EntryObject)->TryGetStringField(TEXT("hash"), Entry.Hash);
    (*EntryObject)->TryGetStringField(TEXT("fileKey"), Entry.FileKey);
    (*EntryObject)->TryGetStringField(TEXT("nodeId"), Entry.NodeId);
    (*EntryObject)->TryGetStringField(TEXT("version"), Entry.Version);
    const UPackage *InMemory =
        Entry.AssetPath.IsEmpty() ? nullptr : FindPackage(nullptr, *Entry.AssetPath);
    const bool bExists = !Entry.AssetPath.IsEmpty() &&
                         (FPackageName::DoesPackageExist(Entry.AssetPath) ||
                          (InMemory && InMemory->FindAssetInPackage()));
    if (bExists)
    {
      Index.Add(FString(*Pair.Key), Entry);
    }
  }
  return Index;
}

bool UIWTDesignImport::Import(const FRequest &InRequest, FResult &OutResult, FString &OutError)
{
  FComponentRun Run;
  Run.Folder = NormalizeFolder(InRequest.ComponentsFolder);
  if (Run.Folder.IsEmpty())
  {
    Run.Folder = UUIWTDesignSettings::Get()->GetComponentsFolder();
  }
  Run.Index = ReadComponentIndex(Run.Folder);
  Run.bSave = InRequest.bSave;
  const bool bImported = ImportInternal(InRequest, Run, OutResult, OutError);
  OutResult.Components = Run.Lines;
  OutResult.Report.Append(Run.Report);
  return bImported;
}

bool UIWTDesignImport::ImportInternal(const FRequest &InRequest, FComponentRun &InOutRun,
                                      FResult &OutResult, FString &OutError)
{
  OutResult = FResult();
  const UUIWTDesignSettings *Settings = UUIWTDesignSettings::Get();

  // The tree.
  const FString DesignFile = FPaths::ConvertRelativePathToFull(InRequest.DesignFile);
  FString Json;
  if (!FFileHelper::LoadFileToString(Json, *DesignFile))
  {
    OutError = FString::Printf(TEXT("Could not read %s."), *DesignFile);
    return false;
  }
  FDocument Doc;
  TArray<FString> ReadErrors;
  if (!ReadDocument(Json, Doc, ReadErrors))
  {
    OutError = FString::Printf(TEXT("%s is not a usable design tree:\n- %s"), *DesignFile,
                               *FString::Join(ReadErrors, TEXT("\n- ")));
    return false;
  }
  if (InRequest.RootMode != ERootMode::Auto)
  {
    Doc.RootMode = InRequest.RootMode;
  }
  const FString DesignDir = FPaths::GetPath(DesignFile);

  // Where it goes.
  FString Folder = NormalizeFolder(InRequest.TargetFolder);
  if (Folder.IsEmpty())
  {
    Folder = Settings->GetImportFolder();
  }
  const FString Name = InRequest.BlueprintName.TrimStartAndEnd().IsEmpty()
                           ? DefaultBlueprintName(Doc)
                           : InRequest.BlueprintName.TrimStartAndEnd();
  if (!CheckTarget(Folder, Name, OutError))
  {
    return false;
  }
  const FString OwnFolder = Folder / Name;
  const FString PackageName = OwnFolder / Name;
  UClass *ParentClass = InRequest.ParentClass ? InRequest.ParentClass : UUserWidget::StaticClass();
  if (!ParentClass->IsChildOf(UUserWidget::StaticClass()))
  {
    OutError = FString::Printf(TEXT("%s is not a UserWidget class."), *ParentClass->GetName());
    return false;
  }

  // Convert, with the components index: instances without overrides become
  // child WBPs.
  FConvertOptions Options = FirstImportOptions(Folder, Name, ParentClass);
  Options.bInlineInstances = false;
  Options.ComponentsFolder = InOutRun.Folder;
  Options.ComponentIndex = InOutRun.Index;
  Options.TakenComponentNames = ComponentFolderNames(InOutRun.Folder);
  Options.bChildComponent = !InRequest.ComponentKey.IsEmpty();
  const FConvertResult Converted = Convert(Doc, Options);
  OutResult.Report = Converted.Report;
  OutResult.WidgetCount = Converted.WidgetCount;
  OutResult.MaxDepth = Converted.MaxDepth;
  OutResult.RootMode = Converted.RootMode;
  if (!Converted.Spec.IsValid())
  {
    OutError = FString::Printf(
        TEXT("The design needs %d widgets and %d levels; Apply allows %d and %d. The report "
             "lists the largest parts: turn repeated parts into components or import a "
             "smaller frame."),
        Converted.WidgetCount, Converted.MaxDepth + 1, MaxWidgets, MaxDepth + 1);
    return false;
  }

  // Child WBPs first: the spec refers to their classes.
  if (!ProcessComponents(Converted.Components, DesignDir, InOutRun, OutError))
  {
    return false;
  }

  // Textures, before the spec that uses them.
  TArray<UObject *> Created;
  TArray<UTexture2D *> Textures;
  TSet<FString> FailedTextures;
  for (const FTextureAsset &Asset : Converted.Assets)
  {
    const FString File = FPaths::ConvertRelativePathToFull(DesignDir / Asset.SourcePath);
    FString Error;
    FImage Pixels;
    FString TexturePackage;
    FString TextureName;
    Asset.ObjectPath.Split(TEXT("."), &TexturePackage, &TextureName, ESearchCase::CaseSensitive,
                           ESearchDir::FromEnd);
    UTexture2D *Texture = nullptr;
    bool bCreated = false;
    if (!IsInsideDirectory(File, DesignDir))
    {
      Error = TEXT("the image path leaves the design's folder");
    }
    else if (UIWTRunImages::LoadImageFile(File, Pixels, Error))
    {
      Texture = UIWTRunImages::WriteTexture(FPackageName::GetLongPackagePath(TexturePackage),
                                            TextureName, Pixels, bCreated, Error);
    }
    if (!Texture)
    {
      OutResult.Report.Add({FString(), TEXT("textureFailed"),
                            FString::Printf(TEXT("%s: %s; its brush has no texture"),
                                            *Asset.SourcePath, *Error)});
      FailedTextures.Add(Asset.ObjectPath);
      continue;
    }
    if (bCreated)
    {
      Created.Add(Texture);
    }
    Textures.Add(Texture);
  }
  OutResult.TexturesWritten = Textures.Num();
  // Apply gets a copy without the textures that failed; the sidecar keeps
  // the converter's spec, so a later re-import tries them again without
  // counting them as design changes.
  TSharedPtr<FJsonObject> ApplySpec = Converted.Spec;
  if (!FailedTextures.IsEmpty())
  {
    const TSharedPtr<FJsonValue> Whole = MakeShared<FJsonValueObject>(Converted.Spec);
    ApplySpec = FJsonValue::Duplicate(Whole)->AsObject();
    ClearReferences(ApplySpec, FailedTextures);
  }

  // The blueprint.
  UPackage *Package = CreatePackage(*PackageName);
  UWidgetBlueprint *Blueprint = FWidgetBlueprintOperationUtils::CreateWidgetBlueprint(
      Package, FName(*Name), BPTYPE_Normal, ParentClass);
  if (!Blueprint || !Blueprint->GeneratedClass)
  {
    DeleteCreated(Created);
    OutError = FString::Printf(TEXT("Could not create %s."), *PackageName);
    return false;
  }
  if (!UIWTWidgetSpec::Apply(Blueprint, ApplySpec.ToSharedRef(), OutResult.ApplyReport,
                             OutResult.ApplyErrors))
  {
    Created.Add(Blueprint);
    DeleteCreated(Created);
    OutError = TEXT("Apply refused the converted spec:\n- ") +
               FString::Join(OutResult.ApplyErrors, TEXT("\n- "));
    return false;
  }
  OutResult.Blueprint = Blueprint;
  OutResult.BlueprintPath = PackageName;
  Created.Add(Blueprint);
  for (UObject *Asset : Created)
  {
    OutResult.CreatedAssets.Add(Asset);
  }

  // Design-time size (import-figma.md step 4): the reference size for a
  // screen, the root's box or desired size for a widget.
  if (UUserWidget *Defaults = Blueprint->GeneratedClass->GetDefaultObject<UUserWidget>())
  {
    const FNode &Root = Doc.Root;
    Defaults->Modify();
    if (Converted.RootMode == ERootMode::Screen)
    {
      Defaults->DesignSizeMode = EDesignPreviewSizeMode::Custom;
      Defaults->DesignTimeSize = Doc.ReferenceSize;
    }
    else if (Root.SizingH == ESizing::Hug && Root.SizingV == ESizing::Hug)
    {
      Defaults->DesignSizeMode = EDesignPreviewSizeMode::Desired;
    }
    else
    {
      Defaults->DesignSizeMode = EDesignPreviewSizeMode::Custom;
      Defaults->DesignTimeSize = FVector2D(Root.Box.W, Root.Box.H);
    }
    if (Defaults->DesignSizeMode == EDesignPreviewSizeMode::Custom)
    {
      OutResult.DesignSize = FIntPoint(FMath::RoundToInt(Defaults->DesignTimeSize.X),
                                       FMath::RoundToInt(Defaults->DesignTimeSize.Y));
    }
    Blueprint->MarkPackageDirty();
  }

  // The reference image, where runs and render compares look for it.
  const FString OwnFolderOnDisk = UIWTGenerated::GetFolderOnDisk(OwnFolder);
  const FString Reference = InRequest.ReferenceImage.IsEmpty()
                                ? DesignDir / TEXT("reference.png")
                                : FPaths::ConvertRelativePathToFull(InRequest.ReferenceImage);
  IFileManager &Files = IFileManager::Get();
  if (InRequest.bCopyReference && !OwnFolderOnDisk.IsEmpty() && Files.FileExists(*Reference))
  {
    const FString Destination = OwnFolderOnDisk / TEXT("reference.png");
    Files.MakeDirectory(*OwnFolderOnDisk, true);
    if (Files.Copy(*Destination, *Reference, true, true) == COPY_OK)
    {
      OutResult.ReferenceFile = Destination;
    }
    else
    {
      OutResult.Report.Add({Doc.Root.Id, TEXT("referenceFailed"),
                            FString::Printf(TEXT("could not copy %s"), *Reference)});
    }
  }
  else if (InRequest.bCopyReference && !InRequest.ReferenceImage.IsEmpty())
  {
    OutResult.Report.Add({Doc.Root.Id, TEXT("referenceFailed"),
                          FString::Printf(TEXT("%s doesn't exist"), *Reference)});
  }

  // Save.
  if (InRequest.bSave)
  {
    bool bSavedAll = true;
    for (UTexture2D *Texture : Textures)
    {
      FString Error;
      if (!UIWTRunImages::SaveAsset(Texture, Error))
      {
        bSavedAll = false;
        OutResult.Report.Add({FString(), TEXT("saveFailed"), Error});
      }
    }
    FText SaveError;
    if (!UIWTGenerated::SaveWidgetBlueprint(Blueprint, SaveError))
    {
      bSavedAll = false;
      OutResult.Report.Add({FString(), TEXT("saveFailed"), SaveError.ToString()});
    }
    OutResult.bSaved = bSavedAll;
  }

  // The sidecar: what this import wrote, for the next one.
  OutResult.SidecarFile = GetSidecarPath(PackageName);
  FString SidecarError;
  if (OutResult.SidecarFile.IsEmpty() ||
      !WriteSidecar(OutResult.SidecarFile, Doc, DesignFile, ParentClass, Converted,
                    ExportNow(Blueprint), InOutRun.Folder, InRequest.ComponentKey, {},
                    SidecarError))
  {
    OutResult.Report.Add({FString(), TEXT("sidecarFailed"),
                          SidecarError.IsEmpty() ? TEXT("no folder on disk for the blueprint")
                                                 : SidecarError});
    OutResult.SidecarFile.Reset();
  }
  return true;
}

FString UIWTDesignImport::ResultToJson(const FResult &InResult)
{
  TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetStringField(TEXT("blueprint"), InResult.BlueprintPath);
  Object->SetStringField(TEXT("sidecar"), InResult.SidecarFile);
  Object->SetStringField(TEXT("reference"), InResult.ReferenceFile);
  Object->SetStringField(TEXT("rootMode"), RootModeName(InResult.RootMode));
  Object->SetStringField(TEXT("designSize"),
                         InResult.DesignSize == FIntPoint::ZeroValue
                             ? FString(TEXT("desired"))
                             : FString::Printf(TEXT("%dx%d"), InResult.DesignSize.X,
                                               InResult.DesignSize.Y));
  Object->SetNumberField(TEXT("widgets"), InResult.WidgetCount);
  Object->SetNumberField(TEXT("depth"), InResult.MaxDepth + 1);
  Object->SetNumberField(TEXT("textures"), InResult.TexturesWritten);
  Object->SetBoolField(TEXT("saved"), InResult.bSaved);
  TArray<TSharedPtr<FJsonValue>> Components;
  for (const FString &Line : InResult.Components)
  {
    Components.Add(MakeShared<FJsonValueString>(Line));
  }
  Object->SetArrayField(TEXT("components"), Components);
  Object->SetStringField(TEXT("applyReport"), InResult.ApplyReport);
  TArray<TSharedPtr<FJsonValue>> ApplyErrors;
  for (const FString &Error : InResult.ApplyErrors)
  {
    ApplyErrors.Add(MakeShared<FJsonValueString>(Error));
  }
  Object->SetArrayField(TEXT("applyErrors"), ApplyErrors);
  TArray<TSharedPtr<FJsonValue>> Report;
  for (const FReportEntry &Entry : InResult.Report)
  {
    TSharedRef<FJsonObject> EntryObject = MakeShared<FJsonObject>();
    EntryObject->SetStringField(TEXT("node"), Entry.Node);
    EntryObject->SetStringField(TEXT("category"), Entry.Category);
    EntryObject->SetStringField(TEXT("detail"), Entry.Detail);
    Report.Add(MakeShared<FJsonValueObject>(EntryObject));
  }
  Object->SetArrayField(TEXT("report"), Report);
  return PrettyJson(Object);
}

// ---------------------------------------------------------------------------
// Re-import

struct UIWTDesignImport::FReimportState
{
  TWeakObjectPtr<UWidgetBlueprint> Blueprint;
  FString Package;
  FString DesignFile;
  FString ReferenceImage;
  FDocument Doc;
  // The new conversion: the next re-import's base once accepted.
  FConvertResult Converted;
  TSharedPtr<FJsonObject> Merged;
  // For the next base's exported values (UIWTDesignMerge::NextExported).
  TMap<FString, UIWTDesignMerge::FBaseWidget> OldWidgets;
  TMap<FString, UIWTDesignMerge::FKeptValues> Kept;
  TMap<FString, UIWTDesignMerge::FExported> AfterApply;
  // Set by ApplyReimport, for Discard.
  TArray<TWeakObjectPtr<UTexture2D>> CreatedTextures;
  TArray<TWeakObjectPtr<UTexture2D>> ChangedTextures;
  // Child WBPs.
  FString ComponentsFolder;
  FString ComponentKey;
  TMap<FString, FComponentIndexEntry> Index;
  // The re-import's own run of child WBPs (only a re-import started
  // directly; a child's re-import shares its screen's).
  TSharedPtr<FComponentRun> Run;
  // Nodes the design added or changed, for the sidecar's changedNodes.
  TArray<FString> ChangedNodes;
};

namespace
{
  // Re-imports applied and waiting for Accept or Discard, by package.
  TMap<FString, TSharedPtr<UIWTDesignImport::FReimportState>> &PendingReimports()
  {
    static TMap<FString, TSharedPtr<UIWTDesignImport::FReimportState>> Pending;
    return Pending;
  }

  // Node ids only match within one file and frame: a re-import from anywhere
  // else would look like every layer was removed and a new one added.
  bool CheckSameSource(const UIWTDesignImport::FSidecar &InSidecar, const FDocument &InDoc,
                       FString &OutError)
  {
    if (!InSidecar.Source.IsEmpty() && InDoc.Source != InSidecar.Source)
    {
      OutError = FString::Printf(TEXT("The design comes from %s, but the blueprint was imported "
                                      "from %s."),
                                 *InDoc.Source, *InSidecar.Source);
      return false;
    }
    for (const TCHAR *Field : {TEXT("fileKey"), TEXT("nodeId")})
    {
      FString Before;
      FString Now;
      if (InSidecar.SourceRef.IsValid() && InDoc.SourceRef.IsValid() &&
          InSidecar.SourceRef->TryGetStringField(Field, Before) &&
          InDoc.SourceRef->TryGetStringField(Field, Now) && Before != Now)
      {
        OutError = FString::Printf(
            TEXT("The design's %s is %s, but the blueprint was imported from %s. Layer ids only "
                 "match within one file and frame, so re-import from the original (use the "
                 "file's version history rather than a copy)."),
            Field, *Now, *Before);
        return false;
      }
    }
    return true;
  }

  // Undoes ApplyReimport's texture writes: new textures are deleted, changed
  // ones reloaded from disk.
  void RestoreTextures(const UIWTDesignImport::FReimportState &InState)
  {
    TArray<UObject *> Created;
    for (const TWeakObjectPtr<UTexture2D> &Texture : InState.CreatedTextures)
    {
      if (Texture.IsValid())
      {
        Created.Add(Texture.Get());
      }
    }
    if (!Created.IsEmpty())
    {
      ObjectTools::ForceDeleteObjects(Created, false);
    }
    TArray<UPackage *> Changed;
    for (const TWeakObjectPtr<UTexture2D> &Texture : InState.ChangedTextures)
    {
      if (Texture.IsValid() && FPackageName::DoesPackageExist(Texture->GetOutermost()->GetName()))
      {
        Changed.AddUnique(Texture->GetOutermost());
      }
    }
    if (!Changed.IsEmpty())
    {
      FText Error;
      UPackageTools::ReloadPackages(Changed, Error, EReloadPackagesInteractionMode::AssumePositive);
    }
  }

  const TCHAR *ChangeName(UIWTDesignMerge::EChange InChange)
  {
    return InChange == UIWTDesignMerge::EChange::Added     ? TEXT("added")
           : InChange == UIWTDesignMerge::EChange::Removed ? TEXT("removed")
                                                           : TEXT("changed");
  }
}

bool UIWTDesignImport::ReadSidecar(const FString &InBlueprintPackage, FSidecar &OutSidecar,
                                   FString &OutError)
{
  OutSidecar = FSidecar();
  const FString Path = GetSidecarPath(InBlueprintPackage);
  FString Text;
  TSharedPtr<FJsonObject> Json;
  if (Path.IsEmpty() || !FFileHelper::LoadFileToString(Text, *Path) ||
      !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid())
  {
    OutError = FString::Printf(TEXT("%s has no design sidecar (%s), so it wasn't made by a "
                                    "design import and can't be re-imported."),
                               *InBlueprintPackage, *Path);
    return false;
  }
  Json->TryGetStringField(TEXT("source"), OutSidecar.Source);
  const TSharedPtr<FJsonObject> *Object = nullptr;
  if (Json->TryGetObjectField(TEXT("sourceRef"), Object))
  {
    OutSidecar.SourceRef = *Object;
  }
  FString DesignFile;
  Json->TryGetStringField(TEXT("designFile"), DesignFile);
  if (!DesignFile.IsEmpty())
  {
    OutSidecar.DesignFile = FPaths::IsRelative(DesignFile)
                                ? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), DesignFile)
                                : DesignFile;
  }
  Json->TryGetStringField(TEXT("parentClass"), OutSidecar.ParentClass);
  Json->TryGetStringField(TEXT("componentsFolder"), OutSidecar.ComponentsFolder);
  Json->TryGetStringField(TEXT("component"), OutSidecar.ComponentKey);
  if (Json->TryGetObjectField(TEXT("owned"), Object))
  {
    for (const auto &Node : (*Object)->Values)
    {
      const TSharedPtr<FJsonObject> Roles = Node.Value.IsValid() ? Node.Value->AsObject() : nullptr;
      if (Roles.IsValid())
      {
        FRoleNames &Names = OutSidecar.Owned.Add(FString(*Node.Key));
        for (const auto &Role : Roles->Values)
        {
          Names.Add(FString(*Role.Key), Role.Value->AsString());
        }
      }
    }
  }
  if (Json->TryGetObjectField(TEXT("textures"), Object))
  {
    for (const auto &Pair : (*Object)->Values)
    {
      OutSidecar.Textures.Add(FString(*Pair.Key), Pair.Value->AsString());
    }
  }
  if (Json->TryGetObjectField(TEXT("widgets"), Object))
  {
    UIWTDesignMerge::ReadBaseWidgets(*Object, OutSidecar.Widgets);
  }
  const TArray<TSharedPtr<FJsonValue>> *Array = nullptr;
  if (Json->TryGetArrayField(TEXT("report"), Array))
  {
    for (const TSharedPtr<FJsonValue> &Value : *Array)
    {
      const TSharedPtr<FJsonObject> Entry = Value->AsObject();
      if (Entry.IsValid())
      {
        OutSidecar.Report.Add({Entry->GetStringField(TEXT("node")),
                               Entry->GetStringField(TEXT("category")),
                               Entry->GetStringField(TEXT("detail"))});
      }
    }
  }
  if (Json->TryGetArrayField(TEXT("changedNodes"), Array))
  {
    for (const TSharedPtr<FJsonValue> &Value : *Array)
    {
      OutSidecar.ChangedNodes.Add(Value->AsString());
    }
  }
  return true;
}

bool UIWTDesignImport::ClearChangedNodes(const FString &InBlueprintPackage, FString &OutError)
{
  const FString Path = GetSidecarPath(InBlueprintPackage);
  FString Text;
  TSharedPtr<FJsonObject> Json;
  if (Path.IsEmpty() || !FFileHelper::LoadFileToString(Text, *Path) ||
      !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid())
  {
    OutError = FString::Printf(TEXT("%s has no readable design sidecar (%s)."),
                               *InBlueprintPackage, *Path);
    return false;
  }
  if (!Json->HasField(TEXT("changedNodes")))
  {
    return true;
  }
  Json->RemoveField(TEXT("changedNodes"));
  if (!FFileHelper::SaveStringToFile(PrettyJson(Json.ToSharedRef()), *Path,
                                     FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
  {
    OutError = FString::Printf(TEXT("Could not write %s."), *Path);
    return false;
  }
  return true;
}

bool UIWTDesignImport::RenameInSidecar(const FString &InBlueprintPackage,
                                       const TMap<FString, FString> &InRenames,
                                       const TSet<FString> &InRemoved, FString &OutError)
{
  const FString Path = GetSidecarPath(InBlueprintPackage);
  FString Text;
  TSharedPtr<FJsonObject> Json;
  if (Path.IsEmpty() || !FFileHelper::LoadFileToString(Text, *Path) ||
      !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid())
  {
    OutError = FString::Printf(TEXT("%s has no readable design sidecar (%s)."),
                               *InBlueprintPackage, *Path);
    return false;
  }
  TSet<FString> Targets;
  for (const TPair<FString, FString> &Rename : InRenames)
  {
    Targets.Add(Rename.Value);
  }
  auto Renamed = [&InRenames](const FString &InName)
  {
    const FString *Found = InRenames.Find(InName);
    return Found ? *Found : InName;
  };
  // A deleted widget whose name a renamed one took: the name isn't it now.
  auto IsStale = [&](const FString &InName)
  { return !InRenames.Contains(InName) && InRemoved.Contains(InName) && Targets.Contains(InName); };

  const TSharedPtr<FJsonObject> *Object = nullptr;
  if (Json->TryGetObjectField(TEXT("owned"), Object))
  {
    TSharedRef<FJsonObject> Owned = MakeShared<FJsonObject>();
    for (const auto &Node : (*Object)->Values)
    {
      const TSharedPtr<FJsonObject> Roles = Node.Value.IsValid() ? Node.Value->AsObject() : nullptr;
      if (!Roles.IsValid())
      {
        continue;
      }
      TSharedRef<FJsonObject> NewRoles = MakeShared<FJsonObject>();
      for (const auto &Role : Roles->Values)
      {
        const FString Name = Role.Value->AsString();
        if (!IsStale(Name))
        {
          NewRoles->SetStringField(FString(*Role.Key), Renamed(Name));
        }
      }
      if (!NewRoles->Values.IsEmpty())
      {
        Owned->SetObjectField(FString(*Node.Key), NewRoles);
      }
    }
    Json->SetObjectField(TEXT("owned"), Owned);
  }
  if (Json->TryGetObjectField(TEXT("widgets"), Object))
  {
    TMap<FString, TSharedPtr<FJsonObject>> Widgets;
    for (const auto &Pair : (*Object)->Values)
    {
      const FString Name(*Pair.Key);
      const TSharedPtr<FJsonObject> Entry = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
      if (!Entry.IsValid() || IsStale(Name))
      {
        continue;
      }
      FString Parent;
      if (Entry->TryGetStringField(TEXT("parent"), Parent) && !Parent.IsEmpty())
      {
        Entry->SetStringField(TEXT("parent"), Renamed(Parent));
      }
      Widgets.Add(Renamed(Name), Entry);
    }
    Widgets.KeySort(TLess<FString>());
    TSharedRef<FJsonObject> WidgetsObject = MakeShared<FJsonObject>();
    for (const TPair<FString, TSharedPtr<FJsonObject>> &Pair : Widgets)
    {
      WidgetsObject->SetObjectField(Pair.Key, Pair.Value);
    }
    Json->SetObjectField(TEXT("widgets"), WidgetsObject);
  }
  Json->SetStringField(TEXT("refinedAt"), FDateTime::UtcNow().ToIso8601());
  if (!FFileHelper::SaveStringToFile(PrettyJson(Json.ToSharedRef()), *Path,
                                     FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
  {
    OutError = FString::Printf(TEXT("Could not write %s."), *Path);
    return false;
  }
  return true;
}

bool UIWTDesignImport::PlanReimport(const FReimportRequest &InRequest, FReimportPlan &OutPlan,
                                    FString &OutError)
{
  return PlanReimportInternal(InRequest, OutPlan, nullptr, OutError);
}

bool UIWTDesignImport::PlanReimportInternal(const FReimportRequest &InRequest,
                                            FReimportPlan &OutPlan, FComponentRun *InRun,
                                            FString &OutError)
{
  OutPlan = FReimportPlan();
  UWidgetBlueprint *Blueprint = InRequest.Blueprint;
  if (!Blueprint || !Blueprint->WidgetTree)
  {
    OutError = TEXT("No Widget Blueprint to re-import into.");
    return false;
  }
  const FString Package = Blueprint->GetOutermost()->GetName();
  if (PendingReimports().Contains(Package))
  {
    OutError = FString::Printf(TEXT("%s has a re-import waiting to be accepted or discarded."),
                               *Package);
    return false;
  }
  FSidecar Sidecar;
  if (!ReadSidecar(Package, Sidecar, OutError))
  {
    return false;
  }

  // The design now.
  const FString DesignFile = FPaths::ConvertRelativePathToFull(
      InRequest.DesignFile.IsEmpty() ? Sidecar.DesignFile : InRequest.DesignFile);
  FString Json;
  TSharedRef<FReimportState> State = MakeShared<FReimportState>();
  TArray<FString> ReadErrors;
  if (!FFileHelper::LoadFileToString(Json, *DesignFile) ||
      !ReadDocument(Json, State->Doc, ReadErrors))
  {
    OutError = FString::Printf(TEXT("%s can't be read as a design tree. %s"), *DesignFile,
                               *FString::Join(ReadErrors, TEXT("; ")));
    return false;
  }
  if (!CheckSameSource(Sidecar, State->Doc, OutError))
  {
    return false;
  }

  // The blueprint now, and what depends on its widgets.
  FString Exported;
  FString ExportError;
  TSharedPtr<FJsonObject> BlueprintSpec;
  if (!UIWTWidgetSpec::Export(Blueprint, Exported, ExportError) ||
      !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Exported), BlueprintSpec))
  {
    OutError = TEXT("Exporting the blueprint failed: ") + ExportError;
    return false;
  }
  UIWTDesignMerge::FInput Input;
  TSet<FString> ProtectedNames;
  for (const UIWTWidgetSpec::FProtectedWidget &Widget :
       UIWTWidgetSpec::FindProtectedWidgets(Blueprint))
  {
    const FString Name = Widget.Name.ToString();
    Input.Protected.Add(Name, Widget.Reasons);
    ProtectedNames.Add(Name);
    OutPlan.Protected.Add(
        FString::Printf(TEXT("%s (%s)"), *Name, *FString::Join(Widget.Reasons, TEXT(", "))));
  }

  // The design converted with the last import's names and textures.
  const FString OwnFolder = FPackageName::GetLongPackagePath(Package);
  const FString Name = Blueprint->GetName();
  const FString Folder = FPackageName::GetShortName(OwnFolder) == Name
                             ? FPackageName::GetLongPackagePath(OwnFolder)
                             : OwnFolder;
  FConvertOptions Options = FirstImportOptions(Folder, Name, Blueprint->ParentClass);
  Options.Names = Sidecar.Owned;
  Options.Textures = Sidecar.Textures;
  Options.ReservedNames = ReservedNamesForBlueprint(Blueprint, Sidecar.Owned, ProtectedNames);
  // Child WBPs: the screen's run when this is a child's re-import, else the
  // components folder the last import used.
  State->ComponentsFolder =
      InRun ? InRun->Folder
            : (Sidecar.ComponentsFolder.IsEmpty() ? UUIWTDesignSettings::Get()->GetComponentsFolder()
                                                  : Sidecar.ComponentsFolder);
  State->Index = InRun ? InRun->Index : ReadComponentIndex(State->ComponentsFolder);
  State->ComponentKey = Sidecar.ComponentKey;
  Options.bInlineInstances = false;
  Options.ComponentsFolder = State->ComponentsFolder;
  Options.ComponentIndex = State->Index;
  Options.TakenComponentNames = ComponentFolderNames(State->ComponentsFolder);
  Options.bChildComponent = !Sidecar.ComponentKey.IsEmpty();
  State->Converted = Convert(State->Doc, Options);
  for (const FComponentJob &Job : State->Converted.Components)
  {
    OutPlan.Components.Add(FString::Printf(
        TEXT("%s: %s %s"), *Job.Key,
        Job.Action == EComponentAction::Create   ? TEXT("create")
        : Job.Action == EComponentAction::Update ? TEXT("update")
                                                 : TEXT("reuse"),
        *Job.AssetPath));
  }
  OutPlan.Report = State->Converted.Report;
  OutPlan.WidgetCount = State->Converted.WidgetCount;
  if (!State->Converted.Spec.IsValid())
  {
    OutError = FString::Printf(
        TEXT("The design needs %d widgets and %d levels; Apply allows %d and %d. The report "
             "lists the largest parts."),
        State->Converted.WidgetCount, State->Converted.MaxDepth + 1, MaxWidgets, MaxDepth + 1);
    return false;
  }

  // The merge.
  Input.BaseOwned = Sidecar.Owned;
  Input.BaseWidgets = Sidecar.Widgets;
  Input.Source = State->Converted.Spec;
  Input.SourceOwned = State->Converted.Owned;
  Input.Blueprint = BlueprintSpec;
  UIWTWidgetSpec::FClassInfo Classes;
  UIWTDesignMerge::FResult Merged = UIWTDesignMerge::Merge(Input, Classes);
  OutPlan.Report.Append(Merged.Report);
  if (!Merged.Spec.IsValid())
  {
    OutError = TEXT("The merge left no root widget.");
    return false;
  }

  State->Blueprint = Blueprint;
  State->Package = Package;
  State->DesignFile = DesignFile;
  State->ReferenceImage = !InRequest.bUseReference ? FString()
                          : InRequest.ReferenceImage.IsEmpty()
                              ? FPaths::GetPath(DesignFile) / TEXT("reference.png")
                              : FPaths::ConvertRelativePathToFull(InRequest.ReferenceImage);
  State->Merged = Merged.Spec;
  State->OldWidgets = MoveTemp(Sidecar.Widgets);
  State->Kept = MoveTemp(Merged.KeptFromBlueprint);
  for (const UIWTDesignMerge::FChange &Change : Merged.Changes)
  {
    if (Change.Change != UIWTDesignMerge::EChange::Removed)
    {
      State->ChangedNodes.Add(Change.Node);
    }
  }
  OutPlan.BlueprintPath = Package;
  OutPlan.Changes = MoveTemp(Merged.Changes);
  OutPlan.Conflicts = Merged.Conflicts;
  OutPlan.KeptUEChanges = Merged.KeptUEChanges;
  OutPlan.UEWidgets = Merged.UEWidgets;
  OutPlan.State = State;
  return true;
}

bool UIWTDesignImport::ApplyReimport(const FReimportPlan &InPlan, FReimportResult &OutResult,
                                     FString &OutError)
{
  return ApplyReimportInternal(InPlan, OutResult, nullptr, OutError);
}

bool UIWTDesignImport::ApplyReimportInternal(const FReimportPlan &InPlan,
                                             FReimportResult &OutResult, FComponentRun *InRun,
                                             FString &OutError)
{
  OutResult = FReimportResult();
  if (!InPlan.State.IsValid())
  {
    OutError = TEXT("The plan is empty; call PlanReimport first.");
    return false;
  }
  FReimportState &State = *InPlan.State;
  UWidgetBlueprint *Blueprint = State.Blueprint.Get();
  if (!Blueprint)
  {
    OutError = TEXT("The blueprint the plan was made for is gone.");
    return false;
  }
  if (PendingReimports().Contains(State.Package))
  {
    OutError = FString::Printf(TEXT("%s has a re-import waiting to be accepted or discarded."),
                               *State.Package);
    return false;
  }

  // Child WBPs first, left unsaved: the spec refers to their classes. A
  // re-import started directly has its own run; a child's shares its
  // screen's.
  const FString DesignDir = FPaths::GetPath(State.DesignFile);
  TSharedPtr<FComponentRun> OwnRun;
  FComponentRun *Run = InRun;
  if (!Run)
  {
    OwnRun = MakeShared<FComponentRun>();
    OwnRun->Folder = State.ComponentsFolder;
    OwnRun->Index = State.Index;
    OwnRun->bPending = true;
    OwnRun->bSave = false;
    Run = OwnRun.Get();
  }
  if (!ProcessComponents(State.Converted.Components, DesignDir, *Run, OutError))
  {
    if (OwnRun)
    {
      DiscardRun(*OwnRun);
    }
    return false;
  }

  // Textures next: the spec refers to them. A texture from the last import
  // gets its new pixels in place.
  TArray<FString> TextureErrors;
  TSet<FString> FailedTextures;
  State.CreatedTextures.Reset();
  State.ChangedTextures.Reset();
  for (const FTextureAsset &Asset : State.Converted.Assets)
  {
    const FString File = FPaths::ConvertRelativePathToFull(DesignDir / Asset.SourcePath);
    FString Error;
    FImage Pixels;
    FString TexturePackage;
    FString TextureName;
    Asset.ObjectPath.Split(TEXT("."), &TexturePackage, &TextureName, ESearchCase::CaseSensitive,
                           ESearchDir::FromEnd);
    UTexture2D *Texture = nullptr;
    bool bCreated = false;
    if (!IsInsideDirectory(File, DesignDir))
    {
      Error = TEXT("the image path leaves the design's folder");
    }
    else if (UIWTRunImages::LoadImageFile(File, Pixels, Error))
    {
      Texture = UIWTRunImages::WriteTexture(FPackageName::GetLongPackagePath(TexturePackage),
                                            TextureName, Pixels, bCreated, Error);
    }
    if (!Texture)
    {
      TextureErrors.Add(FString::Printf(TEXT("texture %s: %s"), *Asset.SourcePath, *Error));
      // A texture from an earlier import still draws; a new one that
      // couldn't be made leaves its brush empty.
      if (!FindObject<UTexture2D>(nullptr, *Asset.ObjectPath) &&
          !FPackageName::DoesPackageExist(TexturePackage))
      {
        FailedTextures.Add(Asset.ObjectPath);
      }
      continue;
    }
    ++OutResult.TexturesWritten;
    if (bCreated)
    {
      ++OutResult.TexturesCreated;
      State.CreatedTextures.Add(Texture);
    }
    else
    {
      State.ChangedTextures.Add(Texture);
    }
  }

  ClearReferences(State.Merged, FailedTextures);
  if (!UIWTWidgetSpec::Apply(Blueprint, State.Merged.ToSharedRef(), OutResult.ApplyReport,
                             OutResult.ApplyErrors))
  {
    RestoreTextures(State);
    if (OwnRun)
    {
      DiscardRun(*OwnRun);
    }
    OutError = TEXT("Apply refused the merged spec; nothing was changed:\n- ") +
               FString::Join(OutResult.ApplyErrors, TEXT("\n- "));
    return false;
  }
  OutResult.ApplyErrors.Append(TextureErrors);
  State.AfterApply = ExportNow(Blueprint);
  State.Run = OwnRun;
  if (OwnRun)
  {
    OutResult.Components = OwnRun->Lines;
    OutResult.ComponentReport = OwnRun->Report;
  }
  PendingReimports().Add(State.Package, InPlan.State);

  // Step 7: the result next to the new reference.
  const FString OwnDir =
      UIWTGenerated::GetFolderOnDisk(FPackageName::GetLongPackagePath(State.Package));
  FImage Reference;
  FString Error;
  if (FApp::CanEverRender() && !OwnDir.IsEmpty() &&
      UIWTRunImages::LoadImageFile(State.ReferenceImage, Reference, Error))
  {
    FImage Rendered;
    if (UIWTRunImages::RenderWidget(Blueprint, FIntPoint(Reference.SizeX, Reference.SizeY),
                                    Rendered, Error))
    {
      FImage Both;
      UIWTRunImages::SideBySide(Reference, Rendered, Both);
      OutResult.RenderFile = OwnDir / TEXT("render_reimport.png");
      OutResult.CompareFile = OwnDir / TEXT("compare_reimport.png");
      UIWTRunImages::SavePng(Rendered, OutResult.RenderFile, Error);
      UIWTRunImages::SavePng(Both, OutResult.CompareFile, Error);
      OutResult.MeanDifference = UIWTRunImages::MeanDifference(Reference, Rendered);
    }
  }
  return true;
}

bool UIWTDesignImport::HasPendingReimport(const UWidgetBlueprint *InBlueprint)
{
  return InBlueprint && PendingReimports().Contains(InBlueprint->GetOutermost()->GetName());
}

void UIWTDesignImport::ForgetReimport(const FString &InPackage)
{
  PendingReimports().Remove(InPackage);
}

bool UIWTDesignImport::AcceptReimport(UWidgetBlueprint *InBlueprint, bool bInSave,
                                      FString &OutError)
{
  const FString Package = InBlueprint ? InBlueprint->GetOutermost()->GetName() : FString();
  const TSharedPtr<FReimportState> *Found = PendingReimports().Find(Package);
  if (!Found)
  {
    OutError = TEXT("There is no applied re-import to accept for this blueprint.");
    return false;
  }
  // The children first: the blueprint refers to them.
  if (const TSharedPtr<FComponentRun> Run = (*Found)->Run)
  {
    for (const FString &Child : Run->UpdatedPackages)
    {
      if (!AcceptSelf(Child, bInSave, OutError))
      {
        return false;
      }
    }
    for (const TWeakObjectPtr<UObject> &Asset : Run->CreatedAssets)
    {
      if (!bInSave || !Asset.IsValid())
      {
        continue;
      }
      FText SaveError;
      UWidgetBlueprint *Child = Cast<UWidgetBlueprint>(Asset.Get());
      if (Child ? !UIWTGenerated::SaveWidgetBlueprint(Child, SaveError)
                : !UIWTRunImages::SaveAsset(Asset.Get(), OutError))
      {
        OutError = Child ? SaveError.ToString() : OutError;
        return false;
      }
    }
    WriteComponentIndex(Run->Folder, Run->Index);
  }
  return AcceptSelf(Package, bInSave, OutError);
}

bool UIWTDesignImport::AcceptSelf(const FString &InPackage, bool bInSave, FString &OutError)
{
  const FString Package = InPackage;
  const TSharedPtr<FReimportState> *Found = PendingReimports().Find(Package);
  UWidgetBlueprint *InBlueprint = Found ? (*Found)->Blueprint.Get() : nullptr;
  if (!Found || !InBlueprint)
  {
    OutError = FString::Printf(TEXT("There is no applied re-import to accept for %s."), *Package);
    return false;
  }
  const FReimportState &State = **Found;
  if (bInSave)
  {
    for (const TWeakObjectPtr<UTexture2D> &Texture : State.CreatedTextures)
    {
      if (Texture.IsValid() && !UIWTRunImages::SaveAsset(Texture.Get(), OutError))
      {
        return false;
      }
    }
    for (const TWeakObjectPtr<UTexture2D> &Texture : State.ChangedTextures)
    {
      if (Texture.IsValid() && !UIWTRunImages::SaveAsset(Texture.Get(), OutError))
      {
        return false;
      }
    }
    FText SaveError;
    if (!UIWTGenerated::SaveWidgetBlueprint(InBlueprint, SaveError))
    {
      OutError = SaveError.ToString();
      return false;
    }
  }

  const FString OwnDir = UIWTGenerated::GetFolderOnDisk(FPackageName::GetLongPackagePath(Package));
  IFileManager &Files = IFileManager::Get();
  if (!OwnDir.IsEmpty() && Files.FileExists(*State.ReferenceImage))
  {
    Files.Copy(*(OwnDir / TEXT("reference.png")), *State.ReferenceImage, true, true);
  }
  // The new conversion is what the next re-import compares against.
  if (!WriteSidecar(GetSidecarPath(Package), State.Doc, State.DesignFile, InBlueprint->ParentClass,
                    State.Converted,
                    UIWTDesignMerge::NextExported(State.AfterApply, State.OldWidgets, State.Kept),
                    State.ComponentsFolder, State.ComponentKey, State.ChangedNodes, OutError))
  {
    return false;
  }
  PendingReimports().Remove(Package);
  return true;
}

bool UIWTDesignImport::DiscardReimport(UWidgetBlueprint *InBlueprint, FString &OutError)
{
  const FString Package = InBlueprint ? InBlueprint->GetOutermost()->GetName() : FString();
  const TSharedPtr<FReimportState> *Found = PendingReimports().Find(Package);
  if (!Found)
  {
    OutError = TEXT("There is no applied re-import to discard for this blueprint.");
    return false;
  }
  const TSharedPtr<FReimportState> State = *Found;
  if (!DiscardSelf(Package, OutError))
  {
    return false;
  }
  // The children after: the reloaded blueprint no longer refers to new ones.
  if (State->Run)
  {
    DiscardRun(*State->Run);
  }
  return true;
}

bool UIWTDesignImport::DiscardSelf(const FString &InPackage, FString &OutError)
{
  const TSharedPtr<FReimportState> *Found = PendingReimports().Find(InPackage);
  if (!Found)
  {
    OutError = FString::Printf(TEXT("There is no applied re-import to discard for %s."),
                               *InPackage);
    return false;
  }
  if (!FPackageName::DoesPackageExist(InPackage))
  {
    OutError = FString::Printf(TEXT("%s was never saved, so there is nothing on disk to go back "
                                    "to."),
                               *InPackage);
    return false;
  }
  const TSharedPtr<FReimportState> State = *Found;
  // The blueprint first: once reloaded it no longer uses the new textures.
  FText ReloadError;
  if (!State->Blueprint.IsValid() ||
      !UIWTGenerated::ReloadWidgetBlueprint(State->Blueprint.Get(), ReloadError))
  {
    OutError = ReloadError.IsEmpty() ? TEXT("The blueprint is gone.") : ReloadError.ToString();
    return false;
  }
  RestoreTextures(*State);
  PendingReimports().Remove(InPackage);
  return true;
}

FString UIWTDesignImport::PlanToJson(const FReimportPlan &InPlan)
{
  TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetStringField(TEXT("blueprint"), InPlan.BlueprintPath);
  TArray<TSharedPtr<FJsonValue>> Changes;
  for (const UIWTDesignMerge::FChange &Change : InPlan.Changes)
  {
    TSharedRef<FJsonObject> ChangeObject = MakeShared<FJsonObject>();
    ChangeObject->SetStringField(TEXT("node"), Change.Node);
    ChangeObject->SetStringField(TEXT("change"), ChangeName(Change.Change));
    TArray<TSharedPtr<FJsonValue>> Details;
    for (const FString &Detail : Change.Details)
    {
      Details.Add(MakeShared<FJsonValueString>(Detail));
    }
    ChangeObject->SetArrayField(TEXT("details"), Details);
    Changes.Add(MakeShared<FJsonValueObject>(ChangeObject));
  }
  Object->SetArrayField(TEXT("changes"), Changes);
  Object->SetNumberField(TEXT("conflicts"), InPlan.Conflicts);
  Object->SetNumberField(TEXT("keptUEChanges"), InPlan.KeptUEChanges);
  Object->SetNumberField(TEXT("ueWidgets"), InPlan.UEWidgets);
  Object->SetNumberField(TEXT("widgets"), InPlan.WidgetCount);
  TArray<TSharedPtr<FJsonValue>> Protected;
  for (const FString &Widget : InPlan.Protected)
  {
    Protected.Add(MakeShared<FJsonValueString>(Widget));
  }
  Object->SetArrayField(TEXT("protected"), Protected);
  TArray<TSharedPtr<FJsonValue>> Components;
  for (const FString &Line : InPlan.Components)
  {
    Components.Add(MakeShared<FJsonValueString>(Line));
  }
  Object->SetArrayField(TEXT("components"), Components);
  TArray<TSharedPtr<FJsonValue>> Report;
  for (const FReportEntry &Entry : InPlan.Report)
  {
    TSharedRef<FJsonObject> EntryObject = MakeShared<FJsonObject>();
    EntryObject->SetStringField(TEXT("node"), Entry.Node);
    EntryObject->SetStringField(TEXT("category"), Entry.Category);
    EntryObject->SetStringField(TEXT("detail"), Entry.Detail);
    Report.Add(MakeShared<FJsonValueObject>(EntryObject));
  }
  Object->SetArrayField(TEXT("report"), Report);
  return PrettyJson(Object);
}

FString UIWTDesignImport::ReimportResultToJson(const FReimportResult &InResult)
{
  TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetStringField(TEXT("applyReport"), InResult.ApplyReport);
  TArray<TSharedPtr<FJsonValue>> Errors;
  for (const FString &Error : InResult.ApplyErrors)
  {
    Errors.Add(MakeShared<FJsonValueString>(Error));
  }
  Object->SetArrayField(TEXT("applyErrors"), Errors);
  Object->SetNumberField(TEXT("texturesWritten"), InResult.TexturesWritten);
  Object->SetNumberField(TEXT("texturesCreated"), InResult.TexturesCreated);
  if (InResult.MeanDifference >= 0.0)
  {
    Object->SetStringField(TEXT("render"), InResult.RenderFile);
    Object->SetStringField(TEXT("compare"), InResult.CompareFile);
    Object->SetNumberField(TEXT("meanDifferencePercent"),
                           FMath::RoundToDouble(InResult.MeanDifference * 100.0) / 100.0);
  }
  TArray<TSharedPtr<FJsonValue>> Components;
  for (const FString &Line : InResult.Components)
  {
    Components.Add(MakeShared<FJsonValueString>(Line));
  }
  Object->SetArrayField(TEXT("components"), Components);
  TArray<TSharedPtr<FJsonValue>> ComponentReport;
  for (const FReportEntry &Entry : InResult.ComponentReport)
  {
    TSharedRef<FJsonObject> EntryObject = MakeShared<FJsonObject>();
    EntryObject->SetStringField(TEXT("node"), Entry.Node);
    EntryObject->SetStringField(TEXT("category"), Entry.Category);
    EntryObject->SetStringField(TEXT("detail"), Entry.Detail);
    ComponentReport.Add(MakeShared<FJsonValueObject>(EntryObject));
  }
  Object->SetArrayField(TEXT("componentReport"), ComponentReport);
  Object->SetStringField(TEXT("next"), TEXT("unsaved: accept to save and update the sidecar, or "
                                            "discard to reload from disk"));
  return PrettyJson(Object);
}
