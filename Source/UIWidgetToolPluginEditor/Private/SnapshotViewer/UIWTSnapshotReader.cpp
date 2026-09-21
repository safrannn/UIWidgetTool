#include "UIWTSnapshotReader.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Compression.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UIWTCheckpointTypes.h"
#include "UIWidgetPreviewObjectManagerSettings.h"

#define LOCTEXT_NAMESPACE "UIWidgetToolSnapshotReader"

namespace
{

  int32 GBrushSerial = 0;

  bool ReadVector2(const TSharedPtr<FJsonObject> &InObject, const FString &InField,
                   double &OutX, double &OutY)
  {
    const TArray<TSharedPtr<FJsonValue>> *Values = nullptr;
    if (!InObject->TryGetArrayField(InField, Values) || Values->Num() < 2)
    {
      return false;
    }
    OutX = (*Values)[0]->AsNumber();
    OutY = (*Values)[1]->AsNumber();
    return true;
  }

  bool ReadLinearColor(const TSharedPtr<FJsonObject> &InObject,
                       const FString &InField, FLinearColor &OutColor)
  {
    const TArray<TSharedPtr<FJsonValue>> *Values = nullptr;
    if (!InObject->TryGetArrayField(InField, Values) || Values->Num() < 4)
    {
      return false;
    }
    OutColor.R = static_cast<float>((*Values)[0]->AsNumber());
    OutColor.G = static_cast<float>((*Values)[1]->AsNumber());
    OutColor.B = static_cast<float>((*Values)[2]->AsNumber());
    OutColor.A = static_cast<float>((*Values)[3]->AsNumber());
    return true;
  }

  void ReadLayoutTransform(const TSharedPtr<FJsonObject> &InObject,
                           FSlateLayoutTransform &OutTransform)
  {
    const TSharedPtr<FJsonObject> *Child = nullptr;
    if (!InObject->TryGetObjectField(UIWTSnapshotFormat::Key::AccumulatedLayoutTransform, Child))
    {
      return;
    }

    double Scale = 1.0;
    (*Child)->TryGetNumberField(UIWTSnapshotFormat::Key::Scale, Scale);

    double X = 0.0, Y = 0.0;
    ReadVector2(*Child, UIWTSnapshotFormat::Key::Translation, X, Y);

    OutTransform = FSlateLayoutTransform(
        static_cast<float>(Scale),
        FVector2f(static_cast<float>(X), static_cast<float>(Y)));
  }

  void ReadRenderTransform(const TSharedPtr<FJsonObject> &InObject,
                           FSlateRenderTransform &OutTransform)
  {
    const TSharedPtr<FJsonObject> *Child = nullptr;
    if (!InObject->TryGetObjectField(UIWTSnapshotFormat::Key::AccumulatedRenderTransform, Child))
    {
      return;
    }

    const TArray<TSharedPtr<FJsonValue>> *Matrix = nullptr;
    if (!(*Child)->TryGetArrayField(UIWTSnapshotFormat::Key::Matrix, Matrix) ||
        Matrix->Num() < 4)
    {
      return;
    }

    double X = 0.0, Y = 0.0;
    ReadVector2(*Child, UIWTSnapshotFormat::Key::Translation, X, Y);

    const FMatrix2x2 Matrix2x2(static_cast<float>((*Matrix)[0]->AsNumber()),
                               static_cast<float>((*Matrix)[1]->AsNumber()),
                               static_cast<float>((*Matrix)[2]->AsNumber()),
                               static_cast<float>((*Matrix)[3]->AsNumber()));

    OutTransform = FSlateRenderTransform(
        Matrix2x2, FVector2f(static_cast<float>(X), static_cast<float>(Y)));
  }

  void ReadHitTestInfo(const TSharedPtr<FJsonObject> &InObject,
                       FUIWTSnapshotNode &OutNode)
  {
    const TSharedPtr<FJsonObject> *Child = nullptr;
    if (!InObject->TryGetObjectField(UIWTSnapshotFormat::Key::HitTestInfo, Child))
    {
      return;
    }
    (*Child)->TryGetBoolField(UIWTSnapshotFormat::Key::IsHitTestVisible, OutNode.bHitTestVisible);
    (*Child)->TryGetBoolField(UIWTSnapshotFormat::Key::AreChildrenHitTestVisible,
                              OutNode.bChildrenHitTestVisible);
  }

