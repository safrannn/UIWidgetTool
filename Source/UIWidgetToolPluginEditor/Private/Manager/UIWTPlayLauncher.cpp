#include "UIWTPlayLauncher.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Misc/PackageName.h"
#include "UIWTCheckpointCodec.h"
#include "UIWTCheckpointTypes.h"
#include "UIWTPendingRestore.h"

#define LOCTEXT_NAMESPACE "UIWidgetTool"

namespace
{
  bool GWaitingForPieToEnd = false;

  bool GStartQueued = false;

  FString GDeferredMapPackagePath;

  FDelegateHandle GEndPieHandle;
  FDelegateHandle GCancelPieHandle;

  void StartPlaySession(const FString &InMapPackagePath)
  {
    if (!GEditor)
    {
      return;
    }

    FRequestPlaySessionParams Params;
    Params.WorldType = EPlaySessionWorldType::PlayInEditor;
    Params.SessionDestination = EPlaySessionDestinationType::InProcess;
    Params.GlobalMapOverride = InMapPackagePath;

    GEditor->RequestPlaySession(Params);
  }

  void OnEndPIE(bool bIsSimulating)
  {
    if (!GWaitingForPieToEnd)
    {
      return;
    }
    GWaitingForPieToEnd = false;

    if (!FUIWTPendingRestore::Get().IsSet())
    {
      GDeferredMapPackagePath.Reset();
      return;
    }

    const FString MapPackagePath = MoveTemp(GDeferredMapPackagePath);
    GDeferredMapPackagePath.Reset();

    if (GEditor)
    {
      GStartQueued = true;
      GEditor->GetTimerManager()->SetTimerForNextTick(
          FTimerDelegate::CreateLambda([MapPackagePath]()
                                       {
          GStartQueued = false;
          if (FUIWTPendingRestore::Get().IsSet()) {
            StartPlaySession(MapPackagePath);
          } }));
    }
  }

  void OnCancelPIE()
  {
    if (GWaitingForPieToEnd || GStartQueued)
    {
      return;
    }
    GDeferredMapPackagePath.Reset();
    FUIWTPendingRestore::Clear();
  }

}

namespace UIWTPlayLauncher
{

  void Startup()
  {
    GEndPieHandle = FEditorDelegates::EndPIE.AddStatic(&OnEndPIE);
    GCancelPieHandle = FEditorDelegates::CancelPIE.AddStatic(&OnCancelPIE);
  }

  void Shutdown()
  {
    FEditorDelegates::EndPIE.Remove(GEndPieHandle);
    FEditorDelegates::CancelPIE.Remove(GCancelPieHandle);
    GEndPieHandle.Reset();
    GCancelPieHandle.Reset();
    GWaitingForPieToEnd = false;
    GStartQueued = false;
    GDeferredMapPackagePath.Reset();
    FUIWTPendingRestore::Clear();
  }

  bool RequestPlay(const FUIWTPlayRequest &InRequest, FText &OutError)
  {
    if (!GEditor)
    {
      OutError = LOCTEXT("PlayNoEditor", "The editor is not available.");
      return false;
    }

    if (InRequest.MapPackagePath.IsEmpty())
    {
      OutError = LOCTEXT("PlayNoTarget",
                         "This widget has no level and no checkpoint to play on. "
                         "Assign a checkpoint, or use it in a level.");
      return false;
    }

    if (!FPackageName::DoesPackageExist(InRequest.MapPackagePath))
    {
      OutError = FText::Format(
          LOCTEXT("PlayMapMissing", "The map '{0}' no longer exists."),
          FText::FromString(InRequest.MapPackagePath));
      return false;
    }

    if (InRequest.CheckpointId.IsValid())
    {
      const UUIWidgetPreviewObjectManagerSettings *Settings =
          GetDefault<UUIWidgetPreviewObjectManagerSettings>();

      FUIWTCheckpointSidecar Sidecar;
      FUIWTCheckpointResult Result;
      if (!UIWTCheckpointCodec::ReadSidecar(InRequest.CheckpointSidecarPath,
                                            Sidecar, Result))
      {
        OutError = FText::Format(
            LOCTEXT("PlaySidecarBad", "The checkpoint sidecar is unusable: {0}"),
            FText::FromString(Result.Message));
        return false;
      }

      FUIWTCheckpointLoadOptions Options =
          Settings ? Settings->MakeLoadOptions() : FUIWTCheckpointLoadOptions();
      Options.ExpectedCheckpointId = InRequest.CheckpointId;
      Options.ExpectedMapPackagePath = InRequest.MapPackagePath;

      FUIWTCheckpointHeader Header;
      if (!UIWTCheckpointCodec::ValidateHeader(InRequest.CheckpointPayloadPath,
                                               Options, Header, Result))
      {
        OutError = FText::Format(
            LOCTEXT("PlayPayloadBad", "The checkpoint is unusable: [{0}] {1}"),
            FText::FromString(Result.GetErrorCode()),
            FText::FromString(Result.Message));
        return false;
      }
    }

    FUIWTPendingRestore &Pending = FUIWTPendingRestore::Get();
    Pending = FUIWTPendingRestore();
    Pending.bPending = true;
    Pending.MapPackagePath = InRequest.MapPackagePath;
    Pending.CheckpointId = InRequest.CheckpointId;
    if (InRequest.CheckpointId.IsValid())
    {
      Pending.CheckpointFile = InRequest.CheckpointPayloadPath;
      Pending.CheckpointSidecarFile = InRequest.CheckpointSidecarPath;
    }
    Pending.WidgetClass = InRequest.WidgetClass;

    if (GEditor->PlayWorld != nullptr || GEditor->bIsSimulatingInEditor)
    {
      GWaitingForPieToEnd = true;
      GDeferredMapPackagePath = InRequest.MapPackagePath;
      GEditor->RequestEndPlayMap();
      UE_LOG(LogUIWidgetToolPlugin, Log,
             TEXT("Queued a restart on %s behind the running PIE session."),
             *InRequest.MapPackagePath);
      return true;
    }

    UE_LOG(LogUIWidgetToolPlugin, Log, TEXT("Starting a play session on %s%s."),
           *InRequest.MapPackagePath,
           InRequest.CheckpointId.IsValid()
               ? TEXT(" with a checkpoint to restore")
               : TEXT(" with no checkpoint"));

    StartPlaySession(InRequest.MapPackagePath);
    return true;
  }

}

#undef LOCTEXT_NAMESPACE
