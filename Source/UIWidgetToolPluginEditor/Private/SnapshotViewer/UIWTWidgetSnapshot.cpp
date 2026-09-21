#include "UIWTWidgetSnapshot.h"

#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "FastUpdate/WidgetProxy.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Layout/ArrangedWidget.h"
#include "Layout/Visibility.h"
#include "Misc/Base64.h"
#include "Misc/Compression.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Types/ReflectionMetadata.h"
#include "Types/SlateAttributeMetaData.h"
#include "UIWTCheckpointTypes.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWidget.h"

#define LOCTEXT_NAMESPACE "UIWidgetToolSnapshot"

namespace
{
  struct FUIWTSnapshotTextureData
  {
    FIntVector Dimensions = FIntVector::ZeroValue;
    TArray<FColor> ColorData;
  };

  FText GetWidgetType(const TSharedPtr<const SWidget> &InWidget)
  {
    return InWidget.IsValid() ? FText::FromString(InWidget->GetTypeAsString())
                              : FText::GetEmpty();
  }

  FText GetWidgetTypeAndShortName(const TSharedPtr<const SWidget> &InWidget)
  {
    if (!InWidget.IsValid())
    {
      return FText::GetEmpty();
    }

    const FText WidgetType = GetWidgetType(InWidget);

    if (TSharedPtr<FReflectionMetaData> MetaData =
            InWidget->GetMetaData<FReflectionMetaData>())
    {
      if (MetaData->Name != NAME_None)
      {
        return FText::Format(LOCTEXT("WidgetTypeAndName", "{0} ({1})"), WidgetType,
                             FText::FromName(MetaData->Name));
      }
    }
    return WidgetType;
  }

  FText GetWidgetClippingText(const TSharedPtr<const SWidget> &InWidget)
  {
    if (InWidget.IsValid())
    {
      switch (InWidget->GetClipping())
      {
      case EWidgetClipping::Inherit:
        return LOCTEXT("WidgetClippingNo", "No");
      case EWidgetClipping::ClipToBounds:
        return LOCTEXT("WidgetClippingYes", "Yes");
      case EWidgetClipping::ClipToBoundsAlways:
        return LOCTEXT("WidgetClippingYesAlways", "Yes (Always)");
      case EWidgetClipping::ClipToBoundsWithoutIntersecting:
        return LOCTEXT("WidgetClippingYesWithoutIntersecting",
                       "Yes (No Intersect)");
      case EWidgetClipping::OnDemand:
        return LOCTEXT("WidgetClippingOnDemand", "On Demand");
      }
    }
    return FText::GetEmpty();
  }

  int32 GetWidgetAttributeCount(const TSharedPtr<const SWidget> &InWidget)
  {
    if (InWidget.IsValid())
    {
      if (FSlateAttributeMetaData *MetaData =
              FSlateAttributeMetaData::FindMetaData(*InWidget.Get()))
      {
        return MetaData->GetRegisteredAttributeCount();
      }
    }
    return 0;
  }

  int32
  GetWidgetCollapsedAttributeCount(const TSharedPtr<const SWidget> &InWidget)
  {
    if (InWidget.IsValid())
    {
      if (FSlateAttributeMetaData *MetaData =
              FSlateAttributeMetaData::FindMetaData(*InWidget.Get()))
      {
        return MetaData->GetRegisteredAffectVisibilityAttributeCount();
      }
    }
    return 0;
  }

  FAssetData GetWidgetAssetData(const TSharedPtr<const SWidget> &InWidget)
  {
    if (InWidget.IsValid())
    {
      TSharedPtr<FReflectionMetaData> MetaData =
          InWidget->GetMetaData<FReflectionMetaData>();
      if (MetaData.IsValid() && MetaData->Asset.Get() != nullptr)
      {
        return FAssetData(MetaData->Asset.Get());
      }
    }
    return FAssetData();
  }

