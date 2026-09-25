#include "UIWTDevToolset.h"

#include "Core/UIWTGeneratedBlueprints.h"
#include "Core/UIWTRunImages.h"
#include "Core/UIWTWidgetSpec.h"
#include "Engine/Texture2D.h"
#include "HAL/IConsoleManager.h"
#include "IModelContextProtocolModule.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Paths.h"
#include "ToolsetRegistry/ToolsetLibrary.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UIWTChatTypes.h"
#include "UIWTClaudeService.h"
#include "UIWTPromptImage.h"
#include "WidgetBlueprint.h"

namespace
{
  bool IsInFolder(const FString &InPackageName, const FString &InFolder)
  {
    return !InFolder.IsEmpty() &&
           InPackageName.StartsWith(InFolder + TEXT("/"),
                                    ESearchCase::IgnoreCase);
  }

  // The active run; null with the script error raised when there is none.
  FUIWTActiveRun *RequireRun()
  {
    FUIWTActiveRun *Run = FUIWTClaudeService::Get().GetActiveRunMutable();
    if (!Run || !Run->IsValid())
    {
      UKismetSystemLibrary::RaiseScriptError(
          TEXT("No UI Widget Tool run is active. Tools only work while the "
               "manager window has a run in flight."));
      return nullptr;
    }
    return Run;
  }

  // The active run, when InObject belongs to an asset in its folder.
  FUIWTActiveRun *RequireInRunFolder(const UObject *InObject)
  {
    FUIWTActiveRun *Run = RequireRun();
    if (!Run)
    {
      return nullptr;
    }
    if (!InObject)
    {
      UKismetSystemLibrary::RaiseScriptError(TEXT("Object is null."));
      return nullptr;
    }
    if (!IsInFolder(InObject->GetOutermost()->GetName(), Run->ContentFolder))
    {
      UKismetSystemLibrary::RaiseScriptError(FString::Printf(
          TEXT("%s is outside the run's folder %s."), *InObject->GetPathName(),
          *Run->ContentFolder));
      return nullptr;
    }
    return Run;
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

  void RefreshMcp()
  {
    if (IModelContextProtocolModule *Mcp = IModelContextProtocolModule::Get())
    {
      Mcp->RefreshTools();
    }
  }

  FAutoConsoleCommand RegisterDevToolsCommand(
      TEXT("UIWT.RegisterDevTools"),
      TEXT("Registers the UI Widget Tool test hooks (UIWTTestHooksToolset) "
           "with the toolset registry, for testing the plugin over MCP."),
      FConsoleCommandDelegate::CreateLambda(
          []()
          {
            UToolsetRegistry::RegisterToolsetClass(
                UUIWTTestHooksToolset::StaticClass());
            RefreshMcp();
          }));

  FAutoConsoleCommand UnregisterDevToolsCommand(
      TEXT("UIWT.UnregisterDevTools"),
      TEXT("Unregisters the UI Widget Tool test hooks."),
      FConsoleCommandDelegate::CreateLambda(
          []()
          {
            UToolsetRegistry::UnregisterToolsetClass(
                UUIWTTestHooksToolset::StaticClass());
            RefreshMcp();
          }));

  // The spec test hooks act on hand-made test blueprints only.
  const TCHAR *const TestHookRoot = TEXT("/Game/UI");

  bool RequireTestHookBlueprint(const UWidgetBlueprint *InBlueprint)
  {
    if (!InBlueprint)
    {
      UKismetSystemLibrary::RaiseScriptError(TEXT("Blueprint is null."));
      return false;
    }
    if (!IsInFolder(InBlueprint->GetOutermost()->GetName(), TestHookRoot))
    {
      UKismetSystemLibrary::RaiseScriptError(FString::Printf(
          TEXT("%s is outside %s."), *InBlueprint->GetPathName(), TestHookRoot));
      return false;
    }
    return true;
  }
}

// ---------------------------------------------------------------------------
// UUIWTDevToolset

FString UUIWTDevToolset::ListObjectProperties(UObject *Object)
{
  if (!RequireInRunFolder(Object))
  {
    return FString();
  }
  return UToolsetLibrary::ListStructProperties(Object->GetClass(), true);
}

FString UUIWTDevToolset::GetObjectProperties(UObject *Object,
                                             const TArray<FName> &PropertyNames)
{
  if (!RequireInRunFolder(Object))
  {
    return FString();
  }
  return UToolsetLibrary::GetObjectProperties(Object, PropertyNames);
}

bool UUIWTDevToolset::SetObjectProperties(UObject *Object,
                                          const FString &PropertiesJson)
{
  FUIWTActiveRun *Run = RequireInRunFolder(Object);
  if (!Run)
  {
    return false;
  }
  Object->Modify();
  Object->PreEditChange(nullptr);
  const bool bSet = UToolsetLibrary::SetObjectProperties(Object, PropertiesJson);
  // What the Details panel does: textures rebuild with their new settings,
  // widgets and slots push the values to Slate.
  Object->PostEditChange();
  Object->MarkPackageDirty();

  if (UWidgetBlueprint *Blueprint = Object->IsA<UWidgetBlueprint>()
                                        ? Cast<UWidgetBlueprint>(Object)
                                        : Object->GetTypedOuter<UWidgetBlueprint>())
  {
    // A render or save then compiles the change instead of using the class
    // compiled before it.
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
  }
  if (UTexture2D *Texture = Object->IsA<UTexture2D>()
                                ? Cast<UTexture2D>(Object)
                                : Object->GetTypedOuter<UTexture2D>())
  {
    // Saved with the blueprint, and restored from disk if the run fails.
    Run->Textures.AddUnique(FSoftObjectPath(Texture));
  }
  return bSet;
}

FString UUIWTDevToolset::ImportTexture(const FString &SourceFile,
                                       const FString &DestinationPath,
                                       const FString &AssetName)
{
  FUIWTActiveRun *Run = RequireRun();
  if (!Run)
  {
    return FString();
  }
  FString Folder = DestinationPath;
  Folder.RemoveFromEnd(TEXT("/"));
  if (!Folder.Equals(Run->ContentFolder, ESearchCase::IgnoreCase) &&
      !IsInFolder(Folder, Run->ContentFolder))
  {
    UKismetSystemLibrary::RaiseScriptError(FString::Printf(
        TEXT("DestinationPath must be %s or a folder inside it."),
        *Run->ContentFolder));
    return FString();
  }

  FString Error;
  FImage Pixels;
  bool bCreated = false;
  UTexture2D *Texture = nullptr;
  if (UIWTRunImages::LoadImageFile(SourceFile, Pixels, Error))
  {
    Texture = UIWTRunImages::WriteTexture(Folder, AssetName, Pixels, bCreated,
                                          Error);
  }
  if (!Texture)
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
    return FString();
  }
  const FSoftObjectPath Path(Texture);
  Run->Textures.AddUnique(Path);
  return Path.ToString();
}

