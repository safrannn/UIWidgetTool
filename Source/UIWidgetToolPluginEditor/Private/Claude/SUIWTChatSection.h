#pragma once

#include "CoreMinimal.h"
#include "Design/UIWTDesignRefine.h"
#include "Framework/SlateDelegates.h"
#include "SUIWTDesignImportDialog.h"
#include "Types/SlateStructs.h"
#include "UIWTClaudeService.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SBox;
class SButton;
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
  struct FReimportPlan;
}

DECLARE_DELEGATE_RetVal(FUIWTRunContext, FOnUIWTGetRunContext);
DECLARE_DELEGATE_RetVal(FString, FOnUIWTPrepareImportEntry);
DECLARE_DELEGATE_OneParam(FOnUIWTSelectEntry, const FGuid &);

// The chat column of the manager's utility panel: dialogue history on top
// (user right-aligned, Claude left-aligned), prompt box below with "+"
// (the import menu: a Figma frame, a Photoshop export or an image to build
// a blueprint from, or local files to send with the prompt) and Send under
// it, and the connect / send / cancel logic against FUIWTClaudeService. It
// learns about the manager's selection only through the construct args, so
// the manager and the Claude code never include each other. An image for
// the prompt is pasted with Ctrl+V.
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
  // Called once per design import, before it starts: the owner
  // ends the selected entry's pending edit and returns the name typed for
  // its new widget, if any. A new entry's blueprint is left to the import.
  SLATE_EVENT(FOnUIWTPrepareImportEntry, PrepareImportEntry)
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
  FReply OnRemoveImageClicked();
  FReply OnRemoveFilesClicked();

  // --- The import menu.
  TSharedRef<SWidget> MakeImportMenu();
  // Figma frame, Photoshop export and image to blueprint, and files for the
  // prompt.
  TSharedRef<SWidget> MakeImportSources();
  // Asks for local files and adds them to the files for the next Send.
  void AttachFiles();
  TSharedRef<SWidget> MakeImportRow(const FText &InLabel, const FText &InToolTip,
                                    FOnClicked InOnClicked, TAttribute<bool> InEnabled);
  bool IsImportEnabled() const;
  // Asks where the design comes from and goes to and imports it into the
  // selected entry: into its widget, which keeps its name, or as the
  // blueprint of an entry without one (a new entry when none is selected).
  // With the source's AI pass on, starts the pass.
  void ImportDesign(EUIWTDesignSource InSource);
  // An image (import-image.md): shows the estimate and starts the reading run
  // on the entry InEntryId, or on a new entry when there is no such entry.
  // InBlueprint set: an earlier image import's blueprint, which the run
  // updates; otherwise its first write imports one named InBlueprintName
  // (empty: the default name).
  void StartImageImport(const FUIWTDesignImportChoice &InChoice, bool bInAIPass,
                        const FGuid &InEntryId, UWidgetBlueprint *InBlueprint,
                        const FString &InBlueprintName);
  // After a successful import, or a replace (bInReplaced): the entry,
  // selected, a reply in its chat saying what was imported, then the AI pass.
  void OnDesignImported(EUIWTDesignSource InSource, const UIWTDesignImport::FResult &InImported,
                        const FGuid &InEntryId, bool bInAIPass, bool bInReplaced);
  // After a successful merge into an entry's widget: the entry, selected, a
  // reply in its chat, then the AI pass on the parts the import changed.
  void OnDesignReimported(const FGuid &InEntryId, UWidgetBlueprint *InBlueprint,
                          const UIWTDesignImport::FReimportPlan &InPlan, bool bInAIPass);
  // The menu's AI pass rows for the selected entry, when a design import
  // made its blueprint: the picked widget's part and the parts the last
  // re-import changed.
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
  void SetPendingFiles(TArray<FString> InFiles);
  // The attachment slot: the pending image and the pending files.
  void RefreshAttachments();
  // Next to the import button: what the selected entry's blueprint was
  // imported from (the Figma frame's name, or the Photoshop document's or
  // the image file's name). Nothing when no design import made it.
  void RefreshImportSource();
  FReply OnCancelClicked();
  FReply OnConnectToggled();
  FReply OnSettingsClicked();

  EVisibility GetCancelVisibility() const;
  EVisibility GetSendVisibility() const;
  FText GetConnectText() const;
  FText GetConnectToolTip() const;
  // Side of the square icon buttons: the height of a regular text button
  // (the Connect button's). Unset until that button has been measured.
  FOptionalSize GetIconButtonSize() const;

  TSharedRef<SWidget> MakeMessageRow(const FUIWTChatMessage &InMessage) const;

  TSharedPtr<SScrollBox> History;
  TSharedPtr<SMultiLineEditableTextBox> PromptBox;
  FText PromptText;
  // Image and files (full paths) for the next Send, and the entry they were
  // picked for; dropped when the selection moves to another entry.
  TSharedPtr<const FUIWTPromptImage> PendingImage;
  TArray<FString> PendingFiles;
  FGuid PendingAttachmentEntryId;
  TSharedPtr<SBox> AttachmentSlot;
  TSharedPtr<SBox> ImportSourceSlot;
  // The sidecar the shown source came from, and its time stamp then.
  FString ImportSourceSidecar;
  FDateTime ImportSourceStamp;
  TSharedPtr<SComboButton> ImportButton;
  TSharedPtr<SButton> ConnectButton;
  FDelegateHandle ChatChangedHandle;
  // What the import dialog showed last, per source, for the next import.
  FUIWTDesignImportChoice LastFigmaChoice;
  FUIWTDesignImportChoice LastPsdChoice;
  FUIWTDesignImportChoice LastImageChoice;

  TAttribute<FGuid> SelectedEntryId;
  FOnUIWTGetRunContext GetRunContext;
  FOnUIWTPrepareImportEntry PrepareImportEntry;
  FOnUIWTSelectEntry SelectEntry;
  TAttribute<FString> PickedWidget;
};