  void ReadForegroundColor(const TSharedPtr<FJsonObject> &InObject,
                           FUIWTSnapshotNode &OutNode)
  {
    const TSharedPtr<FJsonObject> *Child = nullptr;
    if (!InObject->TryGetObjectField(UIWTSnapshotFormat::Key::WidgetForegroundColor, Child))
    {
      return;
    }

    bool bColorSpecified = false;
    (*Child)->TryGetBoolField(UIWTSnapshotFormat::Key::IsColorSpecified, bColorSpecified);
    if (!bColorSpecified)
    {
      OutNode.ForegroundColor = FSlateColor::UseForeground();
      return;
    }

    FLinearColor Color = FLinearColor::White;
    ReadLinearColor(*Child, UIWTSnapshotFormat::Key::Color, Color);
    OutNode.ForegroundColor = FSlateColor(Color);
  }

  void ReadTextField(const TSharedPtr<FJsonObject> &InObject,
                     const FString &InField, FText &OutText)
  {
    FString Value;
    if (InObject->TryGetStringField(InField, Value))
    {
      OutText = FText::FromString(Value);
    }
  }

  // Files before format 2.3 carry no WidgetUMGName. A UMG widget still has
  // an asset path of its own, and the engine then writes its readable
  // location as "<Asset> [<Name>]"; internal Slate children of one get
  // "<Asset> [<Name>(File.cpp(N))]" from the parent and no asset path. So the
  // name is recoverable, and picks on the image keep mapping onto the
  // Blueprint tree for snapshots captured before the field existed.
  FString UMGNameFromReadableLocation(const FString &InLocation,
                                      const FString &InAssetPath)
  {
    const FString Prefix =
        FPackageName::ObjectPathToObjectName(InAssetPath) + TEXT(" [");
    if (!InLocation.StartsWith(Prefix) || !InLocation.EndsWith(TEXT("]")))
    {
      return FString();
    }
    const FString Name =
        InLocation.Mid(Prefix.Len(), InLocation.Len() - Prefix.Len() - 1);
    return Name.Contains(TEXT("(")) ? FString() : Name;
  }

  uint64 ParseWidgetAddress(const FString &InValue)
  {
    FString Digits = InValue;
    Digits.RemoveFromStart(TEXT("0x"), ESearchCase::IgnoreCase);
    if (Digits.IsEmpty())
    {
      return 0;
    }
    return FParse::HexNumber64(*Digits);
  }

  TSharedPtr<FUIWTSnapshotNode> ParseNode(const TSharedPtr<FJsonObject> &InObject,
                                          int32 InDepth, int32 &InOutWidgetCount,
                                          bool &bOutTruncated)
  {
    if (!InObject.IsValid())
    {
      return nullptr;
    }
    using namespace UIWTSnapshotFormat;
    if (InDepth >= MaxNodeDepth)
    {
      bOutTruncated = true;
      return nullptr;
    }

    TSharedRef<FUIWTSnapshotNode> Node = MakeShared<FUIWTSnapshotNode>();
    ++InOutWidgetCount;

    ReadLayoutTransform(InObject, Node->AccumulatedLayoutTransform);
    ReadRenderTransform(InObject, Node->AccumulatedRenderTransform);
    ReadHitTestInfo(InObject, *Node);
    ReadForegroundColor(InObject, *Node);
    ReadLinearColor(InObject, Key::Tint, Node->Tint);

    double X = 0.0, Y = 0.0;
    if (ReadVector2(InObject, Key::LocalSize, X, Y))
    {
      Node->LocalSize = FVector2f(static_cast<float>(X), static_cast<float>(Y));
    }
    if (ReadVector2(InObject, Key::WidgetDesiredSize, X, Y))
    {
      Node->DesiredSize = FVector2D(X, Y);
    }

#define UIWT_READ_BOOL(JsonKey, Member) InObject->TryGetBoolField(TEXT(#JsonKey), Node->Member);
#define UIWT_READ_INT(JsonKey, Member) InObject->TryGetNumberField(TEXT(#JsonKey), Node->Member);
#define UIWT_READ_STRING(JsonKey, Member) InObject->TryGetStringField(TEXT(#JsonKey), Node->Member);
#define UIWT_READ_TEXT(JsonKey, Member) ReadTextField(InObject, TEXT(#JsonKey), Node->Member);
    UIWT_SNAPSHOT_BOOL_FIELDS(UIWT_READ_BOOL)
    UIWT_SNAPSHOT_INT_FIELDS(UIWT_READ_INT)
    UIWT_SNAPSHOT_STRING_FIELDS(UIWT_READ_STRING)
    UIWT_SNAPSHOT_TEXT_FIELDS(UIWT_READ_TEXT)
#undef UIWT_READ_BOOL
#undef UIWT_READ_INT
#undef UIWT_READ_STRING
#undef UIWT_READ_TEXT

    if (Node->UMGName.IsEmpty() && !Node->AssetPathString.IsEmpty())
    {
      Node->UMGName = UMGNameFromReadableLocation(
          Node->WidgetReadableLocation.ToString(), Node->AssetPathString);
    }

    FString Address;
    if (InObject->TryGetStringField(Key::WidgetAddress, Address))
    {
      Node->WidgetAddress = ParseWidgetAddress(Address);
    }

    const TArray<TSharedPtr<FJsonValue>> *Children = nullptr;
    if (InObject->TryGetArrayField(Key::ChildNodes, Children))
    {
      Node->ChildNodes.Reserve(Children->Num());
      for (const TSharedPtr<FJsonValue> &ChildValue : *Children)
      {
        const TSharedPtr<FJsonObject> *ChildObject = nullptr;
        if (!ChildValue.IsValid() || !ChildValue->TryGetObject(ChildObject))
        {
          continue;
        }
        if (TSharedPtr<FUIWTSnapshotNode> Child = ParseNode(
                *ChildObject, InDepth + 1, InOutWidgetCount, bOutTruncated))
        {
          Node->ChildNodes.Add(Child.ToSharedRef());
        }
      }
    }

    return Node;
  }

