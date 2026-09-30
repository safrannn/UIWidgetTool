#include "SUIWTDesignImportDialog.h"

#include "Core/UIWTDesignSettings.h"
#include "Core/UIWTLocalSettings.h"
#include "DesktopPlatformModule.h"
#include "EditorDirectories.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "UIWTPromptImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UIWidgetTool"

namespace
{
  constexpr float DialogWidth = 520.f;

  class SUIWTDesignImportDialog : public SCompoundWidget
  {
  public:
    SLATE_BEGIN_ARGS(SUIWTDesignImportDialog) {}
    SLATE_ARGUMENT(EUIWTDesignSource, Source)
    SLATE_ARGUMENT(FUIWTDesignImportChoice, Choice)
    SLATE_ARGUMENT(TWeakPtr<SWindow>, Window)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs)
    {
      Source = InArgs._Source;
      Choice = InArgs._Choice;
      Window = InArgs._Window;
      const bool bFigma = Source == EUIWTDesignSource::Figma;
      const bool bImage = Source == EUIWTDesignSource::Image;

      TSharedRef<SHorizontalBox> SourceRow = SNew(SHorizontalBox);
      SourceRow->AddSlot().FillWidth(1.f)
          [SAssignNew(SourceBox, SEditableTextBox)
               .Text(FText::FromString(Choice.Source))
               .HintText(bFigma   ? LOCTEXT("FigmaLinkHint",
                                            "https://www.figma.com/design/<file>/...?node-id=...")
                         : bImage ? LOCTEXT("ImageFileHint", "<folder>/<mockup>.png")
                                  : LOCTEXT("PsdManifestHint",
                                            "<folder>/<document>.uiwt-psd/manifest.json"))];
      // Images: how their pixels map to design pixels.
      TSharedRef<SHorizontalBox> ScaleRow = SNew(SHorizontalBox);
      const FText ScaleNames[] = {LOCTEXT("Scale1x", "1x"), LOCTEXT("Scale2x", "2x"),
                                  FText::Format(LOCTEXT("ScaleFit", "Fit to {0} wide"),
                                                FText::AsNumber(UUIWTDesignSettings::Get()
                                                                    ->ReferenceResolution.X))};
      for (int32 Index = 0; Index < 3; ++Index)
      {
        ScaleRow->AddSlot().AutoWidth().Padding(FMargin(0.f, 0.f, 14.f, 0.f))
            [SNew(SCheckBox)
                 .Style(FAppStyle::Get(), "RadioButton")
                 .IsChecked_Lambda([this, Index]
                                   { return Choice.ImageScale == Index ? ECheckBoxState::Checked
                                                                       : ECheckBoxState::Unchecked; })
                 .OnCheckStateChanged_Lambda([this, Index](ECheckBoxState)
                                             { Choice.ImageScale = Index; })
                     [SNew(STextBlock).Text(ScaleNames[Index])]];
      }
      if (!bFigma)
      {
        SourceRow->AddSlot().AutoWidth().Padding(FMargin(4.f, 0.f, 0.f, 0.f))
            [SNew(SButton)
                 .Text(LOCTEXT("BrowseBtn", "Browse..."))
                 .OnClicked(this, &SUIWTDesignImportDialog::OnBrowse)];
      }

      ChildSlot
          [SNew(SBox).WidthOverride(DialogWidth).Padding(FMargin(12.f))
               [SNew(SVerticalBox) +
                SVerticalBox::Slot().AutoHeight()
                    [SNew(STextBlock)
                         .Text(bFigma   ? LOCTEXT("FigmaSourceLabel",
                                                  "Figma frame link (right-click the frame > "
                                                  "Copy link to selection)")
                               : bImage ? LOCTEXT("ImageSourceLabel",
                                                  "Image (PNG, JPEG or BMP): a mockup or a "
                                                  "screenshot. Claude reads it into a design "
                                                  "tree, which is imported like a Figma frame.")
                                        : LOCTEXT("PsdSourceLabel",
                                                  "manifest.json of a Photoshop export (Export "
                                                  "for Unreal in the UIWidgetTool Bridge panel)"))
                         .AutoWrapText(true)] +
                SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 4.f, 0.f, 10.f))
                    [SourceRow] +
                SVerticalBox::Slot().AutoHeight()
                    [SNew(STextBlock)
                         .Visibility(bImage ? EVisibility::Visible : EVisibility::Collapsed)
                         .Text(LOCTEXT("ScaleLabel", "Image pixels per design pixel"))] +
                SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 4.f, 0.f, 10.f))
                    [SNew(SBox)
                         .Visibility(bImage ? EVisibility::Visible : EVisibility::Collapsed)
                             [ScaleRow]] +
                SVerticalBox::Slot().AutoHeight()
                    [SNew(STextBlock).Text(LOCTEXT("FolderLabel", "Content folder"))] +
                SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 4.f, 0.f, 10.f))
                    [SAssignNew(FolderBox, SEditableTextBox)
                         .Text(FText::FromString(Choice.TargetFolder))
                         .HintText(FText::Format(
                             LOCTEXT("FolderHint", "{0} (the default)"),
                             FText::FromString(UUIWTDesignSettings::Get()->GetImportFolder())))] +
                SVerticalBox::Slot().AutoHeight()
                    [SNew(STextBlock).Text(LOCTEXT("NameLabel", "Blueprint name"))] +
                SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 4.f, 0.f, 10.f))
                    [SAssignNew(NameBox, SEditableTextBox)
                         .Text(FText::FromString(Choice.BlueprintName))
                         .HintText(bFigma   ? LOCTEXT("FigmaNameHint", "WBP_<frame name>")
                                   : bImage ? LOCTEXT("ImageNameHint",
                                                      "WBP_<the name Claude gives the screen>")
                                            : LOCTEXT("PsdNameHint",
                                                      "WBP_<artboard or document name>"))] +
                SVerticalBox::Slot().AutoHeight()
                    [SNew(SCheckBox)
                         .IsChecked_Lambda(
                             [this]
                             {
                               return UIWTDesignImportDialog::IsAIPassOn(Source)
                                          ? ECheckBoxState::Checked
                                          : ECheckBoxState::Unchecked;
                             })
                         .OnCheckStateChanged_Lambda(
                             [this](ECheckBoxState InState)
                             {
                               UIWTDesignImportDialog::SetAIPass(
                                   Source, InState == ECheckBoxState::Checked);
                             })
                         .ToolTipText(UIWTDesignImportDialog::DescribeAIPass())
                             [SNew(STextBlock)
                                  .Text(bImage ? LOCTEXT("AIPassCheckImage",
                                                         "AI pass: Claude also refines the "
                                                         "blueprint in the same run")
                                               : LOCTEXT("AIPassCheck",
                                                         "AI pass: refine the import with "
                                                         "Claude (an estimate is shown "
                                                         "first)"))]] +
                SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 14.f, 0.f, 0.f))
                    [SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(1.f) +
                     SHorizontalBox::Slot().AutoWidth()
                         [SNew(SButton)
                              .Text(LOCTEXT("ImportBtn", "Import"))
                              .ButtonStyle(FAppStyle::Get(), "PrimaryButton")
                              .IsEnabled(this, &SUIWTDesignImportDialog::CanImport)
                              .OnClicked(this, &SUIWTDesignImportDialog::OnImport)] +
                     SHorizontalBox::Slot().AutoWidth().Padding(FMargin(6.f, 0.f, 0.f, 0.f))
                         [SNew(SButton)
                              .Text(LOCTEXT("CancelBtn", "Cancel"))
                              .OnClicked(this, &SUIWTDesignImportDialog::OnCancel)]]]];
    }

    bool IsConfirmed() const { return bConfirmed; }
    const FUIWTDesignImportChoice &GetChoice() const { return Choice; }

  private:
    FString SourceText() const
    {
      return SourceBox.IsValid() ? SourceBox->GetText().ToString().TrimStartAndEnd()
                                 : FString();
    }

    bool CanImport() const
    {
      const FString Text = SourceText();
      const FString Extension = FPaths::GetExtension(Text).ToLower();
      switch (Source)
      {
      case EUIWTDesignSource::Figma:
        return Text.Contains(TEXT("figma.com/"));
      case EUIWTDesignSource::Image:
        return Extension == TEXT("png") || Extension == TEXT("jpg") ||
               Extension == TEXT("jpeg") || Extension == TEXT("bmp");
      default:
        return Extension == TEXT("json");
      }
    }

    FReply OnBrowse()
    {
      IDesktopPlatform *DesktopPlatform = FDesktopPlatformModule::Get();
      if (!DesktopPlatform)
      {
        return FReply::Handled();
      }
      TArray<FString> Files;
      const FString Start = SourceText().IsEmpty()
                                ? FEditorDirectories::Get().GetLastDirectory(
                                      ELastDirectory::GENERIC_OPEN)
                                : FPaths::GetPath(SourceText());
      const bool bImage = Source == EUIWTDesignSource::Image;
      if (DesktopPlatform->OpenFileDialog(
              FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared()),
              bImage ? LOCTEXT("ImageBrowseTitle", "Image to import").ToString()
                     : LOCTEXT("PsdBrowseTitle", "Photoshop export: manifest.json").ToString(),
              Start, bImage ? FString() : FString(TEXT("manifest.json")),
              bImage ? UIWTPromptImage::GetFileTypes()
                     : FString(TEXT("Photoshop export (manifest.json)|manifest.json|JSON files "
                                    "(*.json)|*.json")),
              EFileDialogFlags::None, Files) &&
          !Files.IsEmpty())
      {
        FEditorDirectories::Get().SetLastDirectory(ELastDirectory::GENERIC_OPEN,
                                                   FPaths::GetPath(Files[0]));
        SourceBox->SetText(FText::FromString(FPaths::ConvertRelativePathToFull(Files[0])));
      }
      return FReply::Handled();
    }

    FReply OnImport()
    {
      Choice.Source = SourceText();
      Choice.TargetFolder = FolderBox->GetText().ToString().TrimStartAndEnd();
      Choice.BlueprintName = NameBox->GetText().ToString().TrimStartAndEnd();
      bConfirmed = true;
      Close();
      return FReply::Handled();
    }

    FReply OnCancel()
    {
      Close();
      return FReply::Handled();
    }

    void Close()
    {
      if (TSharedPtr<SWindow> Pinned = Window.Pin())
      {
        Pinned->RequestDestroyWindow();
      }
    }

    EUIWTDesignSource Source = EUIWTDesignSource::Figma;
    FUIWTDesignImportChoice Choice;
    TWeakPtr<SWindow> Window;
    TSharedPtr<SEditableTextBox> SourceBox;
    TSharedPtr<SEditableTextBox> FolderBox;
    TSharedPtr<SEditableTextBox> NameBox;
    bool bConfirmed = false;
  };
}

