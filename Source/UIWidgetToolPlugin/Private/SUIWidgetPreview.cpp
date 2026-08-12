#include "SUIWidgetPreview.h"
#include "Blueprint/UserWidget.h"
#include "Engine/Blueprint.h"
#include "UIWidgetReflection.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UIWidgetTool"

void SUIWidgetPreview::Construct(const FArguments &InArgs) {
  WidgetPreviewObjectId = InArgs._EntryId;

  ChildSlot[SNew(SVerticalBox)

            // Toolbar row: manual refresh + status.
            +
            SVerticalBox::Slot().AutoHeight().Padding(
                4.f)[SNew(SHorizontalBox) +
                     SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                         [SNew(SButton)
                              .Text(LOCTEXT("Refresh", "Refresh"))
                              .OnClicked(this,
                                         &SUIWidgetPreview::OnRefreshClicked)] +
                     SHorizontalBox::Slot().FillWidth(1.f).VAlign(
                         VAlign_Center)[SAssignNew(StatusText, STextBlock)]]

            // The live widget goes here.
            + SVerticalBox::Slot().FillHeight(1.f)[SAssignNew(HostBox, SBox)]];

  Rebuild();
}

SUIWidgetPreview::~SUIWidgetPreview() { UnbindCompileHook(); }

void SUIWidgetPreview::Rebuild() {
  // Tear down the previous instance first.
  PreviewWidget.Reset();
  if (HostBox.IsValid()) {
    HostBox->SetContent(SNullWidget::NullWidget);
  }

  UUIWidgetPreviewObjectManagerSettings *Settings =
      UUIWidgetPreviewObjectManagerSettings::Get();
  FWidgetPreviewObject *WidgetPreviewObject =
      Settings ? Settings->FindWidgetPreviewObject(WidgetPreviewObjectId)
               : nullptr;
  if (!WidgetPreviewObject) {
    if (StatusText.IsValid()) {
      StatusText->SetText(LOCTEXT("WidgetPreviewObjectGone",
                                  "WidgetPreviewObject no longer exists."));
    }
    return;
  }

  // Resolve the class fresh every rebuild — reinstancing invalidates old
  // pointers.
  UClass *WidgetClass = WidgetPreviewObject->WidgetClass.LoadSynchronous();
  if (!WidgetClass) {
    if (StatusText.IsValid()) {
      StatusText->SetText(LOCTEXT(
          "ClassInvalid", "Widget asset is missing or failed to load."));
    }
    return;
  }

  // (Re)bind the compile hook to the freshly resolved class's blueprint.
  BindCompileHook(WidgetClass);

  // Build via a transient preview world's game instance is overkill for an
  // editor preview; CreateWidget with the editor world is sufficient here.
  UWorld *World = GWorld;
  UUserWidget *NewWidget = CreateWidget<UUserWidget>(World, WidgetClass);
  if (!NewWidget) {
    if (StatusText.IsValid()) {
      StatusText->SetText(LOCTEXT("CreateFailed", "CreateWidget failed."));
    }
    return;
  }

  // Empty overrides leave CDO values in place; non-empty get ImportText'd.
  const TArray<FName> Failed = UIWidgetReflection::ApplyOverrides(
      NewWidget, WidgetPreviewObject->Overrides);

  PreviewWidget = TStrongObjectPtr<UUserWidget>(NewWidget);

  if (HostBox.IsValid()) {
    HostBox->SetContent(NewWidget->TakeWidget());
  }

  if (StatusText.IsValid()) {
    StatusText->SetText(
        Failed.Num() == 0
            ? FText::Format(LOCTEXT("OkFmt", "Previewing {0}"),
                            FText::FromString(WidgetPreviewObject->LevelBookmark))
            : FText::Format(LOCTEXT("PartialFmt",
                                    "Previewing {0} ({1} override(s) failed)"),
                            FText::FromString(WidgetPreviewObject->LevelBookmark),
                            FText::AsNumber(Failed.Num())));
  }
}

void SUIWidgetPreview::BindCompileHook(UClass *ResolvedClass) {
  UBlueprint *Blueprint =
      ResolvedClass ? Cast<UBlueprint>(ResolvedClass->ClassGeneratedBy)
                    : nullptr;

  // Already bound to the same blueprint? Leave it.
  if (BoundBlueprint.Get() == Blueprint) {
    return;
  }

  UnbindCompileHook();

  if (Blueprint) {
    CompiledHandle = Blueprint->OnCompiled().AddSP(
        this, &SUIWidgetPreview::OnSourceCompiled);
    BoundBlueprint = Blueprint;
  }
}

void SUIWidgetPreview::UnbindCompileHook() {
  if (UBlueprint *Blueprint = BoundBlueprint.Get()) {
    Blueprint->OnCompiled().Remove(CompiledHandle);
  }
  CompiledHandle.Reset();
  BoundBlueprint.Reset();
}

void SUIWidgetPreview::OnSourceCompiled(UBlueprint * /*Blueprint*/) {
  // Class/CDO were reinstanced — rebuild from scratch.
  Rebuild();
}

FReply SUIWidgetPreview::OnRefreshClicked() {
  Rebuild();
  return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
