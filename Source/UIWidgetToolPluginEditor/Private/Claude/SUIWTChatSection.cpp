#include "SUIWTChatSection.h"

#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Core/UIWTDesignFonts.h"
#include "Core/UIWTDesignImport.h"
#include "Core/UIWTGeneratedBlueprints.h"
#include "Core/UIWTJson.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Design/UIWTDesignRefine.h"
#include "Design/UIWTDesignSources.h"
#include "Dialog/SCustomDialog.h"
#include "Design/UIWTImageReader.h"
#include "DesktopPlatformModule.h"
#include "HAL/FileManager.h"
#include "Editor.h"
#include "EditorDirectories.h"
#include "Figma/UIWTFigmaClient.h"
#include "Framework/Application/SlateApplication.h"
#include "ISettingsModule.h"
#include "InputCoreTypes.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "UIWTChatTypes.h"
#include "Core/UIWTLocalSettings.h"
#include "UIWTPromptImage.h"
#include "Core/UIWTNotify.h"
#include "Host/UIWidgetToolPluginStyle.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "UIWTCheckpointTypes.h"
#include "WidgetBlueprint.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UIWidgetTool"

namespace
{
  constexpr float SendButtonTopPadding = 4.f;
  constexpr float MessageGap = 24.f;
  constexpr float MessageMaxWidthFraction = 0.8f;
  constexpr float PromptBoxHeight = 72.f;
  constexpr float AttachmentThumbnailHeight = 20.f;
  constexpr float MessageThumbnailHeight = 96.f;
  constexpr float ImportSourceMaxWidth = 180.f;

  // The name of the design's root node: for Figma, the frame's. Empty when
  // the design file is gone.
  FString ReadDesignRootName(const FString &InDesignFile)
  {
    FString Text;
    TSharedPtr<FJsonObject> Json;
    if (!FFileHelper::LoadFileToString(Text, *InDesignFile) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) ||
        !Json.IsValid())
    {
      return FString();
    }
    const FJsonObject *Root = UIWTJson::Obj(*Json, TEXT("root"));
    return Root ? UIWTJson::Str(*Root, TEXT("name")) : FString();
  }

  // What the design import that made the blueprint read: the Figma frame's
  // name, or the Photoshop document's or the image file's name. False when
  // no design import made it.
  bool DescribeImportSource(const UIWTDesignImport::FSidecar &InSidecar, FString &OutName,
                            FString &OutToolTip)
  {
    const FJsonObject *Ref = InSidecar.SourceRef.Get();
    if (InSidecar.Source == TEXT("figma"))
    {
      OutName = ReadDesignRootName(InSidecar.DesignFile);
      const FString NodeId = Ref ? UIWTJson::Str(*Ref, TEXT("nodeId")) : FString();
      if (OutName.IsEmpty())
      {
        // The cached design is gone: the root widget's name is the frame's,
        // unless an AI pass renamed it.
        const UIWTDesignTree::FRoleNames *Roles = InSidecar.Owned.Find(NodeId);
        const FString *Main = Roles ? Roles->Find(TEXT("main")) : nullptr;
        OutName = Main ? *Main : FString();
      }
      const FString FileName = Ref ? UIWTJson::Str(*Ref, TEXT("fileName")) : FString();
      OutToolTip = FString::Printf(TEXT("Imported from the Figma frame %s (node %s%s%s)."),
                                   *OutName, *NodeId, FileName.IsEmpty() ? TEXT("") : TEXT(" in "),
                                   *FileName);
    }
    else if (InSidecar.Source == TEXT("psd"))
    {
      FString Document = Ref ? UIWTJson::Str(*Ref, TEXT("document")) : FString();
      if (Document.IsEmpty() && Ref)
      {
        Document = UIWTJson::Str(*Ref, TEXT("manifest"));
      }
      OutName = FPaths::GetCleanFilename(Document);
      OutToolTip = FString::Printf(TEXT("Imported from the Photoshop document %s."), *Document);
    }
    else if (InSidecar.Source == TEXT("image"))
    {
      const FString File = Ref ? UIWTJson::Str(*Ref, TEXT("file")) : FString();
      OutName = FPaths::GetCleanFilename(File);
      OutToolTip = FString::Printf(TEXT("Imported from the image %s."), *File);
    }
    return !OutName.IsEmpty();
  }

  // The image's thumbnail at up to InMaxHeight, keeping its aspect. The
  // widget holds the image, so the brush outlives any history rebuild.
  TSharedRef<SWidget> MakeThumbnail(
      const TSharedRef<const FUIWTPromptImage> &InImage, float InMaxHeight)
  {
    if (!InImage->Thumbnail.IsValid())
    {
      return SNullWidget::NullWidget;
    }
    const FVector2f Size = InImage->Thumbnail->GetImageSize();
    const float Height = FMath::Min(InMaxHeight, Size.Y);
    const float Width = Size.Y > 0.f ? Height * Size.X / Size.Y : Height;
    return SNew(SBox)
        .WidthOverride(Width)
        .HeightOverride(Height)
        .ToolTipText(FText::FromString(InImage->FileName))
            [SNew(SImage).Image_Lambda([InImage]()
                                       { return InImage->Thumbnail.Get(); })];
  }
}

void SUIWTChatSection::Construct(const FArguments &InArgs)
{
  SelectedEntryId = InArgs._SelectedEntryId;
  GetRunContext = InArgs._GetRunContext;
  PrepareImportEntry = InArgs._PrepareImportEntry;
  SelectEntry = InArgs._SelectEntry;
  PickedWidget = InArgs._PickedWidget;

  ChildSlot
      [SNew(SVerticalBox) +
       SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 4.f))
           [SNew(SHorizontalBox) +
            SHorizontalBox::Slot().FillWidth(1.f) +
            SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [SAssignNew(ConnectButton, SButton)
                     .Text(this, &SUIWTChatSection::GetConnectText)
                     .ToolTipText(this, &SUIWTChatSection::GetConnectToolTip)
                     .OnClicked(this, &SUIWTChatSection::OnConnectToggled)] +
            SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(FMargin(4.f, 0.f, 0.f, 0.f))
                    [SNew(SBox)
                         .WidthOverride(this, &SUIWTChatSection::GetIconButtonSize)
                         .HeightOverride(this, &SUIWTChatSection::GetIconButtonSize)
                    [SNew(SButton)
                         .ButtonStyle(FUIWidgetToolPluginStyle::Get(), "UIWidgetTool.IconButton")
                         .ContentPadding(FMargin(0.f))
                         .HAlign(HAlign_Center)
                         .VAlign(VAlign_Center)
                         .ToolTipText(LOCTEXT(
                             "SettingsTip",
                             "Open the Claude settings."))
                         .OnClicked(this, &SUIWTChatSection::OnSettingsClicked)
                             [SNew(SImage)
                                  .Image(FUIWidgetToolPluginStyle::Get().GetBrush(
                                      "UIWidgetTool.Icons.Settings"))
                                  .ColorAndOpacity(
                                      FSlateColor::UseForeground())]]]] +
       SVerticalBox::Slot().FillHeight(1.f)
           [SNew(SBorder)
                .BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
                .Padding(FMargin(6.f))
                    [SAssignNew(History, SScrollBox)
                         .Orientation(Orient_Vertical)]] +
       SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 4.f, 0.f, 0.f))
           [SNew(SBox).HeightOverride(PromptBoxHeight)
                [SAssignNew(PromptBox, SMultiLineEditableTextBox)
                     .HintText(this, &SUIWTChatSection::GetPromptHint)
                     .AutoWrapText(true)
                     .IsEnabled(this, &SUIWTChatSection::IsPromptSubmitEnabled)
                     .OnTextChanged(this, &SUIWTChatSection::OnPromptTextChanged)
                     .OnKeyDownHandler(this,
                                       &SUIWTChatSection::OnPromptKeyDown)]] +
       SVerticalBox::Slot()
           .AutoHeight()
           .Padding(FMargin(0.f, SendButtonTopPadding, 0.f, 0.f))
               [SNew(SHorizontalBox) +
                SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [SNew(SBox)
                         .WidthOverride(this, &SUIWTChatSection::GetIconButtonSize)
                         .HeightOverride(this, &SUIWTChatSection::GetIconButtonSize)
                    [SAssignNew(ImportButton, SComboButton)
                         .HasDownArrow(false)
                         .ButtonStyle(FUIWidgetToolPluginStyle::Get(), "UIWidgetTool.IconButton")
                         .ContentPadding(FMargin(0.f))
                         .HAlign(HAlign_Center)
                         .VAlign(VAlign_Center)
                         .ToolTipText(LOCTEXT(
                             "ImportTip",
                             "Import: a Figma frame, a Photoshop export or an "
                             "image to build a blueprint from, or files to send "
                             "with the prompt."))
                         .IsEnabled(this, &SUIWTChatSection::IsImportEnabled)
                         .OnGetMenuContent(this, &SUIWTChatSection::MakeImportMenu)
                         .ButtonContent()
                             [SNew(SImage)
                                  .Image(FUIWidgetToolPluginStyle::Get().GetBrush(
                                      "UIWidgetTool.Icons.AddImage"))
                                  .ColorAndOpacity(
                                      FSlateColor::UseForeground())]]] +
                SHorizontalBox::Slot()
                    .AutoWidth()
                    .VAlign(VAlign_Center)
                    .Padding(FMargin(4.f, 0.f, 0.f, 0.f))
                        [SAssignNew(ImportSourceSlot, SBox)
                             .MaxDesiredWidth(ImportSourceMaxWidth)] +
                SHorizontalBox::Slot()
                    .FillWidth(1.f)
                    .HAlign(HAlign_Left)
                    .VAlign(VAlign_Center)
                    .Padding(FMargin(4.f, 0.f))
                        [SAssignNew(AttachmentSlot, SBox)] +
                SHorizontalBox::Slot().AutoWidth()
                    [SNew(SButton)
                         .VAlign(VAlign_Center)
                         .Text(LOCTEXT("CancelRunBtn", "Cancel"))
                         .ToolTipText(LOCTEXT(
                             "CancelRunTip",
                             "Stop the run and restore the blueprint from disk."))
                         .Visibility(this, &SUIWTChatSection::GetCancelVisibility)
                         .OnClicked(this, &SUIWTChatSection::OnCancelClicked)] +
                SHorizontalBox::Slot().AutoWidth()
                    [SNew(SButton)
                         .VAlign(VAlign_Center)
                         .Text(LOCTEXT("SendBtn", "Send"))
                         .ToolTipText(LOCTEXT(
                             "SendTip",
                             "Send this prompt. Ctrl+Enter in the box above "
                             "does the same thing."))
                         .Visibility(this, &SUIWTChatSection::GetSendVisibility)
                         .IsEnabled(this, &SUIWTChatSection::IsPromptSubmitEnabled)
                         .OnClicked(this, &SUIWTChatSection::OnSendClicked)]]];

  ChatChangedHandle = FUIWTClaudeService::Get().OnChatChanged().AddSP(
      this, &SUIWTChatSection::Refresh);
  Refresh();
}

