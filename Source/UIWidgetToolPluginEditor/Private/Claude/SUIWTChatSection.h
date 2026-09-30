#pragma once

#include "CoreMinimal.h"
#include "Design/UIWTDesignRefine.h"
#include "Framework/SlateDelegates.h"
#include "SUIWTDesignImportDialog.h"
#include "UIWTClaudeService.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SBox;
class SComboButton;
class SMultiLineEditableTextBox;
class SScrollBox;
class SVerticalBox;
class UWidgetBlueprint;
struct FUIWTChatMessage;
struct FUIWTPromptImage;
struct FWidgetPreviewObject;
namespace UIWTDesignImport
{
  struct FResult;
}

DECLARE_DELEGATE_RetVal(FUIWTRunContext, FOnUIWTGetRunContext);
DECLARE_DELEGATE_OneParam(FOnUIWTSelectEntry, const FGuid &);

// The chat column of the manager's utility panel: dialogue history on top
// (user right-aligned, Claude left-aligned), prompt box below with "+"
// (the import menu: an image for the prompt, a Figma frame or a Photoshop
// export) and Send under it, and the connect / send / cancel logic against
// FUIWTClaudeService. It learns about the manager's selection only through
// the construct args, so the manager and the Claude code never include each
// other.
class SUIWTChatSection : public SCompoundWidget
{
public:
  SLATE_BEGIN_ARGS(SUIWTChatSection) {}
  // The manager's selected entry; invalid when nothing is selected.
  SLATE_ATTRIBUTE(FGuid, SelectedEntryId)
  // Level, checkpoint and picked widget for the run about to start. Called
  // once per Send, never polled; the owner may confirm the entry's pending
  // edit first. An entry without a widget is fine: the run creates one.
  SLATE_EVENT(FOnUIWTGetRunContext, GetRunContext)
  // Selects an entry in the manager: the one a design import just added.
  SLATE_EVENT(FOnUIWTSelectEntry, SelectEntry)
  // The widget picked in the snapshot viewer, if any: the scope of an AI
  // pass on part of the selected entry's blueprint.
  SLATE_ATTRIBUTE(FString, PickedWidget)
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

  // --- The import menu.
  TSharedRef<SWidget> MakeImportMenu();
  // Image, Figma frame and Photoshop export.
  TSharedRef<SWidget> MakeImportSources();
  TSharedRef<SWidget> MakeImportRow(const FText &InLabel, const FText &InToolTip,
                                    FOnClicked InOnClicked, TAttribute<bool> InEnabled,
                                    TOptional<EUIWTDesignSource> InAIPassSource);
  bool IsImportEnabled() const;
  // Asks where the design comes from and goes to, imports it as a new
  // entry's blueprint and, with the source's AI pass on, starts the pass.
  void ImportDesign(EUIWTDesignSource InSource);
  // An image (import-image.md): shows the estimate, adds an entry with no
  // blueprint yet and starts the reading run on it.
  void StartImageImport(const FUIWTDesignImportChoice &InChoice, bool bInAIPass);
  // After a successful import: the new entry, selected, then the AI pass.
  void OnDesignImported(EUIWTDesignSource InSource, const UIWTDesignImport::FResult &InImported,
                        const FGuid &InEntryId, bool bInAIPass);
  // The menu's AI pass rows for the selected entry, when a design import
  // made its blueprint: the whole blueprint, the picked widget's part, and
  // the parts the last re-import changed.
  void AddEntryAIPassRows(const TSharedRef<SVerticalBox> &InMenu);
  // The selected entry's blueprint when a design import made it; null with
  // OutWhyNot set otherwise.
  UWidgetBlueprint *FindSelectedDesignBlueprint(FString &OutWhyNot) const;
  // Shows the estimate and, when the user agrees, starts the pass on the
  // entry's blueprint. InReport empty: the sidecar's report.
  void RunAIPass(const FGuid &InEntryId, UWidgetBlueprint *InBlueprint,
                 const UIWTDesignRefine::FScope &InScope,
                 const TArray<UIWTDesignTree::FReportEntry> &InReport);
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
  TSharedPtr<SComboButton> ImportButton;
  FDelegateHandle ChatChangedHandle;
  // What the import dialog showed last, per source, for the next import.
  FUIWTDesignImportChoice LastFigmaChoice;
  FUIWTDesignImportChoice LastPsdChoice;
  FUIWTDesignImportChoice LastImageChoice;

  TAttribute<FGuid> SelectedEntryId;
  FOnUIWTGetRunContext GetRunContext;
  FOnUIWTSelectEntry SelectEntry;
  TAttribute<FString> PickedWidget;
};
