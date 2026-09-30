#include "SUIWTChatSection.h"

#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Core/UIWTDesignImport.h"
#include "Core/UIWTGeneratedBlueprints.h"
#include "Design/UIWTDesignRefine.h"
#include "Design/UIWTDesignSources.h"
#include "Design/UIWTImageReader.h"
#include "HAL/FileManager.h"
#include "DesktopPlatformModule.h"
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
#include "Widgets/Input/SCheckBox.h"
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
  SelectEntry = InArgs._SelectEntry;
  PickedWidget = InArgs._PickedWidget;

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
                    [SAssignNew(ImportButton, SComboButton)
                         .HasDownArrow(false)
                         .ToolTipText(LOCTEXT(
                             "ImportTip",
                             "Import: an image for this prompt, a Figma frame "
                             "or a Photoshop export."))
                         .IsEnabled(this, &SUIWTChatSection::IsImportEnabled)
                         .OnGetMenuContent(this, &SUIWTChatSection::MakeImportMenu)
                         .ButtonContent()
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

  // Imports the design and adds a manager entry for the new blueprint.
  // False with OutError set when nothing was imported.
  bool ImportAsEntry(const UIWTDesignImport::FRequest &InRequest, EUIWTDesignSource InSource,
                     UIWTDesignImport::FResult &OutImported, FGuid &OutEntryId,
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
    OutEntryId = Settings->AddWidgetPreviewObject(
        TSoftClassPtr<UUserWidget>(OutImported.Blueprint->GeneratedClass),
        OutImported.Blueprint->GetName());
    if (FWidgetPreviewObject *Entry = Settings->FindWidgetPreviewObject(OutEntryId))
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
               LOCTEXT("ImportImage", "Image..."),
               LOCTEXT("AddImageTip",
                       "Attach an image to this prompt (PNG, JPEG or BMP). Ctrl+V in "
                       "the prompt box pastes one from the clipboard."),
               FOnClicked::CreateSP(this, &SUIWTChatSection::OnAddImageClicked),
               TAttribute<bool>::CreateSP(this, &SUIWTChatSection::IsPromptSubmitEnabled),
               TOptional<EUIWTDesignSource>())] +
       SVerticalBox::Slot().AutoHeight()
           [MakeImportRow(
               LOCTEXT("ImportFigma", "Figma frame..."),
               LOCTEXT("ImportFigmaTip",
                       "Import a Figma frame as a new Widget Blueprint and manager "
                       "entry. Needs a Figma token (FIGMA_TOKEN, or UI Widget Tool "
                       "(local) settings)."),
               FOnClicked::CreateSPLambda(this,
                                          [this]
                                          {
                                            ImportDesign(EUIWTDesignSource::Figma);
                                            return FReply::Handled();
                                          }),
               true, EUIWTDesignSource::Figma)] +
       SVerticalBox::Slot().AutoHeight()
           [MakeImportRow(
               LOCTEXT("ImportPsd", "Photoshop export..."),
               LOCTEXT("ImportPsdTip",
                       "Import a Photoshop export (Export for Unreal in the UIWidgetTool "
                       "Bridge panel, Tools/PhotoshopBridge) as a new Widget Blueprint "
                       "and manager entry."),
               FOnClicked::CreateSPLambda(this,
                                          [this]
                                          {
                                            ImportDesign(EUIWTDesignSource::Photoshop);
                                            return FReply::Handled();
                                          }),
               true, EUIWTDesignSource::Photoshop)] +
       SVerticalBox::Slot().AutoHeight()
           [MakeImportRow(
               LOCTEXT("ImportImageBlueprint", "Image to blueprint..."),
               LOCTEXT("ImportImageBlueprintTip",
                       "Build a new Widget Blueprint and manager entry from a mockup or "
                       "screenshot: Claude reads the image into a design tree, which is "
                       "imported like a Figma frame and refined against renders. Uses "
                       "tokens; the estimate is shown first. Needs the MCP connection."),
               FOnClicked::CreateSPLambda(this,
                                          [this]
                                          {
                                            ImportDesign(EUIWTDesignSource::Image);
                                            return FReply::Handled();
                                          }),
               true, EUIWTDesignSource::Image)];
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
  InMenu->AddSlot().AutoHeight().Padding(FMargin(8.f, 6.f, 8.f, 2.f))
      [SNew(STextBlock)
           .Text(LOCTEXT("EntryAIPassHeading", "AI pass on the selected entry"))
           .Font(FAppStyle::GetFontStyle("SmallFont"))
           .ColorAndOpacity(FSlateColor::UseSubduedForeground())];

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
  const FGuid EntryId = SelectedEntryId.Get(FGuid());
  const TWeakObjectPtr<UWidgetBlueprint> WeakBlueprint(Blueprint);
  auto Enabled = [this, bUsable = Blueprint != nullptr]
  { return bUsable && IsPromptSubmitEnabled(); };
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
  const FText Unusable = FText::FromString(WhyNot);

  InMenu->AddSlot().AutoHeight()
      [MakeImportRow(LOCTEXT("EntryAIPassWhole", "Whole blueprint..."),
                     Blueprint ? LOCTEXT("EntryAIPassWholeTip",
                                         "Refine the selected entry's blueprint with Claude: "
                                         "buttons, lists, layout boxes, anchors, names and "
                                         "variables. The estimate is shown first.")
                               : Unusable,
                     Start(UIWTDesignRefine::FScope()),
                     TAttribute<bool>::CreateLambda(Enabled), TOptional<EUIWTDesignSource>())];
  if (!Blueprint)
  {
    return;
  }

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
                                "The picked widget is the design's root: use Whole "
                                "blueprint."),
            Start(Scope),
            TAttribute<bool>::CreateLambda([Enabled, bPickedPart] { return bPickedPart && Enabled(); }),
            TOptional<EUIWTDesignSource>())];
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
                       Start(Scope), TAttribute<bool>::CreateLambda(Enabled),
                       TOptional<EUIWTDesignSource>())];
  }
}