SUIWTChatSection::~SUIWTChatSection()
{
  if (ChatChangedHandle.IsValid())
  {
    // The dock tab can outlive the module; TryGet is null by then.
    if (FUIWTClaudeService *Service = FUIWTClaudeService::TryGet())
    {
      Service->OnChatChanged().Remove(ChatChangedHandle);
    }
  }
}

void SUIWTChatSection::Refresh()
{
  if (!History.IsValid())
  {
    return;
  }
  const FGuid EntryId = SelectedEntryId.Get(FGuid());
  if ((PendingImage.IsValid() || !PendingFiles.IsEmpty()) &&
      PendingAttachmentEntryId != EntryId)
  {
    PendingImage.Reset();
    PendingFiles.Reset();
    RefreshAttachments();
  }
  RefreshImportSource();

  const FUIWTClaudeService *Service = FUIWTClaudeService::TryGet();
  const FUIWTChatState *State =
      Service && EntryId.IsValid() ? Service->FindChatState(EntryId) : nullptr;

  History->ClearChildren();
  if (State)
  {
    for (const FUIWTChatMessage &Message : State->Messages)
    {
      History->AddSlot().Padding(FMargin(0.f, 0.f, 0.f, MessageGap))
          [MakeMessageRow(Message)];
    }
  }
  History->ScrollToEnd();
}

void SUIWTChatSection::RefreshImportSource()
{
  if (!ImportSourceSlot.IsValid())
  {
    return;
  }
  // From the blueprint's sidecar, without loading the blueprint. Read again
  // only when the entry or the sidecar changed: Refresh runs on every chat
  // change.
  const FWidgetPreviewObject *PreviewObject = FindSelectedPreviewObject();
  const FString Package = PreviewObject && !PreviewObject->WidgetClass.IsNull()
                              ? PreviewObject->WidgetClass.ToSoftObjectPath().GetLongPackageName()
                              : FString();
  const FString Sidecar =
      Package.IsEmpty() ? FString() : UIWTDesignImport::GetSidecarPath(Package);
  const FDateTime Stamp =
      Sidecar.IsEmpty() ? FDateTime::MinValue() : IFileManager::Get().GetTimeStamp(*Sidecar);
  if (Sidecar == ImportSourceSidecar && Stamp == ImportSourceStamp)
  {
    return;
  }
  ImportSourceSidecar = Sidecar;
  ImportSourceStamp = Stamp;

  UIWTDesignImport::FSidecar Read;
  FString Error;
  FString Name;
  FString ToolTip;
  if (Stamp == FDateTime::MinValue() || !UIWTDesignImport::ReadSidecar(Package, Read, Error) ||
      !DescribeImportSource(Read, Name, ToolTip))
  {
    ImportSourceSlot->SetContent(SNullWidget::NullWidget);
    return;
  }
  ImportSourceSlot->SetContent(
      SNew(SBorder)
          .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
          .Padding(FMargin(6.f, 2.f))
          .ToolTipText(FText::FromString(ToolTip))
              [SNew(STextBlock)
                   .Text(FText::FromString(Name))
                   .OverflowPolicy(ETextOverflowPolicy::Ellipsis)]);
}

TSharedRef<SWidget>
SUIWTChatSection::MakeMessageRow(const FUIWTChatMessage &InMessage) const
{
  const bool bUser = InMessage.Role == EUIWTChatRole::User;

  FSlateColor Color = FSlateColor::UseForeground();
  const FSlateBrush *Brush = FAppStyle::GetBrush("ToolPanel.GroupBorder");
  switch (InMessage.Role)
  {
  case EUIWTChatRole::Error:
    Color = FSlateColor(FLinearColor(1.f, 0.35f, 0.35f));
    break;
  case EUIWTChatRole::Status:
    Color = FSlateColor::UseSubduedForeground();
    Brush = FAppStyle::GetBrush("NoBorder");
    break;
  default:
    break;
  }

  TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);
  const bool bHasImage = InMessage.Image.IsValid();
  if (bHasImage)
  {
    Content->AddSlot().AutoHeight().HAlign(HAlign_Left)
        [MakeThumbnail(InMessage.Image.ToSharedRef(), MessageThumbnailHeight)];
  }
  const bool bHasFiles = !InMessage.Files.IsEmpty();
  if (bHasFiles)
  {
    Content->AddSlot()
        .AutoHeight()
        .Padding(FMargin(0.f, bHasImage ? 4.f : 0.f, 0.f, 0.f))
            [SNew(STextBlock)
                 .Text(FText::Format(LOCTEXT("MessageFiles", "Attached: {0}"),
                                     FText::FromString(FString::Join(InMessage.Files,
                                                                     TEXT(", ")))))
                 .AutoWrapText(true)
                 .ColorAndOpacity(FSlateColor::UseSubduedForeground())];
  }
  if ((!bHasImage && !bHasFiles) || !InMessage.Text.IsEmpty())
  {
    Content->AddSlot()
        .AutoHeight()
        .Padding(FMargin(0.f, bHasImage || bHasFiles ? 4.f : 0.f, 0.f, 0.f))
            [SNew(STextBlock)
                 .Text(FText::FromString(InMessage.Text))
                 .AutoWrapText(true)
                 .ColorAndOpacity(Color)];
  }

  TSharedRef<SWidget> Bubble = SNew(SBorder)
                                   .BorderImage(Brush)
                                   .Padding(FMargin(8.f, 6.f))[Content];

  return SNew(SHorizontalBox) +
         SHorizontalBox::Slot().FillWidth(bUser ? 1.f - MessageMaxWidthFraction : 0.f) +
         SHorizontalBox::Slot()
             .FillWidth(MessageMaxWidthFraction)
             .HAlign(bUser ? HAlign_Right : HAlign_Left)[Bubble] +
         SHorizontalBox::Slot().FillWidth(bUser ? 0.f : 1.f - MessageMaxWidthFraction);
}

const FWidgetPreviewObject *SUIWTChatSection::FindSelectedPreviewObject() const
{
  const FGuid EntryId = SelectedEntryId.Get(FGuid());
  if (!EntryId.IsValid())
  {
    return nullptr;
  }
  return UUIWidgetPreviewObjectManagerSettings::Get()->FindWidgetPreviewObject(
      EntryId);
}

bool SUIWTChatSection::IsRunInFlight()
{
  const FUIWTClaudeService *Service = FUIWTClaudeService::TryGet();
  return Service && Service->IsRunInFlight();
}

bool SUIWTChatSection::IsMcpConnected()
{
  const FUIWTClaudeService *Service = FUIWTClaudeService::TryGet();
  return Service && Service->IsMcpConnected();
}

FText SUIWTChatSection::GetPromptDisabledReason() const
{
  const FWidgetPreviewObject *PreviewObject = FindSelectedPreviewObject();
  if (!PreviewObject)
  {
    return LOCTEXT("PromptNoEntry", "Select an entry.");
  }
  if (!IsMcpConnected())
  {
    return LOCTEXT("PromptNotConnected", "Connection to MCP required.");
  }
  if (IsRunInFlight())
  {
    return LOCTEXT("PromptBusy", "A run is in flight.");
  }
  if (GEditor && GEditor->PlayWorld)
  {
    return LOCTEXT("PromptPIE", "Stop Play In Editor first.");
  }
  return FText::GetEmpty();
}