bool UIWTDesignImportDialog::IsAIPassOn(EUIWTDesignSource InSource)
{
  const UUIWTLocalSettings *Settings = UUIWTLocalSettings::Get();
  return InSource == EUIWTDesignSource::Figma   ? Settings->bRefineFigmaImports
         : InSource == EUIWTDesignSource::Image ? Settings->bRefineImageImports
                                                : Settings->bRefinePsdImports;
}

void UIWTDesignImportDialog::SetAIPass(EUIWTDesignSource InSource, bool bInOn)
{
  UUIWTLocalSettings *Settings = UUIWTLocalSettings::Get();
  bool &Toggle = InSource == EUIWTDesignSource::Figma   ? Settings->bRefineFigmaImports
                 : InSource == EUIWTDesignSource::Image ? Settings->bRefineImageImports
                                                        : Settings->bRefinePsdImports;
  Toggle = bInOn;
  Settings->SaveConfig();
}

FText UIWTDesignImportDialog::AIPassModelName()
{
  const UUIWTLocalSettings *Settings = UUIWTLocalSettings::Get();
  return Settings->RefineModel == EUIWTClaudeModel::Custom
             ? FText::FromString(Settings->RefineCustomModel)
             : UEnum::GetDisplayValueAsText(Settings->RefineModel);
}

