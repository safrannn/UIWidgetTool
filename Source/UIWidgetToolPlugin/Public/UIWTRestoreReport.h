#pragma once

#include "CoreMinimal.h"

struct UIWIDGETTOOLPLUGIN_API FUIWTRestoreReport {
  int32 MatchedActors = 0;
  int32 UnmatchedRecords = 0;

  int32 SpawnedRuntimeActors = 0;
  int32 SpawnFailures = 0;

  int32 SkippedReferences = 0;

  int32 RejectedProperties = 0;

  int32 AppliedComponents = 0;
  int32 UnmatchedComponents = 0;

  int32 CustomSectionsApplied = 0;
  int32 CustomSectionFailures = 0;

  bool bStreamingTimedOut = false;

  bool bLevelSignatureMismatch = false;

  TArray<FString> Messages;

  static constexpr int32 MaxMessages = 64;

  void AddMessage(const FString &InMessage) {
    if (Messages.Num() < MaxMessages) {
      Messages.Add(InMessage);
    } else if (Messages.Num() == MaxMessages) {
      Messages.Add(TEXT("... further messages suppressed."));
    }
  }

  bool HasProblems() const {
    return UnmatchedRecords > 0 || SpawnFailures > 0 || SkippedReferences > 0 ||
           RejectedProperties > 0 || UnmatchedComponents > 0 ||
           CustomSectionFailures > 0 || bStreamingTimedOut ||
           bLevelSignatureMismatch;
  }

  FString ToString() const {
    return FString::Printf(
        TEXT("matched %d, unmatched %d, spawned %d (%d failed), skipped refs "
             "%d, rejected props %d, components %d applied / %d unmatched, "
             "custom sections %d applied / %d failed%s%s"),
        MatchedActors, UnmatchedRecords, SpawnedRuntimeActors, SpawnFailures,
        SkippedReferences, RejectedProperties, AppliedComponents,
        UnmatchedComponents, CustomSectionsApplied, CustomSectionFailures,
        bStreamingTimedOut ? TEXT(", STREAMING TIMED OUT") : TEXT(""),
        bLevelSignatureMismatch ? TEXT(", LEVEL SIGNATURE MISMATCH")
                                : TEXT(""));
  }
};