bool SUIWTChatSection::IsPromptSubmitEnabled() const
{
  return GetPromptDisabledReason().IsEmpty();
}

FText SUIWTChatSection::GetPromptHint() const
{
  const FText Reason = GetPromptDisabledReason();
  if (!Reason.IsEmpty())
  {
    return Reason;
  }

  return LOCTEXT("PromptHint",
                 "send: ctrl+enter. paste:ctrl+v.");
}

void SUIWTChatSection::OnPromptTextChanged(const FText &NewText)
{
  PromptText = NewText;
}

FReply SUIWTChatSection::OnPromptKeyDown(const FGeometry &,
                                         const FKeyEvent &KeyEvent)
{
  const FKey Key = KeyEvent.GetKey();
  if (Key == EKeys::Enter && KeyEvent.IsControlDown())
  {
    Submit();
    return FReply::Handled();
  }

  // Paste: an image on the clipboard becomes the attachment; anything else
  // falls through to the text box's own paste.
  const bool bPasteChord =
      (Key == EKeys::V && KeyEvent.IsControlDown() && !KeyEvent.IsAltDown()) ||
      (Key == EKeys::Insert && KeyEvent.IsShiftDown());
  if (bPasteChord)
  {
    TSharedPtr<const FUIWTPromptImage> Image;
    FText Error;
    if (UIWTPromptImage::PasteFromClipboard(Image, Error))
    {
      AttachImage(Image, Error);
      return FReply::Handled();
    }
  }
  return FReply::Unhandled();
}

FReply SUIWTChatSection::OnSendClicked()
{
  Submit();
  return FReply::Handled();
}

void SUIWTChatSection::Submit()
{
  const FWidgetPreviewObject *PreviewObject = FindSelectedPreviewObject();
  if (!PreviewObject || !IsPromptSubmitEnabled() ||
      (PromptText.IsEmptyOrWhitespace() && !PendingImage.IsValid() && PendingFiles.IsEmpty()))
  {
    return;
  }
  // GetRunContext may confirm the entry's pending edit, which writes the
  // settings the pointer points into.
  const FGuid EntryId = PreviewObject->Id;
  const FString Prompt =
      PromptText.IsEmptyOrWhitespace() ? FString() : PromptText.ToString();

  const FUIWTRunContext Context = GetRunContext.IsBound()
                                      ? GetRunContext.Execute()
                                      : FUIWTRunContext();
  FText Error;
  if (FUIWTClaudeService::Get().StartRun(EntryId, Prompt, PendingImage, PendingFiles,
                                         Context, Error))
  {
    // Cleared only once the run is under way, so a refused Send keeps what
    // the user typed.
    PromptText = FText::GetEmpty();
    PromptBox->SetText(PromptText);
    PendingImage.Reset();
    PendingFiles.Reset();
    RefreshAttachments();
  }
  else
  {
    UIWTNotify::Show(Error, false);
  }
  Refresh();
}

void SUIWTChatSection::AttachImage(TSharedPtr<const FUIWTPromptImage> InImage,
                                   const FText &InError)
{
  if (!InImage.IsValid())
  {
    UIWTNotify::Show(InError, false);
    return;
  }
  SetPendingImage(MoveTemp(InImage));
}

FReply SUIWTChatSection::OnRemoveImageClicked()
{
  SetPendingImage(nullptr);
  return FReply::Handled();
}

FReply SUIWTChatSection::OnRemoveFilesClicked()
{
  SetPendingFiles(TArray<FString>());
  return FReply::Handled();
}

void SUIWTChatSection::SetPendingImage(
    TSharedPtr<const FUIWTPromptImage> InImage)
{
  PendingImage = MoveTemp(InImage);
  PendingAttachmentEntryId = SelectedEntryId.Get(FGuid());
  RefreshAttachments();
}

void SUIWTChatSection::SetPendingFiles(TArray<FString> InFiles)
{
  PendingFiles = MoveTemp(InFiles);
  PendingAttachmentEntryId = SelectedEntryId.Get(FGuid());
  RefreshAttachments();
}

void SUIWTChatSection::RefreshAttachments()
{
  if (!AttachmentSlot.IsValid())
  {
    return;
  }
  if (!PendingImage.IsValid() && PendingFiles.IsEmpty())
  {
    AttachmentSlot->SetContent(SNullWidget::NullWidget);
    return;
  }

  // A chip: an optional thumbnail, a label and a remove button.
  auto MakeChip = [this](TSharedRef<SWidget> InLeading, const FText &InLabel,
                         const FText &InToolTip, const FText &InRemoveToolTip,
                         FReply (SUIWTChatSection::*InOnRemove)())
  {
    return SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
        .Padding(FMargin(4.f, 2.f))
        .ToolTipText(InToolTip)
            [SNew(SHorizontalBox) +
             SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[InLeading] +
             SHorizontalBox::Slot()
                 .FillWidth(1.f)
                 .VAlign(VAlign_Center)
                 .Padding(FMargin(6.f, 0.f, 2.f, 0.f))
                     [SNew(STextBlock)
                          .Text(InLabel)
                          .OverflowPolicy(ETextOverflowPolicy::Ellipsis)] +
             SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                 [SNew(SButton)
                      .ButtonStyle(FAppStyle::Get(), "SimpleButton")
                      .ContentPadding(FMargin(2.f))
                      .ToolTipText(InRemoveToolTip)
                      .OnClicked(this, InOnRemove)
                          [SNew(SImage)
                               .Image(FUIWidgetToolPluginStyle::Get().GetBrush(
                                   "UIWidgetTool.Icons.RemoveImage"))
                               .ColorAndOpacity(FSlateColor::UseForeground())]]];
  };

  TSharedRef<SHorizontalBox> Chips = SNew(SHorizontalBox);
  if (PendingImage.IsValid())
  {
    const FText FileName = FText::FromString(PendingImage->FileName);
    Chips->AddSlot().FillWidth(1.f).VAlign(VAlign_Center)
        [MakeChip(MakeThumbnail(PendingImage.ToSharedRef(), AttachmentThumbnailHeight),
                  FileName, FileName,
                  LOCTEXT("RemoveImageTip", "Remove the attached image."),
                  &SUIWTChatSection::OnRemoveImageClicked)];
  }
  if (!PendingFiles.IsEmpty())
  {
    const FText Label =
        PendingFiles.Num() == 1
            ? FText::FromString(FPaths::GetCleanFilename(PendingFiles[0]))
            : FText::Format(LOCTEXT("AttachedFilesCount", "{0} files"),
                            FText::AsNumber(PendingFiles.Num()));
    Chips->AddSlot()
        .FillWidth(1.f)
        .VAlign(VAlign_Center)
        .Padding(FMargin(PendingImage.IsValid() ? 4.f : 0.f, 0.f, 0.f, 0.f))
            [MakeChip(SNullWidget::NullWidget, Label,
                      FText::FromString(FString::Join(PendingFiles, TEXT("\n"))),
                      LOCTEXT("RemoveFilesTip", "Remove the attached files."),
                      &SUIWTChatSection::OnRemoveFilesClicked)];
  }
  AttachmentSlot->SetContent(Chips);
}

void SUIWTChatSection::AttachFiles()
{
  IDesktopPlatform *DesktopPlatform = FDesktopPlatformModule::Get();
  if (!DesktopPlatform)
  {
    return;
  }
  TArray<FString> Picked;
  if (!DesktopPlatform->OpenFileDialog(
          FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared()),
          LOCTEXT("FilesBrowseTitle", "Files to send with the prompt").ToString(),
          FEditorDirectories::Get().GetLastDirectory(ELastDirectory::GENERIC_OPEN),
          FString(), TEXT("All files (*.*)|*.*"), EFileDialogFlags::Multiple, Picked) ||
      Picked.IsEmpty())
  {
    return;
  }
  FEditorDirectories::Get().SetLastDirectory(ELastDirectory::GENERIC_OPEN,
                                             FPaths::GetPath(Picked[0]));

  // Added to the files already attached for this entry; two files of the
  // same name would be copied over each other, so the later one wins.
  TArray<FString> Files =
      PendingAttachmentEntryId == SelectedEntryId.Get(FGuid()) ? PendingFiles : TArray<FString>();
  for (const FString &File : Picked)
  {
    const FString Full = FPaths::ConvertRelativePathToFull(File);
    const FString Name = FPaths::GetCleanFilename(Full);
    Files.RemoveAll([&Name](const FString &InExisting)
                    { return FPaths::GetCleanFilename(InExisting) == Name; });
    Files.Add(Full);
  }
  SetPendingFiles(MoveTemp(Files));
}

FReply SUIWTChatSection::OnCancelClicked()
{
  FUIWTClaudeService::Get().CancelRun();
  return FReply::Handled();
}

