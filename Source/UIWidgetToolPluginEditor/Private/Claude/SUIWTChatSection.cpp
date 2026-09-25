#include "SUIWTChatSection.h"

#include "Brushes/SlateDynamicImageBrush.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "EditorDirectories.h"
#include "Framework/Application/SlateApplication.h"
#include "ISettingsModule.h"
#include "InputCoreTypes.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "UIWTChatTypes.h"
#include "UIWTLocalSettings.h"
#include "UIWTPromptImage.h"
#include "Core/UIWTNotify.h"
#include "Host/UIWidgetToolPluginStyle.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
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

  ChildSlot
      [SNew(SVerticalBox) +
       SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 4.f))
           [SNew(SHorizontalBox) +
            SHorizontalBox::Slot().FillWidth(1.f) +
            SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [SNew(SButton)
                     .Text(this, &SUIWTChatSection::GetConnectText)
                     .ToolTipText(this, &SUIWTChatSection::GetConnectToolTip)
                     .OnClicked(this, &SUIWTChatSection::OnConnectToggled)] +
            SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(FMargin(4.f, 0.f, 0.f, 0.f))
                    [SNew(SButton)
                         .ToolTipText(LOCTEXT(
                             "SettingsTip",
                             "Open plugin settings."))
                         .OnClicked(this, &SUIWTChatSection::OnSettingsClicked)
                             [SNew(SImage)
                                  .Image(FUIWidgetToolPluginStyle::Get().GetBrush(
                                      "UIWidgetTool.Icons.Settings"))
                                  .ColorAndOpacity(
                                      FSlateColor::UseForeground())]]] +
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
                    [SNew(SButton)
                         .ToolTipText(LOCTEXT(
                             "AddImageTip",
                             "Attach an image to this prompt (PNG, JPEG or "
                             "BMP). Ctrl+V in the box above pastes one from "
                             "the clipboard."))
                         .IsEnabled(this, &SUIWTChatSection::IsPromptSubmitEnabled)
                         .OnClicked(this, &SUIWTChatSection::OnAddImageClicked)
                             [SNew(SImage)
                                  .Image(FUIWidgetToolPluginStyle::Get().GetBrush(
                                      "UIWidgetTool.Icons.AddImage"))
                                  .ColorAndOpacity(
                                      FSlateColor::UseForeground())]] +
                SHorizontalBox::Slot()
                    .FillWidth(1.f)
                    .HAlign(HAlign_Left)
                    .VAlign(VAlign_Center)
                    .Padding(FMargin(4.f, 0.f))
                        [SAssignNew(AttachmentSlot, SBox)] +
                SHorizontalBox::Slot().AutoWidth()
                    [SNew(SButton)
                         .Text(LOCTEXT("CancelRunBtn", "Cancel"))
                         .ToolTipText(LOCTEXT(
                             "CancelRunTip",
                             "Stop the run and restore the blueprint from disk."))
                         .Visibility(this, &SUIWTChatSection::GetCancelVisibility)
                         .OnClicked(this, &SUIWTChatSection::OnCancelClicked)] +
                SHorizontalBox::Slot().AutoWidth()
                    [SNew(SButton)
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
  if (PendingImage.IsValid() && PendingImageEntryId != EntryId)
  {
    SetPendingImage(nullptr);
  }

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
  if (!bHasImage || !InMessage.Text.IsEmpty())
  {
    Content->AddSlot()
        .AutoHeight()
        .Padding(FMargin(0.f, bHasImage ? 4.f : 0.f, 0.f, 0.f))
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
      (PromptText.IsEmptyOrWhitespace() && !PendingImage.IsValid()))
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
  if (FUIWTClaudeService::Get().StartRun(EntryId, Prompt,
                                         PendingImage, Context, Error))
  {
    // Cleared only once the run is under way, so a refused Send keeps what
    // the user typed.
    PromptText = FText::GetEmpty();
    PromptBox->SetText(PromptText);
    SetPendingImage(nullptr);
  }
  else
  {
    UIWTNotify::Show(Error, false);
  }
  Refresh();
}

