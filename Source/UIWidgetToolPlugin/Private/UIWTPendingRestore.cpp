#include "UIWTPendingRestore.h"

#include "UIWTCheckpointTypes.h"

FUIWTPendingRestore &FUIWTPendingRestore::Get()
{
  static FUIWTPendingRestore Instance;
  return Instance;
}

void FUIWTPendingRestore::Clear()
{
  FUIWTPendingRestore &Instance = Get();
  if (Instance.bPending)
  {
    UE_LOG(LogUIWidgetToolPlugin, Verbose,
           TEXT("Cleared the pending restore request for %s."),
           *Instance.MapPackagePath);
  }
  Instance = FUIWTPendingRestore();
}

bool FUIWTPendingRestore::Claim(FUIWTPendingRestore &OutClaimed)
{
  if (!bPending)
  {
    return false;
  }
  OutClaimed = *this;
  *this = FUIWTPendingRestore();
  return true;
}