  TSharedRef<FUIWTSnapshotNode> MakeNode(const FArrangedWidget &InWidgetGeometry)
  {
    const TSharedRef<SWidget> &Widget = InWidgetGeometry.Widget;
    const TSharedPtr<const SWidget> ConstWidget = Widget;

    TSharedRef<FUIWTSnapshotNode> Node = MakeShared<FUIWTSnapshotNode>();

    Node->AccumulatedLayoutTransform =
        InWidgetGeometry.Geometry.GetAccumulatedLayoutTransform();
    Node->AccumulatedRenderTransform =
        InWidgetGeometry.Geometry.GetAccumulatedRenderTransform();
    Node->LocalSize = InWidgetGeometry.Geometry.GetLocalSize();

    const EVisibility WidgetVisibility = Widget->GetVisibility();
    Node->bHitTestVisible = WidgetVisibility.IsHitTestVisible();
    Node->bChildrenHitTestVisible = WidgetVisibility.AreChildrenHitTestVisible();

    Node->WidgetType = GetWidgetType(ConstWidget);
    Node->WidgetTypeAndShortName = GetWidgetTypeAndShortName(ConstWidget);
    Node->WidgetVisibilityText = FText::FromString(WidgetVisibility.ToString());
    Node->WidgetClippingText = GetWidgetClippingText(ConstWidget);
    Node->WidgetReadableLocation =
        FText::FromString(FReflectionMetaData::GetWidgetDebugInfo(&Widget.Get()));

    Node->bVisible = WidgetVisibility.IsVisible();
    Node->bVisibleInherited =
        Widget->GetProxyHandle().GetWidgetVisibility(&Widget.Get()).IsVisible();
    Node->bFocusable = Widget->SupportsKeyboardFocus();
    Node->bNeedsTick = Widget->GetCanTick();
    Node->bIsVolatile = Widget->IsVolatile();
    Node->bIsVolatileIndirectly = Widget->IsVolatileIndirectly();
    Node->bHasActiveTimers = Widget->HasActiveTimers();
    Node->bIsInvalidationRoot = Widget->Advanced_IsInvalidationRoot();
    Node->bEnabled = Widget->IsEnabled();

    Node->bSupportsInvalidation = Widget->SupportsInvalidation();
    Node->bSupportsInvalidationRecursive = Widget->SupportsInvalidationRecursive(
        SWidget::EInvalidationStrategy::UseCachedValue);

    Node->LayerId = Widget->GetPersistentState().LayerId;
    Node->LayerIdOut = Widget->GetPersistentState().OutgoingLayerId;
    Node->File = Widget->GetCreatedInLocation().GetPlainNameString();
    Node->LineNumber = Widget->GetCreatedInLocation().GetNumber();
    Node->AttributeCount = GetWidgetAttributeCount(ConstWidget);
    Node->CollapsedAttributeCount = GetWidgetCollapsedAttributeCount(ConstWidget);

    const UE::Slate::FDeprecateVector2DResult Desired = Widget->GetDesiredSize();
    Node->DesiredSize = FVector2D(Desired.X, Desired.Y);
    Node->ForegroundColor = Widget->GetForegroundColor();

    Node->WidgetAddress =
        static_cast<uint64>(reinterpret_cast<PTRINT>(&Widget.Get()));
    Node->AssetPathString = GetWidgetAssetData(ConstWidget).GetObjectPathString();
    if (TSharedPtr<FReflectionMetaData> MetaData =
            ConstWidget->GetMetaData<FReflectionMetaData>())
    {
      if (MetaData->Name != NAME_None)
      {
        Node->UMGName = MetaData->Name.ToString();
      }
    }

    return Node;
  }

  TSharedRef<FJsonValue> MakeVector2Json(double InX, double InY)
  {
    TArray<TSharedPtr<FJsonValue>> Values;
    Values.Add(MakeShared<FJsonValueNumber>(InX));
    Values.Add(MakeShared<FJsonValueNumber>(InY));
    return MakeShared<FJsonValueArray>(Values);
  }