TSharedRef<SWidget> SUIWTChatSection::MakeImportRow(const FText &InLabel,
                                                    const FText &InToolTip,
                                                    FOnClicked InOnClicked,
                                                    TAttribute<bool> InEnabled,
                                                    TOptional<EUIWTDesignSource> InAIPassSource)
{
  TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
  Row->AddSlot().FillWidth(1.f)
      [SNew(SButton)
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
               })[SNew(STextBlock).Text(InLabel)]];
  if (InAIPassSource.IsSet())
  {
    const EUIWTDesignSource Source = InAIPassSource.GetValue();
    Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(6.f, 0.f, 2.f, 0.f))
        [SNew(SCheckBox)
             .Style(&FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("ToggleButtonCheckbox"))
             .ToolTipText_Static(&UIWTDesignImportDialog::DescribeAIPass)
             .IsChecked_Lambda(
                 [Source]
                 {
                   return UIWTDesignImportDialog::IsAIPassOn(Source) ? ECheckBoxState::Checked
                                                                     : ECheckBoxState::Unchecked;
                 })
             .OnCheckStateChanged_Lambda(
                 [Source](ECheckBoxState InState)
                 { UIWTDesignImportDialog::SetAIPass(Source, InState == ECheckBoxState::Checked); })
                 [SNew(SBox).Padding(FMargin(6.f, 1.f))
                      [SNew(STextBlock).Text(LOCTEXT("AIPassToggle", "AI pass"))]]];
  }
  return Row;
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
  if (InSource == EUIWTDesignSource::Image)
  {
    StartImageImport(Choice, bAIPass);
    return;
  }
  if (bAIPass && !IsMcpConnected())
  {
    UIWTNotify::Show(LOCTEXT("AIPassNeedsMcp",
                             "The AI pass needs the MCP connection: Connect MCP first, or "
                             "turn the AI pass off in the + menu."),
                     false);
    return;
  }

  UIWTDesignImport::FRequest Request;
  Request.TargetFolder = Choice.TargetFolder;
  Request.BlueprintName = Choice.BlueprintName;
  const TWeakPtr<SWidget> WeakThis = AsShared();
  auto Finish = [WeakThis, InSource, bAIPass](const UIWTDesignImport::FRequest &InRequest,
                                              const FString &InError)
  {
    FString Error = InError;
    UIWTDesignImport::FResult Imported;
    FGuid EntryId;
    if (Error.IsEmpty())
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
                                                                      EntryId, bAIPass);
    }
  };

  if (InSource == EUIWTDesignSource::Figma)
  {
    UIWTNotify::Show(LOCTEXT("FetchingFigma", "Fetching the Figma frame..."), true);
    UIWTFigmaClient::Fetch(Choice.Source, false,
                           [Request, Finish](const UIWTFigmaClient::FFetchResult &InFetched) mutable
                           {
                             Request.DesignFile = InFetched.DesignFile;
                             Request.ReferenceImage = InFetched.ReferenceImage;
                             Finish(Request, InFetched.Error);
                           });
    return;
  }
  UIWTDesignSources::FPsdExport Export;
  FString Error;
  if (UIWTDesignSources::PreparePsd(Choice.Source, Export, Error))
  {
    Request.DesignFile = Export.DesignFile;
    Request.ReferenceImage = Export.ReferenceImage;
  }
  Finish(Request, Error);
}