FReply SUIWTChatSection::OnConnectToggled()
{
  FUIWTClaudeService &Service = FUIWTClaudeService::Get();
  if (Service.IsMcpConnected())
  {
    Service.DisconnectMcp();
    UIWTNotify::Show(LOCTEXT("McpDisconnected", "MCP server stopped."), true);
    return FReply::Handled();
  }

  FText Error;
  FUIWTSavedSessions Sessions;
  if (!Service.ConnectMcp(Error, &Sessions))
  {
    UIWTNotify::Show(Error, false);
    return FReply::Handled();
  }
  FText Message = LOCTEXT("McpConnected",
                          "MCP server running on localhost. Claude Code found.");
  if (Sessions.Resumable > 0)
  {
    Message = FText::Format(
        LOCTEXT("McpConnectedResumable",
                "{0}\n{1} {1}|plural(one=entry resumes its,other=entries resume their) "
                "saved Claude session."),
        Message, Sessions.Resumable);
  }
  if (Sessions.Expired > 0)
  {
    Message = FText::Format(
        LOCTEXT("McpConnectedExpired",
                "{0}\n{1} saved {1}|plural(one=session is,other=sessions are) gone from "
                "Claude Code; {1}|plural(one=that entry starts,other=those entries start) "
                "a new one."),
        Message, Sessions.Expired);
  }
  UIWTNotify::Show(Message, true);
  // Each entry's history is loaded now; show the selected one's.
  Refresh();
  return FReply::Handled();
}