  TSharedRef<FJsonValue> MakeLinearColorJson(const FLinearColor &InColor)
  {
    TArray<TSharedPtr<FJsonValue>> Values;
    Values.Add(MakeShared<FJsonValueNumber>(InColor.R));
    Values.Add(MakeShared<FJsonValueNumber>(InColor.G));
    Values.Add(MakeShared<FJsonValueNumber>(InColor.B));
    Values.Add(MakeShared<FJsonValueNumber>(InColor.A));
    return MakeShared<FJsonValueArray>(Values);
  }

  TSharedRef<FJsonValue> MakeSlateColorJson(const FSlateColor &InColor)
  {
    const bool bIsColorSpecified = InColor.IsColorSpecified();
    const FLinearColor ColorToUse =
        bIsColorSpecified ? InColor.GetSpecifiedColor() : FLinearColor::White;

    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetBoolField(UIWTSnapshotFormat::Key::IsColorSpecified, bIsColorSpecified);
    Object->SetField(UIWTSnapshotFormat::Key::Color, MakeLinearColorJson(ColorToUse));
    return MakeShared<FJsonValueObject>(Object);
  }

  TSharedRef<FJsonValue>
  MakeLayoutTransformJson(const FSlateLayoutTransform &InTransform)
  {
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetNumberField(UIWTSnapshotFormat::Key::Scale, InTransform.GetScale());
    Object->SetField(UIWTSnapshotFormat::Key::Translation,
                     MakeVector2Json(InTransform.GetTranslation().X,
                                     InTransform.GetTranslation().Y));
    return MakeShared<FJsonValueObject>(Object);
  }

  TSharedRef<FJsonValue>
  MakeRenderTransformJson(const FSlateRenderTransform &InTransform)
  {
    float M00 = 0.f, M01 = 0.f, M10 = 0.f, M11 = 0.f;
    InTransform.GetMatrix().GetMatrix(M00, M01, M10, M11);

    TArray<TSharedPtr<FJsonValue>> MatrixValues;
    MatrixValues.Add(MakeShared<FJsonValueNumber>(M00));
    MatrixValues.Add(MakeShared<FJsonValueNumber>(M01));
    MatrixValues.Add(MakeShared<FJsonValueNumber>(M10));
    MatrixValues.Add(MakeShared<FJsonValueNumber>(M11));

    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetField(UIWTSnapshotFormat::Key::Matrix,
                     MakeShared<FJsonValueArray>(MatrixValues));
    Object->SetField(UIWTSnapshotFormat::Key::Translation,
                     MakeVector2Json(InTransform.GetTranslation().X,
                                     InTransform.GetTranslation().Y));
    return MakeShared<FJsonValueObject>(Object);
  }

  TSharedRef<FJsonValue> MakeHitTestInfoJson(bool bIsHitTestVisible,
                                             bool bAreChildrenHitTestVisible)
  {
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetBoolField(UIWTSnapshotFormat::Key::IsHitTestVisible, bIsHitTestVisible);
    Object->SetBoolField(UIWTSnapshotFormat::Key::AreChildrenHitTestVisible,
                         bAreChildrenHitTestVisible);
    return MakeShared<FJsonValueObject>(Object);
  }