void SUIWTChatSection::OnDesignImported(EUIWTDesignSource InSource,
                                        const UIWTDesignImport::FResult &InImported,
                                        const FGuid &InEntryId, bool bInAIPass)
{
  SelectEntry.ExecuteIfBound(InEntryId);
  Refresh();
  UIWTNotify::Show(
      FText::Format(LOCTEXT("DesignImported",
                            "Imported {0} as a new entry: {1} widgets, {2} report entries{3}. "
                            "Details are in the Output Log."),
                    FText::FromString(InImported.Blueprint->GetName()),
                    FText::AsNumber(InImported.WidgetCount),
                    FText::AsNumber(InImported.Report.Num()),
                    InImported.ApplyErrors.IsEmpty()
                        ? FText::GetEmpty()
                        : LOCTEXT("DesignImportedErrors",
                                  "; some properties couldn't be set")),
      true);
  if (bInAIPass)
  {
    RunAIPass(InEntryId, InImported.Blueprint, UIWTDesignRefine::FScope(), InImported.Report);
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


void SUIWTChatSection::StartImageImport(const FUIWTDesignImportChoice &InChoice, bool bInAIPass)
{
  if (!IsMcpConnected())
  {
    UIWTNotify::Show(LOCTEXT("ImageNeedsMcp",
                             "An image import is a Claude run: Connect MCP first."),
                     false);
    return;
  }
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
              "Import {0} with Claude?\n\n"
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
          UIWTDesignRefine::EstimateReading(Nodes, Rounds * 2, Prices, bInAIPass))));
  if (FMessageDialog::Open(EAppMsgType::YesNo, Message,
                           LOCTEXT("ImageConfirmTitle", "Image import")) != EAppReturnType::Yes)
  {
    IFileManager::Get().DeleteDirectory(*Source.CacheDir, false, true);
    return;
  }

  // The entry shows the run; the first WriteDesignTree gives it its
  // blueprint.
  UUIWidgetPreviewObjectManagerSettings *Entries = UUIWidgetPreviewObjectManagerSettings::Get();
  const FGuid EntryId = Entries->AddWidgetPreviewObject(TSoftClassPtr<UUserWidget>(), FString());
  if (FWidgetPreviewObject *Entry = Entries->FindWidgetPreviewObject(EntryId))
  {
    Entry->Note = FString::Printf(TEXT("Importing the image %s."),
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
  Start.Request = UIWTImageReader::BuildRequest(Source, Rounds, bInAIPass);
  Start.CacheDir = Source.CacheDir;
  Start.SourceFile = Source.SourceFile;
  Start.Crc = Source.Crc;
  Start.TargetFolder = InChoice.TargetFolder;
  Start.BlueprintName = InChoice.BlueprintName;
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
    Entries->RemoveWidgetPreviewObject(EntryId);
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

#undef LOCTEXT_NAMESPACE
