#pragma once

#include "CoreMinimal.h"
#include "UIWidgetPreviewObjectManagerSettings.h"
#include "Widgets/SCompoundWidget.h"

class UUserWidget;

/** Editor-only preview: hosts one widget instance in a tab, rebuilds it on the
 *  source blueprint's compile event. No PIE, no level load. */
class SUIWidgetPreview : public SCompoundWidget {
public:
  SLATE_BEGIN_ARGS(SUIWidgetPreview) {}
  SLATE_ARGUMENT(FGuid, EntryId)
  SLATE_END_ARGS()

  void Construct(const FArguments &InArgs);
  virtual ~SUIWidgetPreview();

private:
  FGuid WidgetPreviewObjectId;

  /** Keep the built widget alive while previewed. */
  TStrongObjectPtr<UUserWidget> PreviewWidget;

  TSharedPtr<class SBox> HostBox;
  TSharedPtr<class STextBlock> StatusText;

  /** Compile-hook handle for the current source blueprint. */
  FDelegateHandle CompiledHandle;
  TWeakObjectPtr<class UBlueprint> BoundBlueprint;

  void Rebuild();
  void BindCompileHook(UClass *ResolvedClass);
  void UnbindCompileHook();
  void OnSourceCompiled(class UBlueprint *Blueprint);

  FReply OnRefreshClicked();
};