FReply SUIWTChatSection::OnSettingsClicked()
{
  if (ISettingsModule *SettingsModule =
          FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
  {
    const UUIWTLocalSettings *Settings = GetDefault<UUIWTLocalSettings>();
    SettingsModule->ShowViewer(Settings->GetContainerName(),
                               Settings->GetCategoryName(),
                               Settings->GetSectionName());
  }
  return FReply::Handled();
}

// ---------------------------------------------------------------------------
// The import menu

namespace
{
  constexpr float ImportMenuWidth = 220.f;

  FString Tokens(int64 InTokens)
  {
    return InTokens >= 1000000 ? FString::Printf(TEXT("%.1fM"), InTokens / 1e6)
                               : FString::Printf(TEXT("%lldK"), (InTokens + 500) / 1000);
  }

  FString EstimateLine(const UIWTDesignRefine::FEstimate &InEstimate)
  {
    return FString::Printf(
        TEXT("%d rounds: about %s tokens read, %s read back from the prompt cache and %s "
             "written; %s."),
        InEstimate.Rounds, *Tokens(InEstimate.InputTokens), *Tokens(InEstimate.CachedTokens),
        *Tokens(InEstimate.OutputTokens),
        InEstimate.Dollars >= 0.0
            ? *FString::Printf(TEXT("about $%.2f at API rates"), InEstimate.Dollars)
            : TEXT("no prices known for this model (Sonnet and Opus only)"));
  }

  const TCHAR *SourceName(EUIWTDesignSource InSource)
  {
    return InSource == EUIWTDesignSource::Figma ? TEXT("Figma") : TEXT("Photoshop");
  }

  // Imports the design as a new blueprint for the entry InOutEntryId, or for
  // a new manager entry when there is no such entry. False with OutError set
  // when nothing was imported.
  bool ImportAsEntry(const UIWTDesignImport::FRequest &InRequest, EUIWTDesignSource InSource,
                     UIWTDesignImport::FResult &OutImported, FGuid &InOutEntryId,
                     FString &OutError)
  {
    if (!UIWTDesignImport::Import(InRequest, OutImported, OutError))
    {
      return false;
    }
    if (!OutImported.Blueprint || !OutImported.Blueprint->GeneratedClass)
    {
      OutError = TEXT("The import made no usable blueprint.");
      return false;
    }
    UUIWidgetPreviewObjectManagerSettings *Settings =
        UUIWidgetPreviewObjectManagerSettings::Get();
    const TSoftClassPtr<UUserWidget> Class(OutImported.Blueprint->GeneratedClass);
    FWidgetPreviewObject *Entry = Settings->FindWidgetPreviewObject(InOutEntryId);
    if (Entry)
    {
      Entry->WidgetClass = Class;
      Entry->WidgetName = OutImported.Blueprint->GetName();
    }
    else
    {
      InOutEntryId = Settings->AddWidgetPreviewObject(Class, OutImported.Blueprint->GetName());
      Entry = Settings->FindWidgetPreviewObject(InOutEntryId);
    }
    if (Entry)
    {
      Entry->Note = FString::Printf(TEXT("Imported from %s."), SourceName(InSource));
    }
    Settings->SaveWidgetPreviewObjects();
    if (FUIWTClaudeService *Service = FUIWTClaudeService::TryGet())
    {
      Service->OnEntriesChanged().Broadcast();
    }
    return true;
  }

  // The blueprint at InPackage when a design import made it (it has a
  // sidecar), else null.
  UWidgetBlueprint *FindImportedBlueprint(const FString &InPackage)
  {
    if (InPackage.IsEmpty() || !FPaths::FileExists(UIWTDesignImport::GetSidecarPath(InPackage)))
    {
      return nullptr;
    }
    UWidgetBlueprint *Blueprint = LoadObject<UWidgetBlueprint>(
        nullptr, *(InPackage + TEXT(".") + FPackageName::GetShortName(InPackage)));
    return Blueprint && Blueprint->GeneratedClass ? Blueprint : nullptr;
  }

  // The manager entry that shows InBlueprint after the design went into it:
  // InEntryId while that entry has no widget yet or already shows the
  // blueprint, else the first entry showing it, else a new one. InNote set:
  // the entry's new note.
  FGuid EntryForBlueprint(const FGuid &InEntryId, UWidgetBlueprint *InBlueprint,
                          const FString &InNote = FString())
  {
    UUIWidgetPreviewObjectManagerSettings *Settings =
        UUIWidgetPreviewObjectManagerSettings::Get();
    const TSoftClassPtr<UUserWidget> Class(InBlueprint->GeneratedClass);
    FWidgetPreviewObject *Entry = Settings->FindWidgetPreviewObject(InEntryId);
    if (Entry && !Entry->WidgetClass.IsNull() && Entry->WidgetClass != Class)
    {
      Entry = nullptr;
    }
    if (!Entry)
    {
      Entry = Settings->WidgetPreviewObjects.FindByPredicate(
          [&Class](const FWidgetPreviewObject &InEntry) { return InEntry.WidgetClass == Class; });
    }
    FGuid EntryId;
    if (!Entry)
    {
      EntryId = Settings->AddWidgetPreviewObject(Class, InBlueprint->GetName());
      Entry = Settings->FindWidgetPreviewObject(EntryId);
    }
    if (Entry)
    {
      Entry->WidgetClass = Class;
      Entry->WidgetName = InBlueprint->GetName();
      if (!InNote.IsEmpty())
      {
        Entry->Note = InNote;
      }
      EntryId = Entry->Id;
    }
    Settings->SaveWidgetPreviewObjects();
    if (FUIWTClaudeService *Service = FUIWTClaudeService::TryGet())
    {
      Service->OnEntriesChanged().Broadcast();
    }
    return EntryId;
  }

  enum class EReimportOutcome : uint8
  {
    Merged,
    Replaced,
    Declined,
    Failed
  };

  // Puts the design into InBlueprint, an earlier import's, the way the user
  // picks: Merge, the re-import merge (changes made in UE are kept), or
  // Replace, which deletes it and imports the design fresh under the same
  // name (they are lost). Only Replace is offered when the merge can't be
  // planned, a design from another frame for one. Merged: OutPlan is what
  // the merge did. Replaced: OutReplaced has the new blueprint, and
  // InBlueprint is gone. Failed: OutError says why.
  EReimportOutcome ReimportInto(UWidgetBlueprint *InBlueprint,
                                const UIWTDesignImport::FRequest &InRequest,
                                UIWTDesignImport::FReimportPlan &OutPlan,
                                UIWTDesignImport::FResult &OutReplaced, FString &OutError)
  {
    UIWTDesignImport::FReimportRequest Request;
    Request.Blueprint = InBlueprint;
    Request.DesignFile = InRequest.DesignFile;
    Request.ReferenceImage = InRequest.ReferenceImage;
    FString PlanError;
    const bool bCanMerge = UIWTDesignImport::PlanReimport(Request, OutPlan, PlanError);
    const FText Name = FText::FromString(InBlueprint->GetName());
    const FText ReplaceLine =
        FText::Format(LOCTEXT("ReimportReplaceLine",
                              "Replace: delete {0} and its textures and import the design "
                              "fresh under the same name. Changes made in UE are lost."),
                      Name);
    const FText Message =
        bCanMerge
            ? FText::Format(LOCTEXT("ReimportConfirm",
                                    "Import the design into {0}?\n\n"
                                    "Merge: {1} design changes since the last import, {2} "
                                    "changes made in UE kept, {3} conflicts.\n\n{4}\n\n"
                                    "Either way the blueprint keeps its name and is saved."),
                            Name, FText::AsNumber(OutPlan.Changes.Num()),
                            FText::AsNumber(OutPlan.KeptUEChanges),
                            FText::AsNumber(OutPlan.Conflicts), ReplaceLine)
            : FText::Format(LOCTEXT("ReimportCantMerge",
                                    "The design can't be merged into {0}: {1}\n\n{2}"),
                            Name, FText::FromString(PlanError), ReplaceLine);

    enum class EChoice : uint8
    {
      Merge,
      Replace,
      Cancel
    };
    TArray<EChoice> Choices;
    TArray<SCustomDialog::FButton> Buttons;
    if (bCanMerge)
    {
      Choices.Add(EChoice::Merge);
      Buttons.Add(SCustomDialog::FButton(LOCTEXT("ReimportMerge", "Merge"))
                      .SetPrimary(true)
                      .SetFocus());
    }
    Choices.Add(EChoice::Replace);
    Buttons.Add(SCustomDialog::FButton(LOCTEXT("ReimportReplace", "Replace")));
    Choices.Add(EChoice::Cancel);
    Buttons.Add(SCustomDialog::FButton(LOCTEXT("ReimportCancel", "Cancel"))
                    .SetButtonRole(SCustomDialog::EButtonRole::Cancel));
    const TSharedRef<SCustomDialog> Dialog =
        SNew(SCustomDialog)
            .Title(LOCTEXT("ReimportConfirmTitle", "Design import"))
            .Content()[SNew(STextBlock).Text(Message).WrapTextAt(520.f)]
            .Buttons(Buttons);
    const int32 Pressed = Dialog->ShowModal();
    const EChoice Choice = Choices.IsValidIndex(Pressed) ? Choices[Pressed] : EChoice::Cancel;

    if (Choice == EChoice::Replace)
    {
      return UIWTDesignImport::ReplaceImport(InBlueprint, InRequest, OutReplaced, OutError)
                 ? EReimportOutcome::Replaced
                 : EReimportOutcome::Failed;
    }
    if (Choice != EChoice::Merge)
    {
      return EReimportOutcome::Declined;
    }
    UIWTDesignImport::FReimportResult Applied;
    if (!UIWTDesignImport::ApplyReimport(OutPlan, Applied, OutError) ||
        !UIWTDesignImport::AcceptReimport(InBlueprint, true, OutError))
    {
      if (UIWTDesignImport::HasPendingReimport(InBlueprint))
      {
        FString DiscardError;
        UIWTDesignImport::DiscardReimport(InBlueprint, DiscardError);
      }
      return EReimportOutcome::Failed;
    }
    return EReimportOutcome::Merged;
  }
}

bool SUIWTChatSection::IsImportEnabled() const
{
  return !IsRunInFlight() && !(GEditor && GEditor->PlayWorld);
}

TSharedRef<SWidget> SUIWTChatSection::MakeImportMenu()
{
  TSharedRef<SVerticalBox> Menu = SNew(SVerticalBox);
  Menu->AddSlot().AutoHeight()[MakeImportSources()];
  AddEntryAIPassRows(Menu);
  return SNew(SBox).MinDesiredWidth(ImportMenuWidth).Padding(FMargin(2.f))[Menu];
}

TSharedRef<SWidget> SUIWTChatSection::MakeImportSources()
{
  return SNew(SVerticalBox) +
       SVerticalBox::Slot().AutoHeight()
           [MakeImportRow(
               LOCTEXT("ImportFigma", "Figma"),
               LOCTEXT("ImportFigmaTip",
                       "Import a Figma frame."),
               FOnClicked::CreateSPLambda(this,
                                          [this]
                                          {
                                            ImportDesign(EUIWTDesignSource::Figma);
                                            return FReply::Handled();
                                          }),
               true)] +
       SVerticalBox::Slot().AutoHeight()
           [MakeImportRow(
               LOCTEXT("ImportPSD", "Photoshop"),
               LOCTEXT("ImportPSDTip",
                       "Import a Photoshop export (Export for Unreal in the UIWidgetTool "
                       "Bridge panel, Tools/PhotoshopBridge)."),
               FOnClicked::CreateSPLambda(this,
                                          [this]
                                          {
                                            ImportDesign(EUIWTDesignSource::Photoshop);
                                            return FReply::Handled();
                                          }),
               true)] +
       SVerticalBox::Slot().AutoHeight()
           [MakeImportRow(
               LOCTEXT("ImportImage", "Image"),
               LOCTEXT("ImportImageTip",
                       "Build a widget blueprint from an image."),
               FOnClicked::CreateSPLambda(this,
                                          [this]
                                          {
                                            ImportDesign(EUIWTDesignSource::Image);
                                            return FReply::Handled();
                                          }),
               true)] +
       SVerticalBox::Slot().AutoHeight()
           [MakeImportRow(
               LOCTEXT("ImportFiles", "Files"),
               LOCTEXT("ImportFilesTip",
                       "Attach files from this machine to send with the next prompt in "
                       "the selected entry's chat."),
               FOnClicked::CreateSPLambda(this,
                                          [this]
                                          {
                                            AttachFiles();
                                            return FReply::Handled();
                                          }),
               TAttribute<bool>::CreateLambda(
                   [this] { return FindSelectedPreviewObject() != nullptr; }))];
}

UWidgetBlueprint *SUIWTChatSection::FindSelectedDesignBlueprint(FString &OutWhyNot) const
{
  const FWidgetPreviewObject *PreviewObject = FindSelectedPreviewObject();
  if (!PreviewObject || PreviewObject->WidgetClass.IsNull())
  {
    OutWhyNot = TEXT("Select an entry whose blueprint a design import made.");
    return nullptr;
  }
  UWidgetBlueprint *Blueprint = UIWTGenerated::FindWidgetBlueprint(PreviewObject->WidgetClass);
  if (!Blueprint)
  {
    OutWhyNot = TEXT("The selected entry's blueprint can't be loaded.");
    return nullptr;
  }
  if (!FPaths::FileExists(
          UIWTDesignImport::GetSidecarPath(Blueprint->GetOutermost()->GetName())))
  {
    OutWhyNot = TEXT("The selected entry's blueprint wasn't made by a design import (it has "
                     "no <name>.design.json next to it).");
    return nullptr;
  }
  return Blueprint;
}

void SUIWTChatSection::AddEntryAIPassRows(const TSharedRef<SVerticalBox> &InMenu)
{
  FString WhyNot;
  UWidgetBlueprint *Blueprint = FindSelectedDesignBlueprint(WhyNot);
  UIWTDesignImport::FSidecar Sidecar;
  if (Blueprint && !UIWTDesignImport::ReadSidecar(Blueprint->GetOutermost()->GetName(), Sidecar,
                                                  WhyNot))
  {
    Blueprint = nullptr;
  }
  if (Blueprint && !Sidecar.ComponentKey.IsEmpty())
  {
    WhyNot = TEXT("The selected entry's blueprint is a child WBP made from a component; refine "
                  "the screens that use it.");
    Blueprint = nullptr;
  }
  if (!Blueprint)
  {
    return;
  }
  const FGuid EntryId = SelectedEntryId.Get(FGuid());
  const TWeakObjectPtr<UWidgetBlueprint> WeakBlueprint(Blueprint);
  auto Enabled = [this] { return IsPromptSubmitEnabled(); };
  auto Start = [this, EntryId, WeakBlueprint](UIWTDesignRefine::FScope InScope)
  {
    return FOnClicked::CreateSPLambda(
        this,
        [this, EntryId, WeakBlueprint, InScope]
        {
          if (UWidgetBlueprint *Pinned = WeakBlueprint.Get())
          {
            RunAIPass(EntryId, Pinned, InScope, {});
          }
          return FReply::Handled();
        });
  };

  // The picked widget's part: the design node it (or its nearest owned
  // ancestor) was made from, unless that's the whole design.
  const FString Picked = PickedWidget.Get(FString());
  const FString PickedNode =
      UIWTDesignRefine::NodeForWidget(Blueprint, Sidecar.Owned, Picked);
  const FString RootNode =
      Blueprint->WidgetTree && Blueprint->WidgetTree->RootWidget
          ? UIWTDesignRefine::NodeForWidget(Blueprint, Sidecar.Owned,
                                            Blueprint->WidgetTree->RootWidget->GetName())
          : FString();
  const bool bPickedPart = !PickedNode.IsEmpty() && PickedNode != RootNode;
  if (!Picked.IsEmpty())
  {
    UIWTDesignRefine::FScope Scope;
    Scope.Nodes.Add(PickedNode);
    InMenu->AddSlot().AutoHeight()
        [MakeImportRow(
            FText::Format(LOCTEXT("EntryAIPassPicked", "{0} only..."), FText::FromString(Picked)),
            bPickedPart
                ? FText::Format(LOCTEXT("EntryAIPassPickedTip",
                                        "Refine only the part of the blueprint made from the "
                                        "design node {0}, which the widget picked in the "
                                        "snapshot viewer belongs to. The reference is cropped "
                                        "to it."),
                                FText::FromString(PickedNode))
                : PickedNode.IsEmpty()
                      ? LOCTEXT("EntryAIPassPickedNone",
                                "The picked widget wasn't made from a design node (it was "
                                "added in UE); pick a widget the import made.")
                      : LOCTEXT("EntryAIPassPickedRoot",
                                "The picked widget is the design's root; pick a part "
                                "of the design."),
            Start(Scope),
            TAttribute<bool>::CreateLambda([Enabled, bPickedPart] { return bPickedPart && Enabled(); }))];
  }

  // After a re-import: the nodes it added or changed.
  if (!Sidecar.ChangedNodes.IsEmpty())
  {
    UIWTDesignRefine::FScope Scope;
    Scope.Nodes = Sidecar.ChangedNodes;
    Scope.bChangedParts = true;
    InMenu->AddSlot().AutoHeight()
        [MakeImportRow(FText::Format(LOCTEXT("EntryAIPassChanged",
                                             "Changed parts ({0} nodes)..."),
                                     FText::AsNumber(Sidecar.ChangedNodes.Num())),
                       LOCTEXT("EntryAIPassChangedTip",
                               "Refine only the design nodes the last re-import added or "
                               "changed. Once a pass on them succeeds, this entry goes away "
                               "until the next re-import."),
                       Start(Scope), TAttribute<bool>::CreateLambda(Enabled))];
  }
}

TSharedRef<SWidget> SUIWTChatSection::MakeImportRow(const FText &InLabel,
                                                    const FText &InToolTip,
                                                    FOnClicked InOnClicked,
                                                    TAttribute<bool> InEnabled)
{
  return SNew(SButton)
      .ButtonStyle(FAppStyle::Get(), "Menu.Button")
      .ContentPadding(FMargin(8.f, 4.f))
      .ToolTipText(InToolTip)
      .IsEnabled(InEnabled)
      .OnClicked_Lambda(
          [this, InOnClicked]
          {
            // Closed first: the item opens a dialog.
            if (ImportButton.IsValid())
            {
              ImportButton->SetIsOpen(false);
            }
            return InOnClicked.Execute();
          })[SNew(STextBlock).Text(InLabel)];
}

void SUIWTChatSection::ImportDesign(EUIWTDesignSource InSource)
{
  FUIWTDesignImportChoice &Last = InSource == EUIWTDesignSource::Figma   ? LastFigmaChoice
                                  : InSource == EUIWTDesignSource::Image ? LastImageChoice
                                                                         : LastPsdChoice;
  FUIWTDesignImportChoice Choice = Last;
  if (!UIWTDesignImportDialog::Show(InSource, AsShared(), Choice))
  {
    return;
  }
  Last = Choice;
  const bool bAIPass = UIWTDesignImportDialog::IsAIPassOn(InSource);
  const bool bImage = InSource == EUIWTDesignSource::Image;
  if (bImage && !IsMcpConnected())
  {
    UIWTNotify::Show(LOCTEXT("ImageNeedsMcp",
                             "An image import is a Claude run: Connect MCP first."),
                     false);
    return;
  }
  if (bAIPass && !IsMcpConnected())
  {
    UIWTNotify::Show(LOCTEXT("AIPassNeedsMcp",
                             "The AI pass needs the MCP connection: Connect MCP first, or "
                             "turn the AI pass off in the import window."),
                     false);
    return;
  }

  // The selected entry is the target: its widget gets the design and keeps
  // its name, or, when it has none yet, the import makes one for it under
  // the name typed for it. A widget no design import made can't take the
  // merge: the import overwrites it in place, same folder and name. Nothing
  // selected, or an image into a widget the image can't go into: a new
  // entry, leaving the selected one alone.
  const FString PendingName =
      PrepareImportEntry.IsBound() ? PrepareImportEntry.Execute() : FString();
  const FWidgetPreviewObject *Target = FindSelectedPreviewObject();
  FGuid TargetId = Target ? Target->Id : FGuid();
  TWeakObjectPtr<UWidgetBlueprint> TargetBlueprint;
  bool bOverwriteEntry = false;
  if (Target && !Target->WidgetClass.IsNull())
  {
    FString WhyNot;
    TargetBlueprint = FindSelectedDesignBlueprint(WhyNot);
    // The merge only takes a design from the source the blueprint came
    // from; for an image, checked before the run, which would spend tokens
    // first. Other sources are checked by the merge once fetched.
    UIWTDesignImport::FSidecar Sidecar;
    if (bImage && TargetBlueprint.IsValid() &&
        !UIWTDesignImport::ReadSidecar(TargetBlueprint->GetOutermost()->GetName(), Sidecar,
                                       WhyNot))
    {
      TargetBlueprint.Reset();
    }
    else if (bImage && TargetBlueprint.IsValid() && Sidecar.Source != TEXT("image"))
    {
      WhyNot = FString::Printf(TEXT("It was imported from %s, and a blueprint only takes "
                                    "designs from its own source."),
                               *Sidecar.Source);
      TargetBlueprint.Reset();
    }
    if (!TargetBlueprint.IsValid() && bImage)
    {
      UE_LOG(LogUIWidgetToolPlugin, Log,
             TEXT("Importing as a new entry instead of into %s: %s"), *Target->WidgetName,
             *WhyNot);
      TargetId = FGuid();
    }
    else if (!TargetBlueprint.IsValid())
    {
      UWidgetBlueprint *Blueprint = UIWTGenerated::FindWidgetBlueprint(Target->WidgetClass);
      if (!Blueprint)
      {
        UIWTNotify::Show(FText::Format(LOCTEXT("OverwriteEntryMissing",
                                               "{0}'s blueprint can't be loaded."),
                                       FText::FromString(Target->WidgetName)),
                         false);
        return;
      }
      // An empty widget (New Widget's, no root yet) has nothing to lose, so
      // it is overwritten without asking.
      const bool bEmpty = !Blueprint->WidgetTree || !Blueprint->WidgetTree->RootWidget;
      const FText Message = FText::Format(
          LOCTEXT("OverwriteEntryConfirm",
                  "{0} wasn't made by a design import, so the design can't be merged into "
                  "it.\n\n"
                  "Overwrite it? {0} is deleted and the design imported in its place, under "
                  "the same name. Changes made to it in UE are lost."),
          FText::FromString(Blueprint->GetName()));
      if (!bEmpty && FMessageDialog::Open(EAppMsgType::YesNo, Message,
                                          LOCTEXT("OverwriteEntryTitle", "Design import")) !=
                         EAppReturnType::Yes)
      {
        return;
      }
      TargetBlueprint = Blueprint;
      bOverwriteEntry = true;
    }
  }
  if (bImage)
  {
    StartImageImport(Choice, bAIPass, TargetId, TargetBlueprint.Get(), PendingName);
    return;
  }

  UIWTDesignImport::FRequest Request;
  Request.TargetFolder = Choice.TargetFolder;
  Request.BlueprintName = PendingName;
  const TWeakPtr<SWidget> WeakThis = AsShared();
  const bool bIntoWidget = TargetBlueprint.IsValid();
  auto Finish = [WeakThis, InSource, bAIPass, TargetId, bIntoWidget, bOverwriteEntry,
                 TargetBlueprint](const UIWTDesignImport::FRequest &InRequest,
                                  const FString &InError)
  {
    FString Error = InError;
    if (Error.IsEmpty() && bIntoWidget && !TargetBlueprint.IsValid())
    {
      Error = TEXT("The selected entry's blueprint is gone.");
    }
    else if (Error.IsEmpty() && !bIntoWidget && TargetId.IsValid() &&
             !UUIWidgetPreviewObjectManagerSettings::Get()->FindWidgetPreviewObject(TargetId))
    {
      Error = TEXT("The selected entry was removed during the import.");
    }
    // The design goes into an earlier import's blueprint: the selected
    // entry's, or the one this import would create, which would otherwise
    // fail as taken. The user picks merge or replace.
    UWidgetBlueprint *Existing =
        !Error.IsEmpty() ? nullptr
        : bIntoWidget    ? TargetBlueprint.Get()
                         : FindImportedBlueprint(UIWTDesignImport::GetImportPackage(InRequest));
    UIWTDesignImport::FResult Imported;
    FGuid EntryId = TargetId;
    bool bReplaced = false;
    if (bOverwriteEntry && Error.IsEmpty())
    {
      // The selected entry's blueprint, no import's: overwritten in place,
      // so the entry keeps showing the same class path.
      if (UIWTDesignImport::OverwriteImport(TargetBlueprint.Get(), InRequest, Imported, Error))
      {
        EntryId = EntryForBlueprint(
            TargetId, Imported.Blueprint,
            FString::Printf(TEXT("Imported from %s."), SourceName(InSource)));
        bReplaced = true;
      }
    }
    else if (Existing)
    {
      UIWTDesignImport::FReimportPlan Plan;
      switch (ReimportInto(Existing, InRequest, Plan, Imported, Error))
      {
      case EReimportOutcome::Merged:
        EntryId = EntryForBlueprint(TargetId, Existing);
        if (const TSharedPtr<SWidget> Pinned = WeakThis.Pin())
        {
          StaticCastSharedPtr<SUIWTChatSection>(Pinned)->OnDesignReimported(EntryId, Existing,
                                                                            Plan, bAIPass);
        }
        return;
      case EReimportOutcome::Replaced:
        EntryId = EntryForBlueprint(
            TargetId, Imported.Blueprint,
            FString::Printf(TEXT("Imported from %s."), SourceName(InSource)));
        bReplaced = true;
        break;
      case EReimportOutcome::Declined:
        return;
      case EReimportOutcome::Failed:
        break;
      }
    }
    else if (Error.IsEmpty())
    {
      ImportAsEntry(InRequest, InSource, Imported, EntryId, Error);
    }
    if (!Error.IsEmpty())
    {
      UE_LOG(LogUIWidgetToolPlugin, Error, TEXT("%s import failed: %s"), SourceName(InSource),
             *Error);
      UIWTNotify::Show(FText::Format(LOCTEXT("DesignImportFailed", "{0} import failed: {1}"),
                                     FText::FromString(SourceName(InSource)),
                                     FText::FromString(Error)),
                       false);
      return;
    }
    UE_LOG(LogUIWidgetToolPlugin, Log, TEXT("%s import: %s"), SourceName(InSource),
           *UIWTDesignImport::ResultToJson(Imported));
    if (const TSharedPtr<SWidget> Pinned = WeakThis.Pin())
    {
      StaticCastSharedPtr<SUIWTChatSection>(Pinned)->OnDesignImported(InSource, Imported,
                                                                      EntryId, bAIPass, bReplaced);
    }
  };

  // The design's fonts first (found in the project, taken from this
  // computer or downloaded), so the conversion uses them.
  auto FontsThenFinish = [Finish](const UIWTDesignImport::FRequest &InRequest)
  {
    UIWTDesignFonts::EnsureFonts(
        InRequest.DesignFile,
        [InRequest, Finish](const UIWTDesignFonts::FFontsResult &InFonts)
        {
          const FString Fonts = UIWTDesignFonts::Summary(InFonts);
          if (!Fonts.IsEmpty())
          {
            UIWTNotify::Show(FText::FromString(Fonts), InFonts.Failed.IsEmpty());
          }
          Finish(InRequest, FString());
        });
  };

  if (InSource == EUIWTDesignSource::Figma)
  {
    UIWTNotify::Show(LOCTEXT("FetchingFigma", "Fetching the Figma frame..."), true);
    UIWTFigmaClient::Fetch(
        Choice.Source, false,
        [Request, Finish, FontsThenFinish](const UIWTFigmaClient::FFetchResult &InFetched) mutable
        {
          Request.DesignFile = InFetched.DesignFile;
          Request.ReferenceImage = InFetched.ReferenceImage;
          if (!InFetched.Error.IsEmpty())
          {
            Finish(Request, InFetched.Error);
            return;
          }
          FontsThenFinish(Request);
        });
    return;
  }
  UIWTDesignSources::FPsdExport Export;
  FString Error;
  if (!UIWTDesignSources::PreparePsd(Choice.Source, Export, Error))
  {
    Finish(Request, Error);
    return;
  }
  Request.DesignFile = Export.DesignFile;
  Request.ReferenceImage = Export.ReferenceImage;
  FontsThenFinish(Request);
}

void SUIWTChatSection::OnDesignImported(EUIWTDesignSource InSource,
                                        const UIWTDesignImport::FResult &InImported,
                                        const FGuid &InEntryId, bool bInAIPass,
                                        bool bInReplaced)
{
  SelectEntry.ExecuteIfBound(InEntryId);
  const FText Summary = FText::Format(
      LOCTEXT("DesignImportedSummary", "{0} widgets, {1} report entries{2}"),
      FText::AsNumber(InImported.WidgetCount), FText::AsNumber(InImported.Report.Num()),
      InImported.ApplyErrors.IsEmpty()
          ? FText::GetEmpty()
          : LOCTEXT("DesignImportedErrors", "; some properties couldn't be set"));
  const FText Name = FText::FromString(InImported.Blueprint->GetName());
  const FText Done =
      bInReplaced
          ? FText::Format(LOCTEXT("DesignReplaced",
                                  "Replaced {0} with a fresh import of the {1} design: {2}."),
                          Name, FText::FromString(SourceName(InSource)), Summary)
          : FText::Format(LOCTEXT("DesignImported",
                                  "Imported the {1} design as {0}: {2}."),
                          Name, FText::FromString(SourceName(InSource)), Summary);
  if (FUIWTClaudeService *Service = FUIWTClaudeService::TryGet())
  {
    Service->PostReply(InEntryId, Done.ToString());
  }
  Refresh();
  UIWTNotify::Show(FText::Format(LOCTEXT("DesignImportedToast",
                                         "{0} Details are in the Output Log."),
                                 Done),
                   true);
  if (bInAIPass)
  {
    RunAIPass(InEntryId, InImported.Blueprint, UIWTDesignRefine::FScope(), InImported.Report);
  }
}

void SUIWTChatSection::OnDesignReimported(const FGuid &InEntryId, UWidgetBlueprint *InBlueprint,
                                          const UIWTDesignImport::FReimportPlan &InPlan,
                                          bool bInAIPass)
{
  const FText Done = FText::Format(LOCTEXT("DesignReimported",
                                           "Merged the design into {0}: {1} design changes, "
                                           "{2} changes made in UE kept."),
                                   FText::FromString(InBlueprint->GetName()),
                                   FText::AsNumber(InPlan.Changes.Num()),
                                   FText::AsNumber(InPlan.KeptUEChanges));
  if (FUIWTClaudeService *Service = FUIWTClaudeService::TryGet())
  {
    Service->OnEntriesChanged().Broadcast();
    Service->PostReply(InEntryId, Done.ToString());
  }
  SelectEntry.ExecuteIfBound(InEntryId);
  Refresh();
  UIWTNotify::Show(Done, true);
  if (!bInAIPass)
  {
    return;
  }
  // The pass covers what this import changed.
  UIWTDesignImport::FSidecar Sidecar;
  FString Error;
  if (UIWTDesignImport::ReadSidecar(InBlueprint->GetOutermost()->GetName(), Sidecar, Error) &&
      !Sidecar.ChangedNodes.IsEmpty())
  {
    UIWTDesignRefine::FScope Scope;
    Scope.Nodes = Sidecar.ChangedNodes;
    Scope.bChangedParts = true;
    RunAIPass(InEntryId, InBlueprint, Scope, InPlan.Report);
  }
}

void SUIWTChatSection::RunAIPass(const FGuid &InEntryId, UWidgetBlueprint *InBlueprint,
                                 const UIWTDesignRefine::FScope &InScope,
                                 const TArray<UIWTDesignTree::FReportEntry> &InReport)
{
  if (!IsMcpConnected())
  {
    UIWTNotify::Show(LOCTEXT("AIPassNotConnected",
                             "The AI pass needs the MCP connection: Connect MCP first."),
                     false);
    return;
  }
  const UUIWTLocalSettings *Settings = UUIWTLocalSettings::Get();
  const int32 Rounds = FMath::Clamp(Settings->RefineRounds, 1, 10);
  UIWTDesignRefine::FPass Pass;
  FString Error;
  if (!UIWTDesignRefine::Prepare(InBlueprint, Rounds, InScope, InReport, Pass, Error))
  {
    UIWTNotify::Show(FText::Format(LOCTEXT("AIPassPrepareFailed", "AI pass not started: {0}"),
                                   FText::FromString(Error)),
                     false);
    return;
  }

  // The cost is shown before anything runs (import-tree.md → Model and
  // cost shown up front).
  const UIWTDesignRefine::EPrices Prices =
      UIWTDesignRefine::PricesFor(Settings->RefineModel, Settings->RefineCustomModel);
  const FText Scope =
      Pass.ScopeWidgets.IsEmpty()
          ? FText::Format(LOCTEXT("AIPassScopeWhole", "the whole blueprint, {0} design nodes"),
                          FText::AsNumber(Pass.NodeCount))
          : FText::Format(LOCTEXT("AIPassScopeParts",
                                  "{0} ({1}), {2} design nodes{3}; the rest of the blueprint "
                                  "stays as it is"),
                          FText::FromString(Pass.ScopeLabel),
                          FText::FromString(FString::Join(Pass.ScopeWidgets, TEXT(", "))),
                          FText::AsNumber(Pass.NodeCount),
                          Pass.Crop.Area() > 0
                              ? LOCTEXT("AIPassScopeCropped", ", the reference cropped to it")
                              : FText::GetEmpty());
  const FText Message = FText::Format(
      LOCTEXT("AIPassConfirm",
              "Refine {0} with Claude?\n\n"
              "Scope: {1}.\n"
              "Model: {2}, up to {3} rounds of apply, render and compare.\n\n"
              "Estimate (from the node count, not a measurement):\n{4}\n{5}\n"
              "Claude Code runs on your login: on a Pro or Max plan this comes out of the "
              "plan's usage limits instead. What the pass used is shown in the chat when it "
              "ends.\n\n"
              "No leaves the blueprint as it is."),
      FText::FromString(InBlueprint->GetName()), Scope,
      UIWTDesignImportDialog::AIPassModelName(), FText::AsNumber(Rounds),
      FText::FromString(EstimateLine(UIWTDesignRefine::Estimate(Pass.NodeCount, Rounds, Prices))),
      FText::FromString(
          EstimateLine(UIWTDesignRefine::Estimate(Pass.NodeCount, Rounds * 2, Prices))));
  if (FMessageDialog::Open(EAppMsgType::YesNo, Message,
                           LOCTEXT("AIPassConfirmTitle", "AI pass")) != EAppReturnType::Yes)
  {
    return;
  }

  FUIWTRefineStart Start;
  if (!Pass.AttachImage.IsEmpty())
  {
    // Without it, Claude still has the reference's path.
    FText ImageError;
    Start.Image = UIWTPromptImage::LoadFromFile(Pass.AttachImage, ImageError);
  }
  const TCHAR *SourceLabel = Pass.Source == TEXT("psd")     ? TEXT("Photoshop")
                             : Pass.Source == TEXT("image") ? TEXT("image")
                                                            : TEXT("Figma");
  Start.DisplayText = FString::Printf(
      TEXT("AI pass on %s (%s import): %d design nodes, up to %d rounds."), *Pass.ScopeLabel,
      SourceLabel, Pass.NodeCount, Rounds);
  Start.Request = MoveTemp(Pass.Request);
  Start.Source = Pass.Source;
  Start.Scope = Pass.ScopeLabel;
  Start.Nodes = Pass.NodeCount;
  Start.Rounds = Rounds;
  Start.bChangedParts = InScope.bChangedParts;
  const FUIWTRunContext Context =
      GetRunContext.IsBound() ? GetRunContext.Execute() : FUIWTRunContext();
  FText StartError;
  if (!FUIWTClaudeService::Get().StartDesignRefine(InEntryId, Start, Context, StartError))
  {
    UIWTNotify::Show(StartError, false);
  }
  Refresh();
}


void SUIWTChatSection::StartImageImport(const FUIWTDesignImportChoice &InChoice, bool bInAIPass,
                                        const FGuid &InEntryId, UWidgetBlueprint *InBlueprint,
                                        const FString &InBlueprintName)
{
  UIWTImageReader::FSource Source;
  FString Error;
  const UIWTImageReader::EScale Scale =
      static_cast<UIWTImageReader::EScale>(FMath::Clamp(InChoice.ImageScale, 0, 2));
  if (!UIWTImageReader::PrepareSource(InChoice.Source, Scale, Source, Error))
  {
    UIWTNotify::Show(FText::Format(LOCTEXT("ImagePrepareFailed", "Image import: {0}"),
                                   FText::FromString(Error)),
                     false);
    return;
  }

  // The cost first (import-image.md, section 5).
  const UUIWTLocalSettings *Settings = UUIWTLocalSettings::Get();
  const int32 Rounds = FMath::Clamp(Settings->RefineRounds, 1, 10);
  const int32 Nodes = UIWTDesignRefine::ExpectedNodes(Source.DesignSize);
  const UIWTDesignRefine::EPrices Prices =
      UIWTDesignRefine::PricesFor(Settings->RefineModel, Settings->RefineCustomModel);
  const FText Message = FText::Format(
      LOCTEXT("ImageConfirm",
              "Import {0}{11} with Claude?\n\n"
              "The image is {1} x {2} pixels: {3} x {4} design pixels. Claude reads it into a "
              "design tree (about {5} nodes expected), imports it and compares renders, up to "
              "{6} rounds{7}.\n"
              "Model: {8}.\n\n"
              "Estimate (from the image size, not a measurement):\n{9}\n{10}\n"
              "Claude Code runs on your login: on a Pro or Max plan this comes out of the "
              "plan's usage limits instead. What the run used is shown in the chat when it "
              "ends.\n\n"
              "No cancels; nothing is created."),
      FText::FromString(FPaths::GetCleanFilename(Source.SourceFile)),
      FText::AsNumber(Source.ImageSize.X), FText::AsNumber(Source.ImageSize.Y),
      FText::AsNumber(FMath::RoundToInt(Source.DesignSize.X)),
      FText::AsNumber(FMath::RoundToInt(Source.DesignSize.Y)), FText::AsNumber(Nodes),
      FText::AsNumber(Rounds),
      bInAIPass ? LOCTEXT("ImageConfirmPass", ", then refines the blueprint (AI pass)")
                : FText::GetEmpty(),
      UIWTDesignImportDialog::AIPassModelName(),
      FText::FromString(EstimateLine(
          UIWTDesignRefine::EstimateReading(Nodes, Rounds, Prices, bInAIPass))),
      FText::FromString(EstimateLine(
          UIWTDesignRefine::EstimateReading(Nodes, Rounds * 2, Prices, bInAIPass))),
      InBlueprint ? FText::Format(LOCTEXT("ImageConfirmInto", " into {0}"),
                                  FText::FromString(InBlueprint->GetName()))
                  : FText::GetEmpty());
  if (FMessageDialog::Open(EAppMsgType::YesNo, Message,
                           LOCTEXT("ImageConfirmTitle", "Image import")) != EAppReturnType::Yes)
  {
    IFileManager::Get().DeleteDirectory(*Source.CacheDir, false, true);
    return;
  }

  // The entry shows the run: the selected one, or a new one. Without a
  // blueprint, the first WriteDesignTree gives it one.
  UUIWidgetPreviewObjectManagerSettings *Entries = UUIWidgetPreviewObjectManagerSettings::Get();
  FWidgetPreviewObject *Entry = Entries->FindWidgetPreviewObject(InEntryId);
  const bool bNewEntry = !Entry;
  const FGuid EntryId =
      bNewEntry ? Entries->AddWidgetPreviewObject(TSoftClassPtr<UUserWidget>(), FString())
                : InEntryId;
  const FString OldNote = Entry ? Entry->Note : FString();
  Entry = Entries->FindWidgetPreviewObject(EntryId);
  if (Entry)
  {
    // The first write's import replaces the note; a merge doesn't.
    Entry->Note = FString::Printf(TEXT("%s the image %s."),
                                  InBlueprint ? TEXT("Imported from") : TEXT("Importing"),
                                  *FPaths::GetCleanFilename(Source.SourceFile));
  }
  Entries->SaveWidgetPreviewObjects();
  FUIWTClaudeService &Service = FUIWTClaudeService::Get();
  Service.OnEntriesChanged().Broadcast();
  SelectEntry.ExecuteIfBound(EntryId);

  FUIWTImageReadStart Start;
  FText ImageError;
  Start.Image = UIWTPromptImage::LoadFromFile(Source.CacheDir / TEXT("reference.png"), ImageError);
  Start.DisplayText = FString::Printf(
      TEXT("Import the image %s (%d x %d, %g image pixels per design pixel): up to %d "
           "rounds%s."),
      *FPaths::GetCleanFilename(Source.SourceFile), Source.ImageSize.X, Source.ImageSize.Y,
      Source.Scale, Rounds, bInAIPass ? TEXT(", then an AI pass") : TEXT(""));
  Start.Request = UIWTImageReader::BuildRequest(Source, Rounds, bInAIPass,
                                                InBlueprint ? InBlueprint->GetName() : FString());
  Start.CacheDir = Source.CacheDir;
  Start.SourceFile = Source.SourceFile;
  Start.Crc = Source.Crc;
  Start.TargetFolder = InChoice.TargetFolder;
  Start.BlueprintName = InBlueprintName;
  Start.Blueprint = InBlueprint;
  Start.Scale = Source.Scale;
  Start.Nodes = Nodes;
  Start.Rounds = Rounds;
  Start.bAIPass = bInAIPass;
  const FUIWTRunContext Context =
      GetRunContext.IsBound() ? GetRunContext.Execute() : FUIWTRunContext();
  FText StartError;
  if (!Service.StartImageRead(EntryId, Start, Context, StartError))
  {
    UIWTNotify::Show(StartError, false);
    if (bNewEntry)
    {
      Entries->RemoveWidgetPreviewObject(EntryId);
    }
    else if (FWidgetPreviewObject *Kept = Entries->FindWidgetPreviewObject(EntryId))
    {
      Kept->Note = OldNote;
    }
    Entries->SaveWidgetPreviewObjects();
    Service.OnEntriesChanged().Broadcast();
    IFileManager::Get().DeleteDirectory(*Source.CacheDir, false, true);
  }
  Refresh();
}

EVisibility SUIWTChatSection::GetCancelVisibility() const
{
  return IsRunInFlight() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SUIWTChatSection::GetSendVisibility() const
{
  return IsRunInFlight() ? EVisibility::Collapsed : EVisibility::Visible;
}

FText SUIWTChatSection::GetConnectText() const
{
  return IsMcpConnected() ? LOCTEXT("DisconnectBtn", "Disconnect MCP")
                          : LOCTEXT("ConnectBtn", "Connect MCP");
}

FText SUIWTChatSection::GetConnectToolTip() const
{
  return IsMcpConnected()
             ? LOCTEXT("DisconnectTip",
                       "Stop the editor's MCP server.")
             : LOCTEXT("ConnectTip",
                       "Start the editor's MCP server.");
}

FOptionalSize SUIWTChatSection::GetIconButtonSize() const
{
  // The Connect button sits before both icon buttons, so the layout has
  // already measured it this frame.
  const float Height =
      ConnectButton.IsValid() ? ConnectButton->GetDesiredSize().Y : 0.f;
  return Height > 0.f ? FOptionalSize(Height) : FOptionalSize();
}

#undef LOCTEXT_NAMESPACE
