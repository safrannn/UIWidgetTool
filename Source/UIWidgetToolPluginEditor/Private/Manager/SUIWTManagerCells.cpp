#include "SUIWTManagerCells.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"

#define LOCTEXT_NAMESPACE "UIWidgetTool"

void SUIWTCopyableCell::Construct(const FArguments &InArgs)
{
  CopyText = InArgs._CopyText;
  OnRename = InArgs._OnRename;
  CanRename = InArgs._CanRename.IsSet() ? InArgs._CanRename : true;
  OnDuplicateEntry = InArgs._OnDuplicateEntry;
  OnDoubleClicked = InArgs._OnDoubleClicked;
  ChildSlot[InArgs._Content.Widget];
}

FReply SUIWTCopyableCell::OnMouseButtonDoubleClick(
    const FGeometry &MyGeometry, const FPointerEvent &MouseEvent)
{
  if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton ||
      !OnDoubleClicked.IsBound())
  {
    return FReply::Unhandled();
  }
  OnDoubleClicked.Execute();
  return FReply::Handled();
}

FReply SUIWTCopyableCell::OnMouseButtonUp(const FGeometry &MyGeometry,
                                          const FPointerEvent &MouseEvent)
{
  if (MouseEvent.GetEffectingButton() != EKeys::RightMouseButton)
  {
    return FReply::Unhandled();
  }

  FMenuBuilder MenuBuilder(true, nullptr);
  MenuBuilder.AddMenuEntry(
      LOCTEXT("CopyCell", "Copy"),
      LOCTEXT("CopyCellTip", "Copy name to the clipboard."),
      FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
      FUIAction(
          FExecuteAction::CreateSP(this, &SUIWTCopyableCell::CopyToClipboard)));

  if (OnRename.IsBound())
  {
    MenuBuilder.AddMenuEntry(
        LOCTEXT("RenameCell", "Rename"),
        LOCTEXT("RenameCellTip", "Rename the checkpoint."),
        FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Rename"),
        // FSimpleDelegate and FExecuteAction are the same void() delegate.
        FUIAction(OnRename, FCanExecuteAction::CreateSP(
                                this, &SUIWTCopyableCell::CanRenameNow)));
  }

  if (OnDuplicateEntry.IsBound())
  {
    MenuBuilder.AddMenuEntry(
        LOCTEXT("DuplicateEntry", "Duplicate Entry"),
        LOCTEXT("DuplicateEntryTip", "Duplicate the entry."),
        FSlateIcon(FAppStyle::GetAppStyleSetName(),
                   "GenericCommands.Duplicate"),
        FUIAction(FExecuteAction::CreateSPLambda(
            this, [this] { OnDuplicateEntry.Execute(); })));
  }

  const FWidgetPath *EventPath = MouseEvent.GetEventPath();
  FSlateApplication::Get().PushMenu(
      SharedThis(this), EventPath ? *EventPath : FWidgetPath(),
      MenuBuilder.MakeWidget(), MouseEvent.GetScreenSpacePosition(),
      FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));

  return FReply::Handled();
}

void SUIWTCopyableCell::CopyToClipboard() const
{
  FPlatformApplicationMisc::ClipboardCopy(*CopyText.Get().ToString());
}

void SUIWTNameEditCell::Construct(const FArguments &InArgs)
{
  OnCommitted = InArgs._OnCommitted;
  ChildSlot
      [SNew(SWidgetSwitcher)
           .WidgetIndex(this, &SUIWTNameEditCell::GetWidgetIndex) +
       SWidgetSwitcher::Slot()[InArgs._Content.Widget] +
       SWidgetSwitcher::Slot()
           [SAssignNew(EditBox, SEditableTextBox)
                .SelectAllTextWhenFocused(true)
                .RevertTextOnEscape(true)
                .HintText(InArgs._HintText)
                .OnTextCommitted(this,
                                 &SUIWTNameEditCell::OnTextCommitted)
                .OnKeyDownHandler(this,
                                  &SUIWTNameEditCell::OnEditKeyDown)]];
}

void SUIWTNameEditCell::BeginEdit(FGuid Id,
                                  const FString &CurrentName)
{
  EditingId = Id;
  OriginalName = CurrentName;
  EditBox->SetText(FText::FromString(CurrentName));

  RegisterActiveTimer(
      0.f, FWidgetActiveTimerDelegate::CreateLambda(
               [this](double, float) -> EActiveTimerReturnType
               {
                 if (EditingId.IsValid())
                 {
                   FSlateApplication::Get().SetKeyboardFocus(
                       EditBox, EFocusCause::SetDirectly);
                 }
                 return EActiveTimerReturnType::Stop;
               }));
}

void SUIWTNameEditCell::EndEdit(bool bReleaseFocus)
{
  EditingId.Invalidate();
  if (bReleaseFocus && EditBox->HasKeyboardFocus())
  {
    FSlateApplication::Get().ClearKeyboardFocus(EFocusCause::SetDirectly);
  }
}

void SUIWTNameEditCell::OnTextCommitted(const FText &NewText,
                                              ETextCommit::Type CommitType)
{
  if (!EditingId.IsValid())
  {
    return;
  }

  const FGuid Id = EditingId;
  EndEdit(CommitType == ETextCommit::OnEnter);

  if (CommitType == ETextCommit::OnCleared ||
      NewText.ToString().TrimStartAndEnd() == OriginalName)
  {
    return;
  }
  OnCommitted.ExecuteIfBound(Id, NewText);
}

FReply SUIWTNameEditCell::OnEditKeyDown(const FGeometry &,
                                              const FKeyEvent &KeyEvent)
{
  if (KeyEvent.GetKey() == EKeys::Escape && EditingId.IsValid())
  {
    EndEdit(true);
    return FReply::Handled();
  }
  return FReply::Unhandled();
}

#undef LOCTEXT_NAMESPACE
