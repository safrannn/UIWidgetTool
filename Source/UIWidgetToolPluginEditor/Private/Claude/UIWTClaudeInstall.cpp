#include "UIWTClaudeInstall.h"

#include "Async/Async.h"
#include "Core/UIWTLocalSettings.h"
#include "Core/UIWTNotify.h"
#include "HAL/PlatformProcess.h"
#include "UIWTCheckpointTypes.h"
#include "UIWTClaudeRunner.h"

#define LOCTEXT_NAMESPACE "UIWidgetTool"

namespace
{
  UIWTClaudeInstall::EState GState = UIWTClaudeInstall::EState::NotInstalled;

  // The native installer; it puts claude in ~/.local/bin, which
  // FindExecutable searches even when PATH hasn't picked it up yet.
#if PLATFORM_WINDOWS
  const TCHAR *const InstallExecutable = TEXT("powershell.exe");
  const TCHAR *const InstallArgs =
      TEXT("-NoProfile -ExecutionPolicy Bypass -Command "
           "\"irm https://claude.ai/install.ps1 | iex\"");
#else
  const TCHAR *const InstallExecutable = TEXT("/bin/bash");
  const TCHAR *const InstallArgs =
      TEXT("-c \"curl -fsSL https://claude.ai/install.sh | bash\"");
#endif
}

void UIWTClaudeInstall::Check()
{
  FText Error;
  GState = FUIWTClaudeRunner::FindExecutable(
               UUIWTLocalSettings::Get()->ClaudeExecutable.FilePath, Error)
                   .IsEmpty()
               ? EState::NotInstalled
               : EState::Installed;
}

UIWTClaudeInstall::EState UIWTClaudeInstall::GetState() { return GState; }

void UIWTClaudeInstall::Install()
{
  check(IsInGameThread());
  if (GState != EState::NotInstalled)
  {
    return;
  }
  GState = EState::Installing;
  UE_LOG(LogUIWidgetToolPlugin, Log, TEXT("Installing Claude Code: %s %s"),
         InstallExecutable, InstallArgs);

  Async(EAsyncExecution::Thread,
        []
        {
          int32 ReturnCode = -1;
          FString StdOut;
          FString StdErr;
          const bool bLaunched = FPlatformProcess::ExecProcess(
              InstallExecutable, InstallArgs, &ReturnCode, &StdOut, &StdErr);

          AsyncTask(ENamedThreads::GameThread,
                    [bLaunched, ReturnCode, StdOut = MoveTemp(StdOut),
                     StdErr = MoveTemp(StdErr)]
                    {
                      Check();
                      if (GState == EState::Installed)
                      {
                        UE_LOG(LogUIWidgetToolPlugin, Log,
                               TEXT("Claude Code installed.\n%s"), *StdOut);
                        UIWTNotify::Show(LOCTEXT("ClaudeInstalled",
                                                 "Claude Code installed."),
                                         true);
                        return;
                      }
                      UE_LOG(LogUIWidgetToolPlugin, Warning,
                             TEXT("Claude Code install failed (launched: %d, "
                                  "exit code %d).\n%s\n%s"),
                             bLaunched, ReturnCode, *StdOut, *StdErr);
                      UIWTNotify::Show(
                          LOCTEXT("ClaudeInstallFailed",
                                  "Claude Code install failed. See the "
                                  "Output Log for details."),
                          false);
                    });
        });
}

#undef LOCTEXT_NAMESPACE
