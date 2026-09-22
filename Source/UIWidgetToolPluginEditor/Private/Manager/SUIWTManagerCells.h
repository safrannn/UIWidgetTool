#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SEditableTextBox;

class SUIWTCopyableCell : public SCompoundWidget
{
public:
  SLATE_BEGIN_ARGS(SUIWTCopyableCell) {}
  SLATE_ATTRIBUTE(FText, CopyText)
  SLATE_EVENT(FSimpleDelegate, OnRename)
  SLATE_ATTRIBUTE(bool, CanRename)
  SLATE_EVENT(FOnClicked, OnDuplicateEntry)
  SLATE_EVENT(FSimpleDelegate, OnDoubleClicked)
  SLATE_DEFAULT_SLOT(FArguments, Content)
  SLATE_END_ARGS()

  void Construct(const FArguments &InArgs);

  virtual FReply
  OnMouseButtonDoubleClick(const FGeometry &MyGeometry,
                           const FPointerEvent &MouseEvent) override;
  virtual FReply OnMouseButtonUp(const FGeometry &MyGeometry,
                                 const FPointerEvent &MouseEvent) override;

private:
  void CopyToClipboard() const;
  bool CanRenameNow() const { return CanRename.Get(); }

  TAttribute<FText> CopyText;
  FSimpleDelegate OnRename;
  TAttribute<bool> CanRename;
  FOnClicked OnDuplicateEntry;
  FSimpleDelegate OnDoubleClicked;
};

DECLARE_DELEGATE_TwoParams(FOnUIWTNameCommitted, FGuid,
                           const FText &);

class SUIWTNameEditCell : public SCompoundWidget
{
public:
  SLATE_BEGIN_ARGS(SUIWTNameEditCell) {}
  SLATE_EVENT(FOnUIWTNameCommitted, OnCommitted)
  SLATE_ATTRIBUTE(FText, HintText)
  SLATE_DEFAULT_SLOT(FArguments, Content)
  SLATE_END_ARGS()

  void Construct(const FArguments &InArgs);

  void BeginEdit(FGuid Id, const FString &CurrentName);

private:
  int32 GetWidgetIndex() const { return EditingId.IsValid() ? 1 : 0; }
  void EndEdit(bool bReleaseFocus);
  void OnTextCommitted(const FText &NewText, ETextCommit::Type CommitType);
  FReply OnEditKeyDown(const FGeometry &, const FKeyEvent &KeyEvent);

  FOnUIWTNameCommitted OnCommitted;
  TSharedPtr<SEditableTextBox> EditBox;
  FGuid EditingId;
  FString OriginalName;
};
