#pragma once

#include "CoreMinimal.h"
#include "UIWTClaudeService.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SMultiLineEditableTextBox;
class SScrollBox;
struct FUIWTChatMessage;
struct FWidgetPreviewObject;

DECLARE_DELEGATE_RetVal(FUIWTRunContext, FOnUIWTGetRunContext);

// The chat column of the manager's utility panel: dialogue history on top
// (user right-aligned, Claude left-aligned), prompt box and Send below, and
// the connect / send / cancel logic against FUIWTClaudeService. On an
// original entry the chat is disabled and a Duplicate button takes its place
// above the history. It learns about the manager's selection only through
// the construct args, so the manager and the Claude code never include each
// other.
class SUIWTChatSection : public SCompoundWidget
{
public:
  SLATE_BEGIN_ARGS(SUIWTChatSection) {}
  // The manager's selected entry; invalid when nothing is selected.
  SLATE_ATTRIBUTE(FGuid, SelectedEntryId)
  // Whether the manager would accept a Duplicate on that entry right now.
  SLATE_ATTRIBUTE(bool, CanDuplicate)
  // Level, checkpoint and picked widget for the run about to start. Called
  // once per Send, never polled.
  SLATE_EVENT(FOnUIWTGetRunContext, GetRunContext)
  // Also fired by Connect MCP on an original entry, so the user lands on a
  // copy whose chat is enabled instead of a disabled prompt box.
  SLATE_EVENT(FOnClicked, OnDuplicate)
  SLATE_END_ARGS()

  void Construct(const FArguments &InArgs);
  virtual ~SUIWTChatSection() override;

  // Rebuilds the history for the selected entry. The owner calls this when
  // the selection changes; chat changes are picked up internally.
  void Refresh();

private:
  const FWidgetPreviewObject *FindSelectedPreviewObject() const;
  bool IsSelectedEntryOriginal() const;
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
  FReply OnCancelClicked();
  FReply OnConnectToggled();

  EVisibility GetDuplicateVisibility() const;
  EVisibility GetCancelVisibility() const;
  EVisibility GetSendVisibility() const;
  FText GetConnectText() const;
  FText GetConnectToolTip() const;

  TSharedRef<SWidget> MakeMessageRow(const FUIWTChatMessage &InMessage) const;

  TSharedPtr<SScrollBox> History;
  TSharedPtr<SMultiLineEditableTextBox> PromptBox;
  FText PromptText;
  FDelegateHandle ChatChangedHandle;

  TAttribute<FGuid> SelectedEntryId;
  TAttribute<bool> CanDuplicate;
  FOnUIWTGetRunContext GetRunContext;
  FOnClicked OnDuplicate;
};
