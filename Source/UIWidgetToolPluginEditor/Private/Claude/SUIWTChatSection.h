#pragma once

#include "CoreMinimal.h"
#include "UIWTClaudeService.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SBox;
class SMultiLineEditableTextBox;
class SScrollBox;
struct FUIWTChatMessage;
struct FUIWTPromptImage;
struct FWidgetPreviewObject;

DECLARE_DELEGATE_RetVal(FUIWTRunContext, FOnUIWTGetRunContext);

// The chat column of the manager's utility panel: dialogue history on top
// (user right-aligned, Claude left-aligned), prompt box below with "+"
// (attach an image) and Send under it, and the connect / send / cancel logic
// against FUIWTClaudeService. It learns
// about the manager's selection only through the construct args, so the
// manager and the Claude code never include each other.
class SUIWTChatSection : public SCompoundWidget
{
public:
  SLATE_BEGIN_ARGS(SUIWTChatSection) {}
  // The manager's selected entry; invalid when nothing is selected.
  SLATE_ATTRIBUTE(FGuid, SelectedEntryId)
  // Level, checkpoint and picked widget for the run about to start. Called
  // once per Send, never polled.
  SLATE_EVENT(FOnUIWTGetRunContext, GetRunContext)
  SLATE_END_ARGS()

  void Construct(const FArguments &InArgs);
  virtual ~SUIWTChatSection() override;

  // Rebuilds the history for the selected entry. The owner calls this when
  // the selection changes; chat changes are picked up internally.
  void Refresh();

private:
  const FWidgetPreviewObject *FindSelectedPreviewObject() const;
  static bool IsRunInFlight();
  static bool IsMcpConnected();

  // Empty when Send is allowed; otherwise the hint the prompt box shows.
  FText GetPromptDisabledReason() const;
  bool IsPromptSubmitEnabled() const;
  FText GetPromptHint() const;
  void OnPromptTextChanged(const FText &NewText);
  FReply OnPromptKeyDown(const FGeometry &, const FKeyEvent &KeyEvent);
  FReply OnSendClicked();
  void Submit();
  FReply OnAddImageClicked();
  FReply OnRemoveImageClicked();
  // Makes InImage the pending attachment, or shows InError when it is null.
  void AttachImage(TSharedPtr<const FUIWTPromptImage> InImage,
                   const FText &InError);
  void SetPendingImage(TSharedPtr<const FUIWTPromptImage> InImage);
  FReply OnCancelClicked();
  FReply OnConnectToggled();
  FReply OnSettingsClicked();

  EVisibility GetCancelVisibility() const;
  EVisibility GetSendVisibility() const;
  FText GetConnectText() const;
  FText GetConnectToolTip() const;

  TSharedRef<SWidget> MakeMessageRow(const FUIWTChatMessage &InMessage) const;

  TSharedPtr<SScrollBox> History;
  TSharedPtr<SMultiLineEditableTextBox> PromptBox;
  FText PromptText;
  // Image for the next Send, and the entry it was picked for; dropped when
  // the selection moves to another entry.
  TSharedPtr<const FUIWTPromptImage> PendingImage;
  FGuid PendingImageEntryId;
  TSharedPtr<SBox> AttachmentSlot;
  FDelegateHandle ChatChangedHandle;

  TAttribute<FGuid> SelectedEntryId;
  FOnUIWTGetRunContext GetRunContext;
};