bool UUIWTDevToolset::SaveAsset(UObject *Asset)
{
  if (!RequireInRunFolder(Asset))
  {
    return false;
  }
  if (const UBlueprint *Blueprint = Cast<UBlueprint>(Asset))
  {
    if (Blueprint->Status == BS_Error)
    {
      UKismetSystemLibrary::RaiseScriptError(
          TEXT("The blueprint has compile errors; fix them and compile "
               "before saving."));
      return false;
    }
  }
  FString Error;
  if (!UIWTRunImages::SaveAsset(Asset, Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
    return false;
  }
  return true;
}

bool UUIWTDevToolset::RenderWidgetToPng(UWidgetBlueprint *WidgetBlueprint,
                                        int32 Width, int32 Height,
                                        const FString &OutFile)
{
  FUIWTActiveRun *Run = RequireInRunFolder(WidgetBlueprint);
  if (!Run)
  {
    return false;
  }
  if (!IsInsideDirectory(OutFile, Run->RunDirectory))
  {
    UKismetSystemLibrary::RaiseScriptError(FString::Printf(
        TEXT("%s is not inside the run folder %s."), *OutFile,
        *Run->RunDirectory));
    return false;
  }
  FImage Rendered;
  FString Error;
  if (!UIWTRunImages::RenderWidget(WidgetBlueprint, FIntPoint(Width, Height),
                                   Rendered, Error) ||
      !UIWTRunImages::SavePng(Rendered, OutFile, Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// UUIWTTestHooksToolset

FString UUIWTTestHooksToolset::DevApplyWidgetSpec(UWidgetBlueprint *WidgetBlueprint,
                                                  const FString &SpecJson)
{
  if (!RequireTestHookBlueprint(WidgetBlueprint))
  {
    return FString();
  }
  FString Report;
  TArray<FString> Errors;
  if (!UIWTWidgetSpec::Apply(WidgetBlueprint, SpecJson, Report, Errors))
  {
    UKismetSystemLibrary::RaiseScriptError(
        TEXT("REJECTED:\n- ") + FString::Join(Errors, TEXT("\n- ")));
    return FString();
  }
  WidgetBlueprint->MarkPackageDirty();
  if (!Errors.IsEmpty() || WidgetBlueprint->Status == BS_Error)
  {
    UKismetSystemLibrary::RaiseScriptError(
        Report + TEXT("\nProblems:\n- ") + FString::Join(Errors, TEXT("\n- ")));
  }
  return Report;
}

FString UUIWTTestHooksToolset::DevExportWidgetSpec(UWidgetBlueprint *WidgetBlueprint)
{
  if (!RequireTestHookBlueprint(WidgetBlueprint))
  {
    return FString();
  }
  FString Json;
  FString Error;
  if (!UIWTWidgetSpec::Export(WidgetBlueprint, Json, Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error);
  }
  return Json;
}

bool UUIWTTestHooksToolset::DevBeginRun(UWidgetBlueprint *WidgetBlueprint,
                                        const FString &EntryId,
                                        const FString &ImageFile)
{
  FGuid Id;
  if (!FGuid::Parse(EntryId, Id))
  {
    UKismetSystemLibrary::RaiseScriptError(TEXT("EntryId is not a GUID."));
    return false;
  }
  FText Error;
  TSharedPtr<const FUIWTPromptImage> Image;
  if (!ImageFile.IsEmpty())
  {
    Image = UIWTPromptImage::LoadFromFile(ImageFile, Error);
    if (!Image.IsValid())
    {
      UKismetSystemLibrary::RaiseScriptError(Error.ToString());
      return false;
    }
  }
  if (!FUIWTClaudeService::Get().DevBeginRun(Id, WidgetBlueprint, Image, Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error.ToString());
    return false;
  }
  return true;
}

bool UUIWTTestHooksToolset::DevEndRun(bool bSuccess)
{
  FUIWTClaudeService::Get().DevEndRun(bSuccess);
  return true;
}

FString UUIWTTestHooksToolset::DevStartRun(const FString &EntryId,
                                           const FString &Prompt,
                                           const FString &ImageFile)
{
  FGuid Id;
  if (!FGuid::Parse(EntryId, Id))
  {
    return TEXT("EntryId is not a GUID.");
  }
  FText Error;
  TSharedPtr<const FUIWTPromptImage> Image;
  if (!ImageFile.IsEmpty())
  {
    Image = UIWTPromptImage::LoadFromFile(ImageFile, Error);
    if (!Image.IsValid())
    {
      return Error.ToString();
    }
  }
  FUIWTClaudeService &Service = FUIWTClaudeService::Get();
  if (!Service.ConnectMcp(Error))
  {
    return TEXT("ConnectMcp: ") + Error.ToString();
  }
  if (!Service.StartRun(Id, Prompt, Image, FUIWTRunContext(), Error))
  {
    return TEXT("StartRun: ") + Error.ToString();
  }
  return TEXT("started");
}

bool UUIWTTestHooksToolset::DevRenameWidgetBlueprint(
    UWidgetBlueprint *WidgetBlueprint, const FString &NewName)
{
  if (!WidgetBlueprint ||
      (!UIWTGenerated::IsGeneratedPath(WidgetBlueprint->GetPathName()) &&
       !RequireTestHookBlueprint(WidgetBlueprint)))
  {
    return false;
  }
  FText Error;
  if (!UIWTGenerated::RenameWidgetBlueprint(WidgetBlueprint, NewName, Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error.ToString());
    return false;
  }
  return true;
}

FString UUIWTTestHooksToolset::DevDuplicateWidgetBlueprint(
    UWidgetBlueprint *WidgetBlueprint)
{
  FText Error;
  UWidgetBlueprint *Copy =
      UIWTGenerated::DuplicateWidgetBlueprint(WidgetBlueprint, Error);
  if (!Copy)
  {
    UKismetSystemLibrary::RaiseScriptError(Error.ToString());
    return FString();
  }
  return Copy->GetPathName();
}

bool UUIWTTestHooksToolset::DevDeleteWidgetBlueprint(
    UWidgetBlueprint *WidgetBlueprint)
{
  if (!WidgetBlueprint || !WidgetBlueprint->GeneratedClass)
  {
    UKismetSystemLibrary::RaiseScriptError(TEXT("No compiled blueprint."));
    return false;
  }
  FText Error;
  if (!UIWTGenerated::DeleteWidgetBlueprint(
          TSoftClassPtr<UUserWidget>(WidgetBlueprint->GeneratedClass), Error))
  {
    UKismetSystemLibrary::RaiseScriptError(Error.ToString());
    return false;
  }
  return true;
}

FString UUIWTTestHooksToolset::DevRunStatus(const FString &EntryId)
{
  FGuid Id;
  FGuid::Parse(EntryId, Id);
  FUIWTClaudeService &Service = FUIWTClaudeService::Get();
  FString Out = Service.IsRunInFlight() ? TEXT("IN FLIGHT\n") : TEXT("IDLE\n");
  if (const FUIWTChatState *State = Service.FindChatState(Id))
  {
    for (const FUIWTChatMessage &Message : State->Messages)
    {
      Out += FString::Printf(TEXT("[%d] %s\n"), static_cast<int32>(Message.Role),
                             *Message.Text);
    }
  }
  return Out;
}