FReply SUIWTChatSection::OnAddImageClicked()
{
  IDesktopPlatform *DesktopPlatform = FDesktopPlatformModule::Get();
  if (!DesktopPlatform)
  {
    return FReply::Handled();
  }

  TArray<FString> Files;
  const bool bPicked = DesktopPlatform->OpenFileDialog(
      FSlateApplication::Get().FindBestParentWindowHandleForDialogs(
          AsShared()),
      LOCTEXT("AddImageDialogTitle", "Attach an image").ToString(),
      FEditorDirectories::Get().GetLastDirectory(ELastDirectory::GENERIC_OPEN),
      FString(), UIWTPromptImage::GetFileTypes(), EFileDialogFlags::None,
      Files);
  if (!bPicked || Files.Num() == 0)
  {
    return FReply::Handled();
  }
  FEditorDirectories::Get().SetLastDirectory(ELastDirectory::GENERIC_OPEN,
                                             FPaths::GetPath(Files[0]));

  FText Error;
  AttachImage(UIWTPromptImage::LoadFromFile(Files[0], Error), Error);
  return FReply::Handled();
}

void SUIWTChatSection::AttachImage(TSharedPtr<const FUIWTPromptImage> InImage,
                                   const FText &InError)
{
  if (!InImage.IsValid())
  {
    UIWTNotify::Show(InError, false);
    return;
  }
  PendingImageEntryId = SelectedEntryId.Get(FGuid());
  SetPendingImage(MoveTemp(InImage));
}

FReply SUIWTChatSection::OnRemoveImageClicked()
{
  SetPendingImage(nullptr);
  return FReply::Handled();
}

void SUIWTChatSection::SetPendingImage(
    TSharedPtr<const FUIWTPromptImage> InImage)
{
  PendingImage = MoveTemp(InImage);
  if (!AttachmentSlot.IsValid())
  {
    return;
  }
  if (!PendingImage.IsValid())
  {
    AttachmentSlot->SetContent(SNullWidget::NullWidget);
    return;
  }

  const FText FileName = FText::FromString(PendingImage->FileName);
  AttachmentSlot->SetContent(
      SNew(SBorder)
          .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
          .Padding(FMargin(4.f, 2.f))
          .ToolTipText(FileName)
              [SNew(SHorizontalBox) +
               SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                   [MakeThumbnail(PendingImage.ToSharedRef(),
                                  AttachmentThumbnailHeight)] +
               SHorizontalBox::Slot()
                   .FillWidth(1.f)
                   .VAlign(VAlign_Center)
                   .Padding(FMargin(6.f, 0.f, 2.f, 0.f))
                       [SNew(STextBlock)
                            .Text(FileName)
                            .OverflowPolicy(ETextOverflowPolicy::Ellipsis)] +
               SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                   [SNew(SButton)
                        .ButtonStyle(FAppStyle::Get(), "SimpleButton")
                        .ContentPadding(FMargin(2.f))
                        .ToolTipText(LOCTEXT("RemoveImageTip",
                                             "Remove the attached image."))
                        .OnClicked(this,
                                   &SUIWTChatSection::OnRemoveImageClicked)
                            [SNew(SImage)
                                 .Image(FUIWidgetToolPluginStyle::Get().GetBrush(
                                     "UIWidgetTool.Icons.RemoveImage"))
                                 .ColorAndOpacity(
                                     FSlateColor::UseForeground())]]]);
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
  if (!Service.ConnectMcp(Error))
  {
    UIWTNotify::Show(Error, false);
    return FReply::Handled();
  }
  UIWTNotify::Show(
      LOCTEXT("McpConnected",
              "MCP server running on localhost. Claude Code found."),
      true);
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

#undef LOCTEXT_NAMESPACE