  TSharedRef<FJsonValue> NodeToJson(const TSharedRef<FUIWTSnapshotNode> &InNode,
                                    int32 &InOutWidgetCount)
  {
    ++InOutWidgetCount;

    using namespace UIWTSnapshotFormat;
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();

    Object->SetField(Key::AccumulatedLayoutTransform,
                     MakeLayoutTransformJson(InNode->AccumulatedLayoutTransform));
    Object->SetField(Key::AccumulatedRenderTransform,
                     MakeRenderTransformJson(InNode->AccumulatedRenderTransform));
    Object->SetField(Key::LocalSize,
                     MakeVector2Json(InNode->LocalSize.X, InNode->LocalSize.Y));
    Object->SetField(Key::HitTestInfo,
                     MakeHitTestInfoJson(InNode->bHitTestVisible,
                                         InNode->bChildrenHitTestVisible));
    Object->SetField(Key::Tint, MakeLinearColorJson(InNode->Tint));
    Object->SetField(Key::WidgetDesiredSize,
                     MakeVector2Json(InNode->DesiredSize.X, InNode->DesiredSize.Y));
    Object->SetField(Key::WidgetForegroundColor,
                     MakeSlateColorJson(InNode->ForegroundColor));
    Object->SetStringField(Key::WidgetAddress,
                           FString::Printf(TEXT("0x%0llx"), InNode->WidgetAddress));

#define UIWT_WRITE_BOOL(JsonKey, Member) Object->SetBoolField(TEXT(#JsonKey), InNode->Member);
#define UIWT_WRITE_INT(JsonKey, Member) Object->SetNumberField(TEXT(#JsonKey), InNode->Member);
#define UIWT_WRITE_STRING(JsonKey, Member) Object->SetStringField(TEXT(#JsonKey), InNode->Member);
#define UIWT_WRITE_TEXT(JsonKey, Member) Object->SetStringField(TEXT(#JsonKey), InNode->Member.ToString());
    UIWT_SNAPSHOT_BOOL_FIELDS(UIWT_WRITE_BOOL)
    UIWT_SNAPSHOT_INT_FIELDS(UIWT_WRITE_INT)
    UIWT_SNAPSHOT_STRING_FIELDS(UIWT_WRITE_STRING)
    UIWT_SNAPSHOT_TEXT_FIELDS(UIWT_WRITE_TEXT)
#undef UIWT_WRITE_BOOL
#undef UIWT_WRITE_INT
#undef UIWT_WRITE_STRING
#undef UIWT_WRITE_TEXT

    TArray<TSharedPtr<FJsonValue>> ChildValues;
    ChildValues.Reserve(InNode->ChildNodes.Num());
    for (const TSharedRef<FUIWTSnapshotNode> &Child : InNode->ChildNodes)
    {
      ChildValues.Add(NodeToJson(Child, InOutWidgetCount));
    }
    Object->SetArrayField(Key::ChildNodes, ChildValues);

    return MakeShared<FJsonValueObject>(Object);
  }

  TSharedRef<FJsonValue>
  TextureToJson(const FUIWTSnapshotTextureData &InTextureData)
  {
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetField(UIWTSnapshotFormat::Key::Dimensions,
                     MakeVector2Json(InTextureData.Dimensions.X,
                                     InTextureData.Dimensions.Y));

    const int32 UncompressedBytes =
        InTextureData.ColorData.Num() * sizeof(FColor);

    TArray<uint8> CompressedBuffer;
    CompressedBuffer.AddZeroed(
        FCompression::CompressMemoryBound(NAME_Zlib, UncompressedBytes));
    int32 CompressedSize = CompressedBuffer.Num();

    if (UncompressedBytes > 0 &&
        FCompression::CompressMemory(NAME_Zlib, CompressedBuffer.GetData(),
                                     CompressedSize,
                                     InTextureData.ColorData.GetData(),
                                     UncompressedBytes))
    {
      Object->SetBoolField(UIWTSnapshotFormat::Key::IsCompressed, true);
      Object->SetNumberField(UIWTSnapshotFormat::Key::UncompressedSize, UncompressedBytes);
      CompressedBuffer.SetNum(CompressedSize, EAllowShrinking::No);
      Object->SetStringField(UIWTSnapshotFormat::Key::TextureData,
                             FBase64::Encode(CompressedBuffer));
    }
    else
    {
      Object->SetBoolField(UIWTSnapshotFormat::Key::IsCompressed, false);
      TArray<uint8> RawBytes;
      RawBytes.Append(
          reinterpret_cast<const uint8 *>(InTextureData.ColorData.GetData()),
          UncompressedBytes);
      Object->SetStringField(UIWTSnapshotFormat::Key::TextureData, FBase64::Encode(RawBytes));
    }

    return MakeShared<FJsonValueObject>(Object);
  }

