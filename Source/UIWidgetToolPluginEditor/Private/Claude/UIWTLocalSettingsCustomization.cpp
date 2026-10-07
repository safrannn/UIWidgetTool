#include "UIWTLocalSettingsCustomization.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "UIWTClaudeInstall.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UIWidgetTool"

TSharedRef<IDetailCustomization> FUIWTLocalSettingsCustomization::MakeInstance()
{
  return MakeShared<FUIWTLocalSettingsCustomization>();
}

void FUIWTLocalSettingsCustomization::CustomizeDetails(
    IDetailLayoutBuilder &DetailBuilder)
{
  IDetailCategoryBuilder &Category =
      DetailBuilder.EditCategory(TEXT("Claude Code"));

  Category.AddCustomRow(LOCTEXT("InstallRowFilter", "Install Claude Code"))
      .NameContent()
          [SNew(STextBlock)
               .Text(LOCTEXT("InstallRowName", "Installation"))
               .Font(IDetailLayoutBuilder::GetDetailFont())]
      .ValueContent()
          [SNew(SButton)
               .IsEnabled_Lambda(
                   []
                   {
                     return UIWTClaudeInstall::GetState() ==
                            UIWTClaudeInstall::EState::NotInstalled;
                   })
               .OnClicked_Lambda(
                   []
                   {
                     UIWTClaudeInstall::Install();
                     return FReply::Handled();
                   })
               .ToolTipText(LOCTEXT(
                   "InstallTooltip",
                   "Runs the official Claude Code installer."))
               [SNew(STextBlock)
                    .Font(IDetailLayoutBuilder::GetDetailFont())
                    .Text_Lambda(
                        []
                        {
                          switch (UIWTClaudeInstall::GetState())
                          {
                          case UIWTClaudeInstall::EState::Installed:
                            return LOCTEXT("Installed", "Installed");
                          case UIWTClaudeInstall::EState::Installing:
                            return LOCTEXT("Installing", "Installing...");
                          default:
                            return LOCTEXT("Install", "Install");
                          }
                        })]];
}

#undef LOCTEXT_NAMESPACE