FText UIWTDesignImportDialog::DescribeAIPass()
{
  const UUIWTLocalSettings *Settings = UUIWTLocalSettings::Get();
  const FText Model = AIPassModelName();
  return FText::Format(
      LOCTEXT("AIPassTip",
              "After the import, Claude refines the new blueprint: buttons, lists and "
              "scroll areas, layout boxes, anchors, clean names and variables. It uses "
              "tokens; an estimate is shown before it starts. Off, the import uses none.\n"
              "Model: {0}, up to {1} rounds (UI Widget Tool (local) settings > AI Pass). "
              "Needs the MCP connection."),
      Model, FText::AsNumber(Settings->RefineRounds));
}

bool UIWTDesignImportDialog::Show(EUIWTDesignSource InSource, const TSharedPtr<SWidget> &InParent,
                                  FUIWTDesignImportChoice &InOutChoice)
{
  const TSharedRef<SWindow> Window =
      SNew(SWindow)
          .Title(InSource == EUIWTDesignSource::Figma   ? LOCTEXT("FigmaDialogTitle", "Import a Figma frame")
                 : InSource == EUIWTDesignSource::Image ? LOCTEXT("ImageDialogTitle", "Import an image")
                                                        : LOCTEXT("PsdDialogTitle", "Import a Photoshop export"))
          .SizingRule(ESizingRule::Autosized)
          .SupportsMinimize(false)
          .SupportsMaximize(false);
  const TSharedRef<SUIWTDesignImportDialog> Dialog = SNew(SUIWTDesignImportDialog)
                                                         .Source(InSource)
                                                         .Choice(InOutChoice)
                                                         .Window(Window);
  Window->SetContent(Dialog);
  const TSharedPtr<SWindow> Parent =
      InParent.IsValid() ? FSlateApplication::Get().FindWidgetWindow(InParent.ToSharedRef())
                         : nullptr;
  FSlateApplication::Get().AddModalWindow(Window, Parent);
  if (!Dialog->IsConfirmed())
  {
    return false;
  }
  InOutChoice = Dialog->GetChoice();
  return true;
}

#undef LOCTEXT_NAMESPACE