  FArrangedWidget ArrangeRoot(const TSharedRef<SWidget> &InRoot)
  {
    return FArrangedWidget(InRoot, InRoot->GetTickSpaceGeometry());
  }

  FString DescribePIEWorlds()
  {
    if (!GEngine)
    {
      return FString();
    }

    TArray<FString> Names;
    for (const FWorldContext &Context : GEngine->GetWorldContexts())
    {
      if (Context.WorldType != EWorldType::PIE || !Context.GameViewport)
      {
        continue;
      }
      const UWorld *World = Context.World();
      Names.Add(World ? World->GetMapName() : TEXT("<unnamed>"));
    }
    return FString::Join(Names, TEXT(", "));
  }

  bool ResolvePIEWorldRoots(TArray<TSharedRef<SWidget>> &OutRoots)
  {
    OutRoots.Reset();
    if (!GEngine || !FSlateApplication::IsInitialized())
    {
      return false;
    }

    for (const FWorldContext &Context : GEngine->GetWorldContexts())
    {
      if (Context.WorldType != EWorldType::PIE || !Context.GameViewport)
      {
        continue;
      }
      if (TSharedPtr<SViewport> ViewportWidget =
              Context.GameViewport->GetGameViewportWidget())
      {
        OutRoots.AddUnique(ViewportWidget.ToSharedRef());
      }
    }

    if (OutRoots.Num() > 0)
    {
      return true;
    }

    if (TSharedPtr<SViewport> GameViewport =
            FSlateApplication::Get().GetGameViewport())
    {
      OutRoots.Add(GameViewport.ToSharedRef());
      return true;
    }
    return false;
  }

  TSharedRef<FUIWTSnapshotNode>
  BuildNodeTree(const FArrangedWidget &InWidgetGeometry)
  {
    TSharedRef<FUIWTSnapshotNode> Node = MakeNode(InWidgetGeometry);

    const TSharedRef<SWidget> &Parent = InWidgetGeometry.Widget;
#if WITH_SLATE_DEBUGGING
    FChildren *Children = Parent->Debug_GetChildrenForReflector();
#else
    FChildren *Children = Parent->GetChildren();
#endif

    if (!Children)
    {
      return Node;
    }

    for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
    {
      TSharedRef<SWidget> ChildWidget = Children->GetChildAt(ChildIndex);

      FGeometry ChildGeometry = ChildWidget->GetTickSpaceGeometry();

      if (ChildWidget->GetVisibility() == EVisibility::Collapsed ||
          !Parent->ValidatePathToChild(&ChildWidget.Get()))
      {
        ChildGeometry = FGeometry();
      }

      Node->ChildNodes.Add(
          BuildNodeTree(FArrangedWidget(ChildWidget, ChildGeometry)));
    }

    return Node;
  }

