#include "SUIWTChatSection.h"

#include "Editor.h"
#include "InputCoreTypes.h"
#include "Styling/AppStyle.h"
#include "UIWTChatTypes.h"
#include "Core/UIWTNotify.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
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
  // "Two blank lines" between entries.
  constexpr float MessageGap = 24.f;
  constexpr float MessageMaxWidthFraction = 0.8f;
  constexpr float PromptBoxHeight = 72.f;
}

void SUIWTChatSection::Construct(const FArguments &InArgs)
{
  SelectedEntryId = InArgs._SelectedEntryId;
  CanDuplicate = InArgs._CanDuplicate;
  GetRunContext = InArgs._GetRunContext;
  OnDuplicate = InArgs._OnDuplicate;

  ChildSlot
      [SNew(SVerticalBox) +
       SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 4.f))
           [SNew(SHorizontalBox) +
            SHorizontalBox::Slot().FillWidth(1.f) +
            SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [SNew(SButton)
                     .Text(this, &SUIWTChatSection::GetConnectText)
                     .ToolTipText(this, &SUIWTChatSection::GetConnectToolTip)
                     .OnClicked(this, &SUIWTChatSection::OnConnectToggled)]] +
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
           .HAlign(HAlign_Right)
           .Padding(FMargin(0.f, SendButtonTopPadding, 0.f, 0.f))
               [SNew(SHorizontalBox) +
                SHorizontalBox::Slot().AutoWidth()
                    [SNew(SButton)
                         .Text(LOCTEXT("CancelRunBtn", "Cancel"))
                         .ToolTipText(LOCTEXT(
                             "CancelRunTip",
                             "Stop the run and restore the copy from disk."))
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

  TSharedRef<SWidget> Bubble =
      SNew(SBorder)
          .BorderImage(Brush)
          .Padding(FMargin(8.f, 6.f))
              [SNew(STextBlock)
                   .Text(FText::FromString(InMessage.Text))
                   .AutoWrapText(true)
                   .ColorAndOpacity(Color)];

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

bool SUIWTChatSection::IsSelectedEntryOriginal() const
{
  const FWidgetPreviewObject *PreviewObject = FindSelectedPreviewObject();
  return PreviewObject && PreviewObject->IsOriginal();
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
  if (PreviewObject->IsOriginal())
  {
    return LOCTEXT("PromptOriginal",
                   "Originals are never edited. Press Duplicate to make a "
                   "copy you can chat about.");
  }
  if (!IsMcpConnected())
  {
    return LOCTEXT("PromptNotConnected", "Press Connect MCP first.");
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
                 "Describe what you want changed, then Send (Ctrl+Enter).");
}

void SUIWTChatSection::OnPromptTextChanged(const FText &NewText)
{
  PromptText = NewText;
}

FReply SUIWTChatSection::OnPromptKeyDown(const FGeometry &,
                                         const FKeyEvent &KeyEvent)
{
  if (KeyEvent.GetKey() != EKeys::Enter || !KeyEvent.IsControlDown())
  {
    return FReply::Unhandled();
  }
  Submit();
  return FReply::Handled();
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
      PromptText.IsEmptyOrWhitespace())
  {
    return;
  }
  const FString Prompt = PromptText.ToString();
  PromptText = FText::GetEmpty();
  PromptBox->SetText(PromptText);

  const FUIWTRunContext Context = GetRunContext.IsBound()
                                      ? GetRunContext.Execute()
                                      : FUIWTRunContext();
  FText Error;
  if (!FUIWTClaudeService::Get().StartRun(PreviewObject->Id, Prompt, Context,
                                          Error))
  {
    UIWTNotify::Show(Error, false);
  }
  Refresh();
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

  // Connecting on an original is a request to start chatting, and originals
  // are never edited: duplicate and land on the copy. The manager selects
  // the copy and the owner refreshes us through its selection-changed path,
  // so with the server now up the prompt box and Send read as enabled on
  // the next tick.
  if (IsSelectedEntryOriginal() && CanDuplicate.Get(false) &&
      OnDuplicate.IsBound())
  {
    OnDuplicate.Execute();
  }
  return FReply::Handled();
}

EVisibility SUIWTChatSection::GetDuplicateVisibility() const
{
  return IsSelectedEntryOriginal() ? EVisibility::Visible
                                   : EVisibility::Collapsed;
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
                       "Stop the editor's MCP server. Runs cannot start "
                       "while disconnected.")
             : LOCTEXT("ConnectTip",
                       "Start the editor's MCP server on localhost and check "
                       "that Claude Code is installed. Required before Send. "
                       "On an original entry this also duplicates it and "
                       "selects the copy, since originals are never edited.");
}

#undef LOCTEXT_NAMESPACE