  // OutPixels is the image as the writer captured it: FColor pixels, so BGRA
  // bytes, which is what FSlateDynamicImageBrush takes.
  bool ParseTexture(const TSharedPtr<FJsonObject> &InObject,
                    FIntPoint &OutDimensions, TArray<uint8> &OutPixels,
                    FString &OutWarning)
  {
    OutDimensions = FIntPoint::ZeroValue;
    OutPixels.Reset();

    if (!InObject.IsValid())
    {
      return false;
    }

    double Width = 0.0, Height = 0.0;
    if (!ReadVector2(InObject, UIWTSnapshotFormat::Key::Dimensions, Width, Height))
    {
      return false;
    }
    if (Width <= 0.0 || Height <= 0.0)
    {
      return false;
    }

    const int64 PixelCount =
        static_cast<int64>(Width) * static_cast<int64>(Height);
    const int64 ExpectedBytes = PixelCount * 4;
    if (ExpectedBytes <= 0 || ExpectedBytes > MAX_int32)
    {
      OutWarning = TEXT("A root's image dimensions are out of range.");
      return false;
    }

    FString Encoded;
    if (!InObject->TryGetStringField(UIWTSnapshotFormat::Key::TextureData, Encoded) ||
        Encoded.IsEmpty())
    {
      return false;
    }

    TArray<uint8> Decoded;
    if (!FBase64::Decode(Encoded, Decoded))
    {
      OutWarning = TEXT("A root's image data could not be decoded.");
      return false;
    }

    bool bCompressed = false;
    InObject->TryGetBoolField(UIWTSnapshotFormat::Key::IsCompressed, bCompressed);

    if (bCompressed)
    {
      int64 Reported = 0;
      if (!InObject->TryGetNumberField(UIWTSnapshotFormat::Key::UncompressedSize, Reported) ||
          Reported != ExpectedBytes)
      {
        OutWarning = TEXT("A root's image header disagrees with its dimensions.");
        return false;
      }

      OutPixels.SetNumUninitialized(static_cast<int32>(ExpectedBytes));
      if (!FCompression::UncompressMemory(
              NAME_Zlib, OutPixels.GetData(), static_cast<int32>(ExpectedBytes),
              Decoded.GetData(), Decoded.Num()))
      {
        OutPixels.Reset();
        OutWarning = TEXT("A root's image could not be decompressed.");
        return false;
      }
    }
    else
    {
      if (Decoded.Num() != ExpectedBytes)
      {
        OutWarning = TEXT("A root's image size disagrees with its dimensions.");
        return false;
      }
      OutPixels = MoveTemp(Decoded);
    }

    OutDimensions =
        FIntPoint(static_cast<int32>(Width), static_cast<int32>(Height));
    return true;
  }