  bool SaveRoots(const TArray<TSharedRef<SWidget>> &InRoots,
                 const FString &InFilePath,
                 UIWTWidgetSnapshot::FSnapshotResult &OutResult)
  {
    TArray<TSharedRef<FUIWTSnapshotNode>> Nodes;
    TArray<FUIWTSnapshotTextureData> Textures;
    Nodes.Reserve(InRoots.Num());
    Textures.Reserve(InRoots.Num());

    int32 FailedScreenshots = 0;

    for (const TSharedRef<SWidget> &Root : InRoots)
    {
      Nodes.Add(BuildNodeTree(ArrangeRoot(Root)));

      FUIWTSnapshotTextureData &TextureData = Textures.AddDefaulted_GetRef();
      if (!FSlateApplication::Get().TakeScreenshot(Root, TextureData.ColorData,
                                                   TextureData.Dimensions))
      {
        TextureData.Dimensions = FIntVector::ZeroValue;
        TextureData.ColorData.Reset();
        ++FailedScreenshots;
      }
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(UIWTSnapshotFormat::Key::Version, UIWTSnapshotFormat::JsonVersion);
    Root->SetStringField(UIWTSnapshotFormat::Key::PortedFromEngine, UIWTSnapshotFormat::PortedFromEngine);

    {
      TArray<TSharedPtr<FJsonValue>> RootValues;
      RootValues.Reserve(Nodes.Num());
      for (const TSharedRef<FUIWTSnapshotNode> &Node : Nodes)
      {
        RootValues.Add(NodeToJson(Node, OutResult.WidgetCount));
      }
      Root->SetArrayField(UIWTSnapshotFormat::Key::Windows, RootValues);
    }

    Root->SetArrayField(UIWTSnapshotFormat::Key::NavigationData, TArray<TSharedPtr<FJsonValue>>());

    {
      TArray<TSharedPtr<FJsonValue>> TextureValues;
      TextureValues.Reserve(Textures.Num());
      for (const FUIWTSnapshotTextureData &TextureData : Textures)
      {
        TextureValues.Add(TextureToJson(TextureData));
      }
      Root->SetArrayField(UIWTSnapshotFormat::Key::Textures, TextureValues);
    }

    TUniquePtr<FArchive> FileAr(IFileManager::Get().CreateFileWriter(*InFilePath));
    if (!FileAr)
    {
      OutResult.Message =
          FString::Printf(TEXT("Failed to open '%s' for writing."), *InFilePath);
      return false;
    }

    using FSnapshotWriter = TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;
    using FSnapshotWriterFactory =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;

    TSharedRef<FSnapshotWriter> Writer =
        FSnapshotWriterFactory::Create(FileAr.Get());
    const bool bSerialized = FJsonSerializer::Serialize(Root, Writer);
    FileAr->Close();
    const bool bArchiveError = FileAr->IsError();
    FileAr.Reset();

    if (!bSerialized || bArchiveError)
    {
      IFileManager::Get().Delete(*InFilePath, false, true, true);
      OutResult.Message =
          FString::Printf(TEXT("Failed to write snapshot '%s'."), *InFilePath);
      return false;
    }

    OutResult.RootCount = InRoots.Num();
    OutResult.FileBytes = IFileManager::Get().FileSize(*InFilePath);
    if (FailedScreenshots > 0)
    {
      OutResult.Message = FString::Printf(
          TEXT("%d of %d viewport screenshots could not be read back; their "
               "hierarchy was still captured."),
          FailedScreenshots, InRoots.Num());
    }

    UE_LOG(LogUIWidgetToolPlugin, Log,
           TEXT("Wrote widget snapshot '%s': %d PIE viewport(s), %d widget(s), "
                "%lld bytes."),
           *InFilePath, OutResult.RootCount, OutResult.WidgetCount,
           OutResult.FileBytes);
    return true;
  }

  FString GetDefaultSnapshotDirectory()
  {
    return UUIWidgetPreviewObjectManagerSettings::Get()
        ->GetResolvedCheckpointDirectory();
  }

}

namespace UIWTWidgetSnapshot
{

  bool TakeSnapshot(const FString &InFilePath, FSnapshotResult &OutResult)
  {
    OutResult = FSnapshotResult();

    FString FilePath = InFilePath;
    if (FilePath.IsEmpty() || FPaths::IsRelative(FilePath))
    {
      FilePath = FPaths::Combine(GetDefaultSnapshotDirectory(), FilePath);
      FPaths::NormalizeFilename(FilePath);
    }
    OutResult.FilePath = FilePath;

    if (FPaths::GetCleanFilename(FilePath).IsEmpty())
    {
      OutResult.Message = TEXT("No snapshot file name to write to.");
      return false;
    }

    TArray<TSharedRef<SWidget>> Roots;
    if (!ResolvePIEWorldRoots(Roots))
    {
      OutResult.Message =
          TEXT("PIE is not running, or its viewport widget has already gone.");
      return false;
    }

    OutResult.RootDescription = DescribePIEWorlds();
    return SaveRoots(Roots, FilePath, OutResult);
  }

}

#undef LOCTEXT_NAMESPACE
