#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Core/UIWTDesignImport.h"
#include "Core/UIWTGeneratedBlueprints.h"
#include "Core/UIWTRunImages.h"
#include "Core/UIWTWidgetSpec.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

// The image-import spike (import-image.md, section 0): imports the design
// trees a perfect image reader would write for three mockups
// (Plugins/UIWidgetToolPlugin/Tests/ImageSpike, made by
// Tools/ImageSpike/make-mockups.ps1), without and with layout blocks, and
// measures what the tree route gives: widgets, canvases against boxes, the
// tree's size against the spec's (what Claude writes in each route), and
// the render's difference from the mockup. It records; it only fails when an
// import fails. Results: Saved/UIWidgetTool/ImageSpike/results.md.
namespace
{
  constexpr EAutomationTestFlags TestFlags =
      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

  int32 CountNodes(const TSharedPtr<FJsonObject> &InNode)
  {
    int32 Count = 1;
    const TArray<TSharedPtr<FJsonValue>> *Children = nullptr;
    if (InNode.IsValid() && InNode->TryGetArrayField(TEXT("children"), Children))
    {
      for (const TSharedPtr<FJsonValue> &Child : *Children)
      {
        Count += CountNodes(Child->AsObject());
      }
    }
    return Count;
  }

  FString Minified(const TSharedRef<FJsonObject> &InObject)
  {
    FString Json;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
    FJsonSerializer::Serialize(InObject, Writer);
    return Json;
  }

  class FUIWTImageSpikeTestBase : public FAutomationTestBase
  {
  public:
    FUIWTImageSpikeTestBase(const FString &InName, const bool bInComplexTask)
        : FAutomationTestBase(InName, bInComplexTask)
    {
    }
    virtual bool SuppressLogErrors() override { return true; }
    virtual bool SuppressLogWarnings() override { return true; }
  };
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUIWTImageSpikeTest, FUIWTImageSpikeTestBase,
                                        "UIWidgetTool.ImageSpike.Measure", TestFlags)

