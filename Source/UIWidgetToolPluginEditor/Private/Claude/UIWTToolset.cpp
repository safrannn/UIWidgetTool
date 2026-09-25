#include "UIWTToolset.h"

#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/Widget.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ToolsetRegistry/ToolsetLibrary.h"
#include "UIWTChatTypes.h"
#include "UIWTClaudeService.h"
#include "UIWTLocalSettings.h"
#include "Core/UIWTGeneratedBlueprints.h"
#include "Core/UIWTRunImages.h"
#include "Core/UIWTWidgetSpec.h"
#include "Engine/Texture2D.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "WidgetBlueprint.h"

namespace
{
  // Null when no run is active; the script error is already raised.
  const FUIWTActiveRun *RequireActiveRun()
  {
    const FUIWTActiveRun *Run =
        FUIWTClaudeService::Get().GetActiveRun();
    if (!Run || !Run->IsValid())
    {
      UKismetSystemLibrary::RaiseScriptError(
          TEXT("No UI Widget Tool run is active. Tools only work while the "
               "manager window has a run in flight."));
      return nullptr;
    }
    return Run;
  }

  UWidgetBlueprint *ResolveRunBlueprint(const FUIWTActiveRun &Run)
  {
    UWidgetBlueprint *Blueprint =
        Cast<UWidgetBlueprint>(Run.BlueprintPath.TryLoad());
    if (!Blueprint)
    {
      UKismetSystemLibrary::RaiseScriptError(FString::Printf(
          TEXT("The run's blueprint %s could not be loaded."),
          *Run.BlueprintPath.ToString()));
    }
    return Blueprint;
  }

  // The object must belong to the run's blueprint: a widget in its
  // tree, or a slot on one of those widgets.
  bool ObjectBelongsToRun(UObject *Object, const FUIWTActiveRun &Run)
  {
    if (!Object)
    {
      UKismetSystemLibrary::RaiseScriptError(TEXT("Object is null."));
      return false;
    }
    UWidgetBlueprint *Blueprint = ResolveRunBlueprint(Run);
    if (!Blueprint)
    {
      return false;
    }
    if (Object->IsA<UWidget>() || Object->IsA<UPanelSlot>())
    {
      if (Blueprint->WidgetTree && Object->IsIn(Blueprint->WidgetTree))
      {
        return true;
      }
    }
    UKismetSystemLibrary::RaiseScriptError(FString::Printf(
        TEXT("%s is not a widget or slot in the run's blueprint %s. Only the "
             "blueprint returned by GetContext may be edited."),
        *Object->GetPathName(), *Run.BlueprintPath.ToString()));
    return false;
  }

  bool IsRunBlueprint(const UWidgetBlueprint *Blueprint,
                      const FUIWTActiveRun &Run)
  {
    if (!Blueprint || FSoftObjectPath(Blueprint) != Run.BlueprintPath)
    {
      UKismetSystemLibrary::RaiseScriptError(FString::Printf(
          TEXT("Only the run's blueprint %s may be used."),
          *Run.BlueprintPath.ToString()));
      return false;
    }
    return true;
  }

  // The image tools record files and textures on the run.
  FUIWTActiveRun *RequireActiveRunMutable()
  {
    return RequireActiveRun()
               ? FUIWTClaudeService::Get().GetActiveRunMutable()
               : nullptr;
  }

  // Textures live in the blueprint's own folder, so a blueprint under
  // Content/ never references one in the generated mount or the other way
  // round.
  FString GetTextureFolder(const FUIWTActiveRun &Run)
  {
    return Run.ContentFolder;
  }

  bool LoadReference(const FUIWTActiveRun &Run, FImage &OutImage)
  {
    if (Run.ReferenceImagePath.IsEmpty())
    {
      UKismetSystemLibrary::RaiseScriptError(
          TEXT("This entry has no reference image; the user has not "
               "attached one."));
      return false;
    }
    FString Error;
    if (!UIWTRunImages::LoadImageFile(Run.ReferenceImagePath, OutImage, Error))
    {
      UKismetSystemLibrary::RaiseScriptError(Error);
      return false;
    }
    return true;
  }

