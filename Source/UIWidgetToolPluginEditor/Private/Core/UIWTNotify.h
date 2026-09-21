#pragma once

#include "CoreMinimal.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

namespace UIWTNotify
{

  // Fire-and-forget editor toast. Failures linger a little longer.
  inline void Show(const FText &InText, bool bSuccess)
  {
    FNotificationInfo Info(InText);
    Info.ExpireDuration = bSuccess ? 5.f : 8.f;
    Info.bFireAndForget = true;
    if (TSharedPtr<SNotificationItem> Item =
            FSlateNotificationManager::Get().AddNotification(Info))
    {
      Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success
                                        : SNotificationItem::CS_Fail);
    }
  }

}