  TSharedPtr<FSlateDynamicImageBrush> MakeBrush(const FIntPoint &InDimensions,
                                                const TArray<uint8> &InPixels)
  {
    if (InPixels.Num() == 0)
    {
      return nullptr;
    }
    const FName BrushName(
        *FString::Printf(TEXT("UIWTSnapshot_%d"), GBrushSerial++));
    return FSlateDynamicImageBrush::CreateWithImageData(
        BrushName,
        FVector2D(static_cast<float>(InDimensions.X),
                  static_cast<float>(InDimensions.Y)),
        InPixels);
  }

  int64 GetMaxSnapshotBytes()
  {
    int32 MaxMB = 256;
    if (const UUIWidgetPreviewObjectManagerSettings *Settings =
            UUIWidgetPreviewObjectManagerSettings::Get())
    {
      MaxMB = FMath::Max(Settings->MaxSnapshotFileMB, 1);
    }
    return static_cast<int64>(MaxMB) * 1024 * 1024;
  }

  // Snapshots are written the way the engine's Widget Reflector writes them: raw
  // TCHAR streamed into the archive, so on Windows the file is UTF-16LE with no
  // BOM. FFileHelper::LoadFileToString only recognises UTF-16 by its BOM and
  // would otherwise decode the bytes as ANSI, so detect that layout here.
  bool LoadSnapshotText(const FString &InFilePath, FString &OutJson)
  {
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *InFilePath) || Bytes.Num() == 0)
    {
      return false;
    }

    const bool bHasUtf16Bom =
        Bytes.Num() >= 2 && ((Bytes[0] == 0xFF && Bytes[1] == 0xFE) ||
                             (Bytes[0] == 0xFE && Bytes[1] == 0xFF));
    const bool bHasUtf8Bom = Bytes.Num() >= 3 && Bytes[0] == 0xEF &&
                             Bytes[1] == 0xBB && Bytes[2] == 0xBF;
    const bool bLooksLikeBareUtf16Le = !bHasUtf16Bom && !bHasUtf8Bom &&
                                       Bytes.Num() >= 4 && Bytes[0] != 0 &&
                                       Bytes[1] == 0 && Bytes[3] == 0;

    if (bLooksLikeBareUtf16Le)
    {
      const int32 NumChars = Bytes.Num() / 2;
      FUTF16ToTCHAR Converter(
          reinterpret_cast<const UTF16CHAR *>(Bytes.GetData()), NumChars);
      OutJson = FString::ConstructFromPtrSize(Converter.Get(), Converter.Length());
      return true;
    }

    FFileHelper::BufferToString(OutJson, Bytes.GetData(), Bytes.Num());
    return true;
  }

  void AppendWarning(FString &InOutWarning, const FString &InLine)
  {
    if (InLine.IsEmpty())
    {
      return;
    }
    if (!InOutWarning.IsEmpty())
    {
      InOutWarning.Append(TEXT(" "));
    }
    InOutWarning.Append(InLine);
  }

}

FUIWTSnapshotDocument::~FUIWTSnapshotDocument()
{
  for (FUIWTSnapshotRoot &Root : Roots)
  {
    if (Root.Brush.IsValid())
    {
      Root.Brush->ReleaseResource();
    }
  }
}

int32 FUIWTSnapshotDocument::TotalWidgetCount() const
{
  int32 Total = 0;
  for (const FUIWTSnapshotRoot &Root : Roots)
  {
    Total += Root.WidgetCount;
  }
  return Total;
}

namespace UIWTSnapshotReader
{

  bool LoadSnapshotDocument(const FString &InFilePath,
                            FUIWTSnapshotDocument &OutDocument, FText &OutError)
  {
    OutError = FText::GetEmpty();

    if (InFilePath.IsEmpty())
    {
      OutError = LOCTEXT("NoPath", "No snapshot path was given.");
      return false;
    }

    IFileManager &FileManager = IFileManager::Get();
    if (!FileManager.FileExists(*InFilePath))
    {
      OutError = FText::Format(
          LOCTEXT("Missing", "'{0}' does not exist."),
          FText::FromString(FPaths::GetCleanFilename(InFilePath)));
      return false;
    }

    const int64 FileSize = FileManager.FileSize(*InFilePath);
    const int64 MaxBytes = GetMaxSnapshotBytes();
    if (FileSize > MaxBytes)
    {
      OutError = FText::Format(
          LOCTEXT("TooLarge",
                  "'{0}' is {1} MB, over the {2} MB snapshot limit."),
          FText::FromString(FPaths::GetCleanFilename(InFilePath)),
          FText::AsNumber(FileSize / (1024 * 1024)),
          FText::AsNumber(MaxBytes / (1024 * 1024)));
      return false;
    }

    FString Json;
    if (!LoadSnapshotText(InFilePath, Json))
    {
      OutError = FText::Format(
          LOCTEXT("Unreadable", "'{0}' could not be read."),
          FText::FromString(FPaths::GetCleanFilename(InFilePath)));
      return false;
    }

    TSharedPtr<FJsonObject> Root;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
      OutError = FText::Format(
          LOCTEXT("BadJson", "'{0}' is not a readable widget snapshot."),
          FText::FromString(FPaths::GetCleanFilename(InFilePath)));
      return false;
    }