  FString NextRunFile(FUIWTActiveRun &Run, const TCHAR *Prefix)
  {
    return Run.RunDirectory /
           FString::Printf(TEXT("%s_%03d.png"), Prefix, ++Run.FileCounter);
  }

  FString RecordTexture(FUIWTActiveRun &Run, UTexture2D *Texture)
  {
    const FSoftObjectPath Path(Texture);
    Run.Textures.AddUnique(Path);
    return Path.ToString();
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
}

FUIWTToolContext UUIWTToolset::GetContext()
{
  FUIWTToolContext Context;
  const FUIWTActiveRun *Run = RequireActiveRun();
  if (!Run)
  {
    return Context;
  }

  const FWidgetPreviewObject *Entry =
      UUIWidgetPreviewObjectManagerSettings::Get()->FindWidgetPreviewObject(
          Run->EntryId);
  if (!Entry)
  {
    UKismetSystemLibrary::RaiseScriptError(
        TEXT("The run's entry no longer exists."));
    return Context;
  }

  Context.BlueprintPath = Run->BlueprintPath.ToString();
  Context.EntryId = Run->EntryId.ToString();
  Context.Level = Run->LevelPackagePath;
  Context.Checkpoint = Run->CheckpointDisplay;
  Context.PickedWidget = Run->PickedWidget;
  Context.Note = Entry->Note;
  Context.RunFolder = Run->RunDirectory;
  Context.TextureFolder = GetTextureFolder(*Run);
  FImage Reference;
  FString Ignored;
  if (!Run->ReferenceImagePath.IsEmpty() &&
      UIWTRunImages::LoadImageFile(Run->ReferenceImagePath, Reference, Ignored))
  {
    Context.ReferenceImage = Run->ReferenceImagePath;
    Context.ReferenceWidth = Reference.SizeX;
    Context.ReferenceHeight = Reference.SizeY;
  }
  return Context;
}

bool UUIWTToolset::SaveWidgetBlueprint(UWidgetBlueprint *WidgetBlueprint)
{
  const FUIWTActiveRun *Run = RequireActiveRun();
  if (!Run)
  {
    return false;
  }
  if (!WidgetBlueprint ||
      FSoftObjectPath(WidgetBlueprint) != Run->BlueprintPath)
  {
    UKismetSystemLibrary::RaiseScriptError(FString::Printf(
        TEXT("Only the run's blueprint %s may be saved."),
        *Run->BlueprintPath.ToString()));
    return false;
  }
  // Edits made since the last compile have not been checked yet.
  if (WidgetBlueprint->Status != BS_UpToDate &&
      WidgetBlueprint->Status != BS_UpToDateWithWarnings)
  {
    FKismetEditorUtilities::CompileBlueprint(
        WidgetBlueprint, EBlueprintCompileOptions::SkipGarbageCollection);
  }
  if (WidgetBlueprint->Status == BS_Error)
  {
    UKismetSystemLibrary::RaiseScriptError(
        TEXT("The blueprint has compile errors; fix them and compile before "
             "saving."));
    return false;
  }
  // Textures first, so the blueprint on disk never references one that is
  // not.
  for (const FSoftObjectPath &Path : Run->Textures)
  {
    UTexture2D *Texture = Cast<UTexture2D>(Path.ResolveObject());
    FString TextureError;
    if (Texture && Texture->GetOutermost()->IsDirty() &&
        !UIWTRunImages::SaveAsset(Texture, TextureError))
    {
      UKismetSystemLibrary::RaiseScriptError(TextureError);
      return false;
    }
  }
  FText Error;
  if (!UIWTGenerated::SaveWidgetBlueprint(WidgetBlueprint, Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error.ToString());
    return false;
  }
  return true;
}

FString UUIWTToolset::ListWidgetProperties(UObject *Object)
{
  const FUIWTActiveRun *Run = RequireActiveRun();
  if (!Run || !ObjectBelongsToRun(Object, *Run))
  {
    return FString();
  }
  return UToolsetLibrary::ListStructProperties(Object->GetClass(), true);
}

FString UUIWTToolset::GetWidgetProperties(UObject *Object,
                                          const TArray<FName> &PropertyNames)
{
  const FUIWTActiveRun *Run = RequireActiveRun();
  if (!Run || !ObjectBelongsToRun(Object, *Run))
  {
    return FString();
  }
  return UToolsetLibrary::GetObjectProperties(Object, PropertyNames);
}

bool UUIWTToolset::SetWidgetProperties(UObject *Object,
                                       const FString &PropertiesJson)
{
  const FUIWTActiveRun *Run = RequireActiveRun();
  if (!Run || !ObjectBelongsToRun(Object, *Run))
  {
    return false;
  }
  Object->Modify();
  if (UWidgetBlueprint *Blueprint = ResolveRunBlueprint(*Run))
  {
    Blueprint->Modify();
    // Sets Status to dirty, so a render or save compiles the change instead
    // of using the class compiled before it.
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
  }
  return UToolsetLibrary::SetObjectProperties(Object, PropertiesJson);
}

FString UUIWTToolset::ApplyWidgetSpec(UWidgetBlueprint *WidgetBlueprint,
                                      const FString &SpecJson)
{
  const FUIWTActiveRun *Run = RequireActiveRun();
  if (!Run || !IsRunBlueprint(WidgetBlueprint, *Run))
  {
    return FString();
  }
  FString Report;
  TArray<FString> Errors;
  if (!UIWTWidgetSpec::Apply(WidgetBlueprint, SpecJson, Report, Errors))
  {
    UKismetSystemLibrary::RaiseScriptError(
        TEXT("The spec was rejected and nothing changed. Fix every problem "
             "and apply the whole spec again:\n- ") +
        FString::Join(Errors, TEXT("\n- ")));
    return FString();
  }
  WidgetBlueprint->MarkPackageDirty();
  // Property failures already raised their own messages, which turn this
  // call into an error; add the summary so the result says what was built.
  if (!Errors.IsEmpty() || WidgetBlueprint->Status == BS_Error)
  {
    FString Message = Report;
    if (!Errors.IsEmpty())
    {
      Message += TEXT("\nProblems (the rest of the spec was applied; fix "
                      "these and apply the whole spec again):\n- ") +
                 FString::Join(Errors, TEXT("\n- "));
    }
    UKismetSystemLibrary::RaiseScriptError(Message);
  }
  return Report;
}

FString UUIWTToolset::ExportWidgetSpec(UWidgetBlueprint *WidgetBlueprint)
{
  const FUIWTActiveRun *Run = RequireActiveRun();
  if (!Run || !IsRunBlueprint(WidgetBlueprint, *Run))
  {
    return FString();
  }
  FString Json;
  FString Error;
  if (!UIWTWidgetSpec::Export(WidgetBlueprint, Json, Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
    return FString();
  }
  return Json;
}

FString UUIWTToolset::ZoomImage(const FString &Source, int32 X, int32 Y,
                                int32 Width, int32 Height, int32 Scale)
{
  FUIWTActiveRun *Run = RequireActiveRunMutable();
  if (!Run)
  {
    return FString();
  }
  const bool bReference = Source.Equals(TEXT("Reference"), ESearchCase::IgnoreCase);
  const bool bRender = Source.Equals(TEXT("Render"), ESearchCase::IgnoreCase);
  const bool bBoth = Source.Equals(TEXT("Both"), ESearchCase::IgnoreCase);
  if (!bReference && !bRender && !bBoth)
  {
    UKismetSystemLibrary::RaiseScriptError(
        TEXT("Source must be \"Reference\", \"Render\" or \"Both\"."));
    return FString();
  }
  if ((bRender || bBoth) && Run->LastRenderPath.IsEmpty())
  {
    UKismetSystemLibrary::RaiseScriptError(
        TEXT("Nothing has been rendered in this run; call "
             "RenderWidgetBlueprint first."));
    return FString();
  }

  const FIntRect Rect(X, Y, X + Width, Y + Height);
  FString Error;
  FImage Zoomed;
  double UsedScale = 1.0;
  auto ZoomFile = [&](const FString &InPath, FImage &OutImage)
  {
    FImage Image;
    if (!UIWTRunImages::LoadImageFile(InPath, Image, Error))
    {
      return false;
    }
    return UIWTRunImages::Zoom(Image, Rect, Scale, OutImage, UsedScale, Error);
  };

  if (bBoth)
  {
    // Half the width each, so the pair still fits what Claude looks at.
    FImage Left;
    FImage Right;
    const int32 PairScale = FMath::Max(
        1, FMath::Min(Scale, (UIWTRunImages::MaxViewEdge / 2) /
                                 FMath::Max(1, Width)));
    Scale = PairScale;
    if (!ZoomFile(Run->ReferenceImagePath, Left) ||
        !ZoomFile(Run->LastRenderPath, Right))
    {
      UKismetSystemLibrary::RaiseScriptError(Error);
      return FString();
    }
    UIWTRunImages::SideBySide(Left, Right, Zoomed);
  }
  else if (!ZoomFile(bReference ? Run->ReferenceImagePath : Run->LastRenderPath,
                     Zoomed))
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
    return FString();
  }

  const FString OutPath = NextRunFile(*Run, TEXT("zoom"));
  if (!UIWTRunImages::SavePng(Zoomed, OutPath, Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
    return FString();
  }
  return FString::Printf(
      TEXT("Wrote %s (%d x %d) at scale %.3g%s. Open it with the Read tool."),
      *OutPath, Zoomed.SizeX, Zoomed.SizeY, UsedScale,
      bBoth ? TEXT("; reference left, render right") : TEXT(""));
}

FString UUIWTToolset::CreateTextureFromReference(
    const FString &TextureName, int32 X, int32 Y, int32 Width, int32 Height,
    const FString &Mode, const FString &Shape, int32 Threshold,
    int32 OutputWidth, int32 OutputHeight)
{
  FUIWTActiveRun *Run = RequireActiveRunMutable();
  if (!Run)
  {
    return FString();
  }
  UIWTRunImages::ECropMode CropMode;
  UIWTRunImages::ECropShape CropShape;
  if (!UIWTRunImages::ParseCropMode(Mode, CropMode))
  {
    UKismetSystemLibrary::RaiseScriptError(
        TEXT("Mode must be \"Copy\", \"KeyDark\" or \"Mask\"."));
    return FString();
  }
  if (!UIWTRunImages::ParseCropShape(Shape, CropShape))
  {
    UKismetSystemLibrary::RaiseScriptError(
        TEXT("Shape must be \"Rect\" or \"Circle\"."));
    return FString();
  }
  FImage Reference;
  if (!LoadReference(*Run, Reference))
  {
    return FString();
  }

  FString Error;
  FImage Pixels;
  bool bCreated = false;
  UTexture2D *Texture = nullptr;
  if (UIWTRunImages::CropForTexture(
          Reference, FIntRect(X, Y, X + Width, Y + Height), CropMode,
          CropShape, Threshold > 0 ? Threshold : 12,
          FIntPoint(OutputWidth, OutputHeight), Pixels, Error))
  {
    Texture = UIWTRunImages::WriteTexture(GetTextureFolder(*Run), TextureName,
                                          Pixels, bCreated, Error);
  }
  if (!Texture)
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
    return FString();
  }
  return RecordTexture(*Run, Texture);
}

FString UUIWTToolset::ImportTextureFile(const FString &FilePath,
                                        const FString &TextureName)
{
  FUIWTActiveRun *Run = RequireActiveRunMutable();
  if (!Run)
  {
    return FString();
  }
  if (!IsInsideDirectory(FilePath, Run->RunDirectory))
  {
    UKismetSystemLibrary::RaiseScriptError(FString::Printf(
        TEXT("%s is not inside the run folder %s."), *FilePath,
        *Run->RunDirectory));
    return FString();
  }
  FString Error;
  FImage Pixels;
  bool bCreated = false;
  UTexture2D *Texture = nullptr;
  if (UIWTRunImages::LoadImageFile(FilePath, Pixels, Error))
  {
    Texture = UIWTRunImages::WriteTexture(GetTextureFolder(*Run), TextureName,
                                          Pixels, bCreated, Error);
  }
  if (!Texture)
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
    return FString();
  }
  return RecordTexture(*Run, Texture);
}

