#include "UIWTLevelScan.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/World.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UIWidgetPreviewObjectManagerSettings.h"

namespace
{
  // The referencer graph around the requested widgets, fetched once from the
  // Asset Registry so every widget's own walk is a lookup rather than a query.
  struct FReferenceSnapshot
  {
    TMap<FName, TArray<FName>> PackageToReferencers;
    TSet<FName> WorldPackages;
    int32 MaxDepth = 3;
  };

  FReferenceSnapshot
  BuildSnapshot(const TArray<FUIWTLevelScanRequest> &InRequests)
  {
    FReferenceSnapshot Snapshot;

    const UUIWidgetPreviewObjectManagerSettings *Settings =
        GetDefault<UUIWidgetPreviewObjectManagerSettings>();
    Snapshot.MaxDepth = FMath::Max(1, Settings->MaxDepth);

    IAssetRegistry &AssetRegistry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry")
            .Get();

    const FTopLevelAssetPath WorldClassPath =
        UWorld::StaticClass()->GetClassPathName();

    TSet<FName> Visited;
    TArray<FName> Frontier;
    for (const FUIWTLevelScanRequest &Request : InRequests)
    {
      if (!Request.WidgetPackageName.IsNone() &&
          !Visited.Contains(Request.WidgetPackageName))
      {
        Visited.Add(Request.WidgetPackageName);
        Frontier.Add(Request.WidgetPackageName);
      }
    }

    for (int32 Depth = 0; Depth < Snapshot.MaxDepth && Frontier.Num() > 0;
         ++Depth)
    {
      TArray<FName> NextFrontier;

      for (const FName &PackageName : Frontier)
      {
        TArray<FName> &Referencers =
            Snapshot.PackageToReferencers.Add(PackageName);
        AssetRegistry.GetReferencers(
            PackageName, Referencers,
            UE::AssetRegistry::EDependencyCategory::Package);

        for (const FName &Referencer : Referencers)
        {
          if (Visited.Contains(Referencer))
          {
            continue;
          }
          Visited.Add(Referencer);

          TArray<FAssetData> Assets;
          AssetRegistry.GetAssetsByPackageName(Referencer, Assets, true);
          const bool bIsWorld = Assets.ContainsByPredicate(
              [&WorldClassPath](const FAssetData &Asset)
              { return Asset.AssetClassPath == WorldClassPath; });

          if (bIsWorld)
          {
            Snapshot.WorldPackages.Add(Referencer);
          }
          else
          {
            NextFrontier.Add(Referencer);
          }
        }
      }

      Frontier = MoveTemp(NextFrontier);
    }

    return Snapshot;
  }

  // Each widget walks the shared snapshot separately because depth is counted
  // from that widget: a level three hops from one widget may be five from another.
  FUIWTLevelScanResult Traverse(const FUIWTLevelScanRequest &InRequest,
                                const FReferenceSnapshot &InSnapshot)
  {
    FUIWTLevelScanResult Result;
    Result.EntryId = InRequest.EntryId;

    if (InRequest.WidgetPackageName.IsNone())
    {
      return Result;
    }

    TSet<FName> Visited;
    TSet<FName> Levels;
    TArray<FName> Frontier;
    Visited.Add(InRequest.WidgetPackageName);
    Frontier.Add(InRequest.WidgetPackageName);

    for (int32 Depth = 0; Depth < InSnapshot.MaxDepth && Frontier.Num() > 0;
         ++Depth)
    {
      TArray<FName> NextFrontier;
      for (const FName &PackageName : Frontier)
      {
        const TArray<FName> *Referencers =
            InSnapshot.PackageToReferencers.Find(PackageName);
        if (!Referencers)
        {
          continue;
        }
        for (const FName &Referencer : *Referencers)
        {
          if (Visited.Contains(Referencer))
          {
            continue;
          }
          Visited.Add(Referencer);
          if (InSnapshot.WorldPackages.Contains(Referencer))
          {
            Levels.Add(Referencer);
          }
          else
          {
            NextFrontier.Add(Referencer);
          }
        }
      }
      Frontier = MoveTemp(NextFrontier);
    }

    Result.LevelPackagePaths = Levels.Array();
    Result.LevelPackagePaths.Sort([](const FName &A, const FName &B)
                                  { return A.LexicalLess(B); });

    return Result;
  }

}

namespace UIWTLevelScan
{

  TArray<FUIWTLevelScanResult>
  Scan(const TArray<FUIWTLevelScanRequest> &InRequests)
  {
    check(IsInGameThread());

    const FReferenceSnapshot Snapshot = BuildSnapshot(InRequests);

    TArray<FUIWTLevelScanResult> Results;
    Results.Reserve(InRequests.Num());
    for (const FUIWTLevelScanRequest &Request : InRequests)
    {
      Results.Add(Traverse(Request, Snapshot));
    }
    return Results;
  }

  // The map's short name, prefixed with its parent folder when another level
  // in InAllPaths shares that short name.
  FString MakeLevelDisplayName(FName InPackagePath, const TSet<FName> &InAllPaths)
  {
    const FString FullPath = InPackagePath.ToString();
    const FString AssetName = FPackageName::GetShortName(FullPath);

    for (const FName &Other : InAllPaths)
    {
      if (Other != InPackagePath &&
          FPackageName::GetShortName(Other.ToString())
              .Equals(AssetName, ESearchCase::IgnoreCase))
      {
        const FString ParentFolder =
            FPackageName::GetShortName(FPaths::GetPath(FullPath));
        return FString::Printf(TEXT("%s/%s"), *ParentFolder, *AssetName);
      }
    }

    return AssetName;
  }

}