    const TArray<TSharedPtr<FJsonValue>> *Windows = nullptr;
    if (!Root->TryGetArrayField(UIWTSnapshotFormat::Key::Windows, Windows) ||
        Windows->Num() == 0)
    {
      OutError = FText::Format(
          LOCTEXT("NoWindows", "'{0}' contains no widget hierarchy."),
          FText::FromString(FPaths::GetCleanFilename(InFilePath)));
      return false;
    }

    const TArray<TSharedPtr<FJsonValue>> *Textures = nullptr;
    Root->TryGetArrayField(UIWTSnapshotFormat::Key::Textures, Textures);

    OutDocument.FilePath = InFilePath;
    OutDocument.Roots.Reset();
    OutDocument.Roots.Reserve(Windows->Num());
    OutDocument.Warning.Reset();

    bool bTruncated = false;
    int32 MissingImages = 0;

    for (int32 Index = 0; Index < Windows->Num(); ++Index)
    {
      const TSharedPtr<FJsonObject> *WindowObject = nullptr;
      if (!(*Windows)[Index].IsValid() ||
          !(*Windows)[Index]->TryGetObject(WindowObject))
      {
        continue;
      }

      FUIWTSnapshotRoot &Entry = OutDocument.Roots.AddDefaulted_GetRef();
      Entry.Node = ParseNode(*WindowObject, 0, Entry.WidgetCount, bTruncated);

      if (Textures && Textures->IsValidIndex(Index) &&
          (*Textures)[Index].IsValid())
      {
        const TSharedPtr<FJsonObject> *TextureObject = nullptr;
        if ((*Textures)[Index]->TryGetObject(TextureObject))
        {
          TArray<uint8> Pixels;
          FString TextureWarning;
          if (ParseTexture(*TextureObject, Entry.Dimensions, Pixels,
                           TextureWarning))
          {
            Entry.Brush = MakeBrush(Entry.Dimensions, Pixels);
          }
          AppendWarning(OutDocument.Warning, TextureWarning);
        }
      }

      if (!Entry.Brush.IsValid())
      {
        Entry.Dimensions = FIntPoint::ZeroValue;
        ++MissingImages;
      }

      Entry.DisplayName =
          Entry.Dimensions.X > 0
              ? FString::Printf(TEXT("Root %d (%d x %d)"), Index + 1,
                                Entry.Dimensions.X, Entry.Dimensions.Y)
              : FString::Printf(TEXT("Root %d (no image)"), Index + 1);
    }

    if (OutDocument.Roots.Num() == 0)
    {
      OutError = FText::Format(
          LOCTEXT("NoRoots", "'{0}' contains no readable roots."),
          FText::FromString(FPaths::GetCleanFilename(InFilePath)));
      return false;
    }

    if (bTruncated)
    {
      AppendWarning(OutDocument.Warning,
                    FString::Printf(TEXT("The hierarchy was deeper than %d "
                                         "levels and was truncated."),
                                    UIWTSnapshotFormat::MaxNodeDepth));
    }
    if (MissingImages > 0)
    {
      AppendWarning(
          OutDocument.Warning,
          FString::Printf(TEXT("%d of %d root(s) have no image; their hierarchy "
                               "is still shown."),
                          MissingImages, OutDocument.Roots.Num()));
    }

    UE_LOG(LogUIWidgetToolPlugin, Log,
           TEXT("Read widget snapshot '%s': %d root(s), %d widget(s)."),
           *InFilePath, OutDocument.Roots.Num(), OutDocument.TotalWidgetCount());
    return true;
  }

}

#undef LOCTEXT_NAMESPACE