FString UUIWTToolset::RenderWidgetBlueprint(UWidgetBlueprint *WidgetBlueprint,
                                            int32 Width, int32 Height)
{
  FUIWTActiveRun *Run = RequireActiveRunMutable();
  if (!Run || !IsRunBlueprint(WidgetBlueprint, *Run))
  {
    return FString();
  }
  FImage Reference;
  FString Error;
  const bool bHasReference =
      !Run->ReferenceImagePath.IsEmpty() &&
      UIWTRunImages::LoadImageFile(Run->ReferenceImagePath, Reference, Error);
  const FIntPoint Size(
      Width > 0 ? Width : (bHasReference ? Reference.SizeX : 1920),
      Height > 0 ? Height : (bHasReference ? Reference.SizeY : 1080));

  FImage Rendered;
  if (!UIWTRunImages::RenderWidget(WidgetBlueprint, Size, Rendered, Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
    return FString();
  }
  const FString RenderPath = NextRunFile(*Run, TEXT("render"));
  if (!UIWTRunImages::SavePng(Rendered, RenderPath, Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
    return FString();
  }
  Run->LastRenderPath = RenderPath;
  FString Report = FString::Printf(TEXT("Rendered %d x %d to %s."), Size.X,
                                   Size.Y, *RenderPath);
  if (!bHasReference)
  {
    return Report + TEXT(" There is no reference image to compare with.");
  }

  FImage Pair;
  UIWTRunImages::SideBySide(Reference, Rendered, Pair);
  const FString ComparePath = NextRunFile(*Run, TEXT("compare"));
  if (!UIWTRunImages::SavePng(Pair, ComparePath, Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
    return FString();
  }
  Report += FString::Printf(
      TEXT(" Side by side (reference left, render right): %s. Mean colour "
           "difference: %.1f%%."),
      *ComparePath, UIWTRunImages::MeanDifference(Reference, Rendered));
  if (Size != FIntPoint(Reference.SizeX, Reference.SizeY))
  {
    Report += TEXT(" The render is not the reference's size, so the render "
                   "was scaled for the comparison and ZoomImage \"Both\" "
                   "coordinates do not line up.");
  }
  return Report;
}

UUIWTAgentSkill::UUIWTAgentSkill()
{
  Description =
      TEXT("Edit the Widget Blueprint selected in the UI Widget Tool "
           "manager window, as the user asked in its chat panel.");
  Instructions = GetInstructionsText();
}

FString UUIWTAgentSkill::GetInstructionsText()
{
  return TEXT(
      "You are editing an Unreal Engine Widget Blueprint through the Unreal "
      "MCP server. Follow this procedure exactly. Tools are reached through "
      "the server's list_toolsets / describe_toolset / call_tool meta-tools; "
      "the toolset names below are suffixes of the registered names (for "
      "example UMGToolSet.UMGToolSet and UIWidgetToolPluginEditor."
      "UIWTToolset).\n"
      "\n"
      "1. Call UIWTToolset.GetContext first. It returns BlueprintPath (the "
      "blueprint you may edit), PickedWidget (the widget the user selected, "
      "if any), EntryId, TextureFolder (the blueprint's own content "
      "folder, where textures go), RunFolder (the same folder on disk, "
      "where the reference image, renders and zooms go) and ReferenceImage "
      "with its size (the image the user attached, now or earlier; empty "
      "when none).\n"
      "2. Read the tree with UIWTToolset.ExportWidgetSpec on BlueprintPath: "
      "every widget with its class, name and non-default properties. {} "
      "means a new, empty blueprint.\n"
      "3. Choose how to edit:\n"
      "   a. A small change to a few widgets or properties: use UMGToolSet "
      "tools (AddWidget, MoveWidget, RemoveWidget, RenameWidget, "
      "WrapWidgets, ...; call UMGToolSet.GetWidgetDescription for the "
      "widget references they take) and UIWTToolset.ListWidgetProperties / "
      "GetWidgetProperties / SetWidgetProperties, then "
      "UMGToolSet.CompileWidgetBlueprint. UMGToolSet's own text mentions "
      "ObjectTools.list_properties / get_properties / set_properties - that "
      "toolset is NOT available here; the UIWTToolset equivalents are the "
      "replacement and take the same widget or slot references.\n"
      "   b. Building a screen, recreating an image, or restructuring: "
      "write the complete tree as a spec and call UIWTToolset.ApplyWidgetSpec "
      "once. When the tree is not empty, start from the exported spec and "
      "keep every widget you keep with the same name and class; that keeps "
      "graph references, bindings and animations working. ApplyWidgetSpec "
      "rejects an invalid spec without changing anything and lists every "
      "problem; it compiles after building and reports the result. Fix the "
      "spec and apply the whole spec again until it builds and compiles "
      "cleanly; reapplying is always safe.\n"
      "4. When the request includes an image, or asks to match the "
      "reference image, follow \"Recreating an image\" below.\n"
      "5. The compile must be clean. If errors cannot be fixed, do NOT save: "
      "report the errors and stop; the editor restores the blueprint from "
      "disk.\n"
      "6. After a clean compile, call UIWTToolset.SaveWidgetBlueprint on "
      "BlueprintPath.\n"
      "7. Reply with a short summary of what changed.\n"
      "\n"
      "Recreating an image:\n"
      "- Work in reference-image pixels (ReferenceWidth x ReferenceHeight); "
      "the prompt says how they relate to the attached image. First list "
      "every region you see with its box (x, y, width, height): "
      "backgrounds, frames and panels, buttons and tabs, icons and "
      "pictures, text, separators and connector lines, and states such as "
      "selected, hovered or disabled. Use UIWTToolset.ZoomImage (Source "
      "\"Reference\") and the Read tool on the PNG it writes to measure "
      "small or crowded regions and to read exact colours. Then write the "
      "spec from that list. Leave parts that show the game world, not UI, "
      "transparent.\n"
      "- Root: a CanvasPanel that fills the screen. For an image of a panel "
      "or dialog, put the design in a CanvasPanel of the image's size, "
      "centred: slot anchors minimum and maximum (0.5, 0.5), alignment "
      "(0.5, 0.5), offsets left 0, top 0, right <width>, bottom <height>. "
      "For a full-screen image, anchor each element to the screen edge or "
      "centre it belongs to.\n"
      "- Pick layout per region. Free-form art (skill trees, maps, HUD "
      "pieces): absolute positions in a CanvasPanel from the measured boxes, "
      "with ZOrder for layering (backgrounds lowest, connector lines below "
      "the icons they join, labels on top). Lists, forms and menus: "
      "VerticalBox / HorizontalBox rows, SizeBox for fixed heights and "
      "widths, slot Padding for the measured gaps, ScrollBox for lists that "
      "scroll.\n"
      "- Use real controls, not pictures of them: Button (style normal / "
      "hovered / pressed), CheckBox, Slider, ComboBoxString, EditableText, "
      "ScrollBox, ProgressBar. Show a state from the image (selected, "
      "hovered, disabled) by styling that widget. Give every widget a "
      "descriptive name and set \"variable\": true on interactive ones.\n"
      "- Draw flat shapes with brushes: RoundedBox with FixedRadius corners "
      "and an outline for panels, plates and pills; roundingType "
      "HalfHeightRadius for circles; nested Borders (dark outer ring, thin "
      "light rim, inner fill) for bevels and rings; thin Images for lines. "
      "Brushes scale cleanly and keep states editable, so prefer them "
      "wherever they can match the image.\n"
      "- For art brushes cannot draw (illustrated icons, portraits, item "
      "art, ornaments, textured backgrounds and frames), cut it out of the "
      "reference with UIWTToolset.CreateTextureFromReference before writing "
      "the spec, and use the returned path as the brush's resourceObject "
      "with drawAs \"Image\" (or \"Box\" with a margin for a frame that "
      "stretches). Crop tightly, without text or anything that will be its "
      "own widget: a button's icon, not the whole button. KeyDark or Mask "
      "cut art out of a dark background; Circle cuts round art; a texture "
      "under a RoundedBox brush takes its corner radius and outline. Do not "
      "cut out whole screens or panels to fake a layout: text must stay "
      "TextBlocks and controls must stay real controls. Use a project "
      "texture only when the user names it.\n"
      "- Colours: UMG stores linear colours. Convert each sRGB channel s "
      "(0-255) you read from the image: c = s / 255; linear = c <= 0.04045 "
      "? c / 12.92 : ((c + 0.055) / 1.055) ^ 2.4. Alpha is unchanged.\n"
      "- Text: Roboto (/Engine/EngineFonts/Roboto.Roboto) with typeface "
      "Regular, Bold, Italic, Bold Italic or Light. A font size about equal "
      "to the capital-letter height in pixels matches the image; adjust "
      "letterSpacing (thousandths of an em) to match the text's width. Copy "
      "every text exactly.\n"
      "- Property JSON (names are exact; unknown names are rejected before "
      "anything changes):\n"
      "  CanvasPanelSlot: {\"LayoutData\": {\"offsets\": {\"left\", \"top\", "
      "\"right\", \"bottom\"}, \"anchors\": {\"minimum\": {\"x\", \"y\"}, "
      "\"maximum\": {\"x\", \"y\"}}, \"alignment\": {\"x\", \"y\"}}, "
      "\"ZOrder\": n, \"bAutoSize\": bool}. With point anchors, right and "
      "bottom are the width and height; with stretched anchors all four are "
      "margins.\n"
      "  HorizontalBoxSlot / VerticalBoxSlot: {\"Size\": {\"sizeRule\": "
      "\"Automatic\" | \"Fill\", \"value\": 1}, \"Padding\": {\"left\", "
      "\"top\", \"right\", \"bottom\"}, \"HorizontalAlignment\": "
      "\"HAlign_Fill\" | \"HAlign_Left\" | \"HAlign_Center\" | "
      "\"HAlign_Right\", \"VerticalAlignment\": \"VAlign_...\"}. "
      "OverlaySlot, BorderSlot, ButtonSlot and SizeBoxSlot take Padding and "
      "the two alignments.\n"
      "  SlateColor: {\"specifiedColor\": {\"r\", \"g\", \"b\", \"a\"}, "
      "\"colorUseRule\": \"UseColor_Specified\"}; LinearColor: {\"r\", "
      "\"g\", \"b\", \"a\"}.\n"
      "  Brush (Image.Brush, Border.Background, Button.WidgetStyle.normal / "
      "hovered / pressed / disabled): {\"drawAs\": \"RoundedBox\" | "
      "\"Image\" | \"Box\" | \"NoDrawType\", \"tintColor\": SlateColor, "
      "\"imageSize\": {\"x\", \"y\"}, \"resourceObject\": \"None\", "
      "\"outlineSettings\": {\"cornerRadii\": {\"x\", \"y\", \"z\", \"w\"}, "
      "\"color\": SlateColor, \"width\": n, \"roundingType\": "
      "\"FixedRadius\" | \"HalfHeightRadius\"}, \"margin\": {\"left\", "
      "\"top\", \"right\", \"bottom\"} (0-1, for drawAs Box)}; "
      "resourceObject is \"None\" or a texture path from the texture "
      "tools, and a textured brush's tintColor is usually white. "
      "Button.WidgetStyle also "
      "takes normalPadding and pressedPadding (the content inset).\n"
      "  TextBlock: {\"Text\", \"Font\": {\"fontObject\": {\"refPath\": "
      "\"/Engine/EngineFonts/Roboto.Roboto\"}, \"typefaceFontName\", "
      "\"size\", \"letterSpacing\", \"outlineSettings\": {\"outlineSize\", "
      "\"outlineColor\": LinearColor}}, \"ColorAndOpacity\": SlateColor, "
      "\"Justification\": \"Left\" | \"Center\" | \"Right\", "
      "\"ShadowOffset\": {\"x\", \"y\"}, \"ShadowColorAndOpacity\": "
      "LinearColor, \"AutoWrapText\": bool}.\n"
      "  SizeBox: {\"WidthOverride\", \"HeightOverride\", "
      "\"bOverride_WidthOverride\": true, \"bOverride_HeightOverride\": "
      "true}. Border: {\"Background\": Brush, \"Padding\", "
      "\"HorizontalAlignment\", \"VerticalAlignment\"} (the Border's own "
      "alignment places its child).\n"
      "  For other classes or properties, call ListWidgetProperties on a "
      "widget of that class first (add one with a small spec if needed).\n"
      "- Before applying, check the spec against your region list: every "
      "region has a widget, positions and sizes match the measured boxes, "
      "and all texts match the image.\n"
      "- After it applies and compiles, check the result: call "
      "UIWTToolset.RenderWidgetBlueprint (size 0 x 0 renders at the "
      "reference size), open the side-by-side PNG with the Read tool, and "
      "ZoomImage with Source \"Both\" on regions that look off. List what "
      "differs (positions, sizes, colours, missing or extra pieces, text "
      "size and weight), fix those in the spec, apply again and render "
      "again. Stop after three rounds, or earlier when only small "
      "differences are left; say in your reply what still differs.\n"
      "\n"
      "UIWTDevToolset works on any asset inside TextureFolder (the "
      "blueprint, its widgets and slots, its textures): "
      "ListObjectProperties / GetObjectProperties / SetObjectProperties for "
      "properties the other tools do not cover, such as a texture's Filter "
      "or tiling; ImportTexture, which brings an image file from anywhere "
      "into TextureFolder as a UI texture; SaveAsset; and RenderWidgetToPng, "
      "which renders the blueprint at any size to a PNG in RunFolder. "
      "Textures it imports or changes are saved and restored with the "
      "blueprint, like the UIWTToolset ones.\n"
      "\n"
      "Rules: edit only the blueprint GetContext returned; never create, "
      "duplicate or delete assets, except textures made with "
      "CreateTextureFromReference, ImportTextureFile or "
      "UIWTDevToolset.ImportTexture; never call "
      "AgentSkillToolset.CreateSkill or any editor tool outside UMGToolSet, "
      "UIWTToolset and UIWTDevToolset; never call "
      "UndoTransaction (it is a no-op here). Layout (anchors, padding, "
      "alignment, size) lives on a widget's Slot, not the widget. Never "
      "remove or rename a widget that the native parent class binds "
      "(BindWidget); the compile will fail. If PickedWidget is set, the "
      "request is about that widget unless the prompt says otherwise.");
}

FString UUIWTAgentSkill::GetAccessText(EUIWTPermissionMode InMode,
                                      const FString &InRunFolder)
{
  if (InMode == EUIWTPermissionMode::DontAsk)
  {
    return TEXT("\n\n(Tool access for this turn: only the editor tools and "
                "reading files in the project, such as the renders and "
                "zooms in the run folder, are allowed; other built-in tools "
                "are refused, so do all image work with the editor tools.)");
  }
  const TCHAR *Checks =
      InMode == EUIWTPermissionMode::BypassPermissions
          ? TEXT("No permission checks run, so these rules are all that "
                 "keep the project safe.")
          : TEXT("A safety check reviews each action and refuses risky "
                 "ones; when it refuses one, do not try to get around it.");
  return FString::Printf(
      TEXT("\n\n(Tool access for this turn: besides the editor tools you "
           "have every built-in tool: shell, files and web. %s Use them for "
           "image work the editor tools do not cover: scripts that measure, "
           "crop, key or generate images (gradients, tiling textures, "
           "composited art), written and run in the run folder %s, with the "
           "results imported through UIWTToolset.ImportTextureFile. The run "
           "folder is also the widget's content folder: create and change "
           "loose files there, never its .uasset files. Never edit, move or "
           "delete any other file, including .uasset, .umap, source, config "
           "and git files; never build, launch or stop programs such as the "
           "editor; never install anything. Every change to the project goes "
           "through the editor tools.)"),
      Checks, *InRunFolder);
}