bool FUIWTImageSpikeTest::RunTest(const FString &Parameters)
{
  const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UIWidgetToolPlugin"));
  if (!TestTrue(TEXT("plugin"), Plugin.IsValid()))
  {
    return false;
  }
  const FString Fixtures =
      FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Tests/ImageSpike"));
  const FString OutDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() /
                                                           TEXT("UIWidgetTool/ImageSpike"));
  IFileManager::Get().MakeDirectory(*OutDir, true);
  const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
  const FString TargetFolder = TEXT("/Temp/UIWTImageSpike_") + Id;
  const bool bRender = FApp::CanEverRender();

  FString Table =
      TEXT("| Mockup | Variant | Nodes | Widgets | Canvases | H/V boxes | Tree (bytes, ~tokens) | "
           "Spec (bytes, ~tokens) | Tree / spec | Render difference |\n"
           "|---|---|---|---|---|---|---|---|---|---|\n");
  for (const TCHAR *Mockup : {TEXT("dialog"), TEXT("shop"), TEXT("hud")})
  {
    FImage Reference;
    FString Error;
    const bool bReference = UIWTRunImages::LoadImageFile(
        Fixtures / Mockup / TEXT("reference.png"), Reference, Error);
    TestTrue(FString::Printf(TEXT("%s reference"), Mockup), bReference);

    for (const TCHAR *Variant : {TEXT("flat"), TEXT("layout")})
    {
      const FString What = FString::Printf(TEXT("%s/%s"), Mockup, Variant);
      const FString DesignFile =
          Fixtures / Mockup / FString::Printf(TEXT("design.%s.json"), Variant);
      FString Text;
      TSharedPtr<FJsonObject> Tree;
      if (!FFileHelper::LoadFileToString(Text, *DesignFile) ||
          !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Tree) ||
          !Tree.IsValid())
      {
        AddError(What + TEXT(": can't read ") + DesignFile);
        continue;
      }
      // What Claude writes in the tree route.
      const int32 TreeBytes = Minified(Tree.ToSharedRef()).Len();
      const int32 Nodes = CountNodes(Tree->GetObjectField(TEXT("root")));

      UIWTDesignImport::FRequest Request;
      Request.DesignFile = DesignFile;
      Request.TargetFolder = TargetFolder;
      Request.BlueprintName = FString::Printf(TEXT("WBP_Spike_%s_%s"), Mockup, Variant);
      Request.ComponentsFolder = TargetFolder / TEXT("Components");
      Request.bSave = false;
      UIWTDesignImport::FResult Result;
      if (!UIWTDesignImport::Import(Request, Result, Error) || !Result.Blueprint)
      {
        AddError(What + TEXT(": import failed: ") + Error);
        continue;
      }
      for (const FString &ApplyError : Result.ApplyErrors)
      {
        AddWarning(What + TEXT(": Apply: ") + ApplyError);
      }
      UWidgetBlueprint *Blueprint = Result.Blueprint;

      TMap<FString, int32> ByClass;
      int32 Widgets = 0;
      Blueprint->WidgetTree->ForEachWidget(
          [&](UWidget *InWidget)
          {
            ++Widgets;
            ++ByClass.FindOrAdd(InWidget->GetClass()->GetName());
          });
      const int32 Canvases = ByClass.FindRef(TEXT("CanvasPanel"));
      const int32 Boxes =
          ByClass.FindRef(TEXT("HorizontalBox")) + ByClass.FindRef(TEXT("VerticalBox"));

      // What Claude writes in today's route: the whole spec.
      FString Spec;
      UIWTWidgetSpec::Export(Blueprint, Spec, Error);
      const int32 SpecBytes = Spec.Len();

      FString Difference = TEXT("not rendered (-nullrhi)");
      if (bRender && bReference)
      {
        FImage Rendered;
        if (UIWTRunImages::RenderWidget(Blueprint, FIntPoint(Reference.SizeX, Reference.SizeY),
                                        Rendered, Error))
        {
          Difference = FString::Printf(TEXT("%.2f%%"),
                                       UIWTRunImages::MeanDifference(Reference, Rendered));
          FImage Pair;
          UIWTRunImages::SideBySide(Reference, Rendered, Pair);
          UIWTRunImages::SavePng(Pair,
                                 OutDir / FString::Printf(TEXT("%s_%s_compare.png"), Mockup,
                                                          Variant),
                                 Error);
        }
        else
        {
          Difference = TEXT("render failed: ") + Error;
        }
      }

      TArray<FString> Classes;
      ByClass.KeySort(TLess<FString>());
      for (const TPair<FString, int32> &Pair : ByClass)
      {
        Classes.Add(FString::Printf(TEXT("%s %d"), *Pair.Key, Pair.Value));
      }
      AddInfo(FString::Printf(TEXT("%s: %s; report %d entries"), *What,
                              *FString::Join(Classes, TEXT(", ")), Result.Report.Num()));
      Table += FString::Printf(
          TEXT("| %s | %s | %d | %d | %d | %d | %d (~%d) | %d (~%d) | %.2f | %s |\n"), Mockup,
          Variant, Nodes, Widgets, Canvases, Boxes, TreeBytes, TreeBytes / 3, SpecBytes,
          SpecBytes / 3, SpecBytes > 0 ? double(TreeBytes) / SpecBytes : 0.0, *Difference);
    }
  }

  const FString Report =
      TEXT("# Image-import spike results\n\n") +
      FString::Printf(TEXT("Run %s. Trees from Tools/ImageSpike/make-mockups.ps1 (exact "
                           "geometry: an upper bound for a reading run). ~tokens = bytes / 3. "
                           "Compare images are next to this file.\n\n"),
                      *FDateTime::Now().ToString()) +
      Table;
  FFileHelper::SaveStringToFile(Report, *(OutDir / TEXT("results.md")),
                                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
  AddInfo(Report);

  const FString OnDisk = UIWTGenerated::GetFolderOnDisk(TargetFolder);
  if (!OnDisk.IsEmpty())
  {
    IFileManager::Get().DeleteDirectory(*OnDisk, false, true);
  }
  return true;
}

#endif
