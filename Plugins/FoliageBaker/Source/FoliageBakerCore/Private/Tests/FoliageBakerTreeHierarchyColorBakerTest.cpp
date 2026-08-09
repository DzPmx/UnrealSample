#include "FoliageBakerTreeHierarchyColorBaker.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSourceData.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "ScopedTransaction.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr double TestPositionTolerance = 1.0e-3;

	const TArray<FSoftObjectPath> TreeHierarchyTestAssetPaths = {
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/NN/SM_Tree_Set_NN_03a.SM_Tree_Set_NN_03a")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/NN/SM_Tree_Set_NN_03b.SM_Tree_Set_NN_03b")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/NN/SM_Tree_Set_NN_03c.SM_Tree_Set_NN_03c")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/NN/SM_Tree_Set_NN_03d.SM_Tree_Set_NN_03d")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/NN/SM_Tree_Set_NN_03e.SM_Tree_Set_NN_03e")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/NN/SM_Tree_Set_NN_03f.SM_Tree_Set_NN_03f")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/NN/SM_Tree_Set_NN_03g.SM_Tree_Set_NN_03g")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/NN/SM_Tree_Set_NN_03h.SM_Tree_Set_NN_03h")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/NN/SM_Tree_Set_NN_03i.SM_Tree_Set_NN_03i")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/NN/SM_Tree_Set_NN_03j.SM_Tree_Set_NN_03j")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/NN/SM_Tree_Set_NN_03k.SM_Tree_Set_NN_03k"))};

	double PointToCylinderDistanceSquared(
		const FVector& Point,
		const FFoliageBakerTreeHierarchyPreviewCylinder& Cylinder)
	{
		return FVector::DistSquared(
			Point,
			FMath::ClosestPointOnSegment(
				Point,
				Cylinder.Start,
				Cylinder.End));
	}

	bool IsPointOnAnyCylinder(
		const FVector& Point,
		const TArray<FFoliageBakerTreeHierarchyPreviewCylinder>& Cylinders)
	{
		for (const FFoliageBakerTreeHierarchyPreviewCylinder& Cylinder : Cylinders)
		{
			if (PointToCylinderDistanceSquared(Point, Cylinder)
				<= FMath::Square(TestPositionTolerance))
			{
				return true;
			}
		}
		return false;
	}

	TStrongObjectPtr<UStaticMesh> CloneMeshForHierarchyTest(
		const UStaticMesh& SourceMesh)
	{
		TStrongObjectPtr<UStaticMesh> TestMesh(NewObject<UStaticMesh>(
			GetTransientPackage(),
			NAME_None,
			RF_Transient | RF_Transactional));
		TestMesh->GetStaticMaterials() = SourceMesh.GetStaticMaterials();
		TestMesh->AddSourceModel();
		TestMesh->CreateMeshDescription(0);
		check(TestMesh->IsMeshDescriptionValid(0));
		check(SourceMesh.IsMeshDescriptionValid(0));
		TestMesh->GetSourceModel(0).StaticMeshDescriptionBulkData->SetFlags(
			RF_Transactional);
		*TestMesh->GetMeshDescription(0) = *SourceMesh.GetMeshDescription(0);
		UStaticMesh::FCommitMeshDescriptionParams CommitParams;
		CommitParams.bMarkPackageDirty = false;
		CommitParams.bUseHashAsGuid = true;
		TestMesh->CommitMeshDescription(0, CommitParams);
		TestMesh->PostEditChange();
		return TestMesh;
	}

	bool ValidatePreview(
		FAutomationTestBase& Test,
		const FString& AssetName,
		const FFoliageBakerTreeHierarchyPreviewData& PreviewData)
	{
		if (!Test.TestTrue(
			FString::Printf(TEXT("%s has preview branches"), *AssetName),
			!PreviewData.Branches.IsEmpty()))
		{
			return false;
		}
		Test.TestTrue(
			FString::Printf(TEXT("%s has assigned leaf clusters"), *AssetName),
			!PreviewData.LeafClusters.IsEmpty());
		TSet<int32> PreviewBranchIDs;
		for (const FFoliageBakerTreeHierarchyPreviewBranch& Branch :
			PreviewData.Branches)
		{
			if (Branch.BranchID != INDEX_NONE)
			{
				PreviewBranchIDs.Add(Branch.BranchID);
			}
		}
		bool bHasBranchOwnedLeafCluster = false;
		for (int32 LeafClusterIndex = 0;
			LeafClusterIndex < PreviewData.LeafClusters.Num();
			++LeafClusterIndex)
		{
			const FFoliageBakerTreeHierarchyPreviewLeafCluster& LeafCluster =
				PreviewData.LeafClusters[LeafClusterIndex];
			Test.TestTrue(
				FString::Printf(
					TEXT("%s leaf cluster %d has source triangles"),
					*AssetName,
					LeafClusterIndex),
				!LeafCluster.SourceTriangleIDs.IsEmpty());
			Test.TestTrue(
				FString::Printf(
					TEXT("%s leaf cluster %d has valid bounds"),
					*AssetName,
					LeafClusterIndex),
				LeafCluster.Bounds.IsValid != 0);
			Test.TestEqual(
				FString::Printf(
					TEXT("%s leaf cluster %d preserves triangle positions"),
					*AssetName,
					LeafClusterIndex),
				LeafCluster.TrianglePositions.Num(),
				LeafCluster.SourceTriangleIDs.Num() * 3);
			Test.TestTrue(
				FString::Printf(
					TEXT("%s leaf cluster %d references an existing parent"),
					*AssetName,
					LeafClusterIndex),
				LeafCluster.ParentBranchID == INDEX_NONE
					|| PreviewBranchIDs.Contains(LeafCluster.ParentBranchID));
			bHasBranchOwnedLeafCluster |=
				LeafCluster.ParentBranchID != INDEX_NONE;
		}
		Test.TestTrue(
			FString::Printf(
				TEXT("%s assigns foliage to at least one branch"),
				*AssetName),
			bHasBranchOwnedLeafCluster);

		const FFoliageBakerTreeHierarchyPreviewBranch& Trunk =
			PreviewData.Branches[0];
		if (!Test.TestTrue(
			FString::Printf(TEXT("%s has trunk cylinders"), *AssetName),
			!Trunk.Cylinders.IsEmpty()))
		{
			return false;
		}
		Test.TestTrue(
			FString::Printf(TEXT("%s trunk starts at the mesh pivot"), *AssetName),
			Trunk.Cylinders[0].Start.IsNearlyZero(TestPositionTolerance));

		for (int32 CylinderIndex = 0;
			CylinderIndex < Trunk.Cylinders.Num();
			++CylinderIndex)
		{
			const FFoliageBakerTreeHierarchyPreviewCylinder& Cylinder =
				Trunk.Cylinders[CylinderIndex];
			Test.TestTrue(
				FString::Printf(
					TEXT("%s trunk cylinder %d advances"),
					*AssetName,
					CylinderIndex),
				Cylinder.End.Z > Cylinder.Start.Z);
			if (CylinderIndex > 0)
			{
				Test.TestTrue(
					FString::Printf(
						TEXT("%s trunk cylinder %d is continuous"),
						*AssetName,
						CylinderIndex),
					Cylinder.Start.Equals(
						Trunk.Cylinders[CylinderIndex - 1].End,
						TestPositionTolerance));
			}
		}

		for (int32 BranchIndex = 1;
			BranchIndex < PreviewData.Branches.Num();
			++BranchIndex)
		{
			const FFoliageBakerTreeHierarchyPreviewBranch& Branch =
				PreviewData.Branches[BranchIndex];
			bool bHasTrunkAttachment = false;
			for (const FFoliageBakerTreeHierarchyPreviewCylinder& Cylinder :
				Branch.Cylinders)
			{
				if (IsPointOnAnyCylinder(Cylinder.Start, Trunk.Cylinders))
				{
					bHasTrunkAttachment = true;
					break;
				}
			}
			Test.TestTrue(
				FString::Printf(
					TEXT("%s branch %d remains attached to the fitted trunk"),
					*AssetName,
					BranchIndex),
				bHasTrunkAttachment);
		}
		return !Test.HasAnyErrors();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFoliageBakerTreeHierarchyAllOakAssetsTest,
	"FoliageBaker.TreeHierarchy.AllOakAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFoliageBakerTreeHierarchyAllOakAssetsTest::RunTest(
	const FString& Parameters)
{
	const FScopedTransaction Transaction(NSLOCTEXT(
		"FoliageBaker",
		"TestTreeHierarchyBake",
		"Test tree hierarchy bake"));
	for (const FSoftObjectPath& AssetPath : TreeHierarchyTestAssetPaths)
	{
		TStrongObjectPtr<UStaticMesh> SourceMesh(
			Cast<UStaticMesh>(AssetPath.TryLoad()));
		if (!TestNotNull(
			FString::Printf(TEXT("Load %s"), *AssetPath.ToString()),
			SourceMesh.Get()))
		{
			continue;
		}

		TStrongObjectPtr<UStaticMesh> TestMesh =
			CloneMeshForHierarchyTest(*SourceMesh);
		const FFoliageBakerTreeHierarchyColorBakeResult Result =
			FFoliageBakerTreeHierarchyColorBaker::Bake(*TestMesh, 0);
		const FString AssetName = SourceMesh->GetName();
		if (!TestTrue(
			FString::Printf(TEXT("%s hierarchy bake succeeds"), *AssetName),
			Result.bSucceeded))
		{
			AddError(Result.Report);
			continue;
		}
		if (!TestNotNull(
			FString::Printf(TEXT("%s produces preview data"), *AssetName),
			Result.PreviewData.Get()))
		{
			continue;
		}
		TestEqual(
			FString::Printf(
				TEXT("%s reports every assigned leaf cluster"),
				*AssetName),
			Result.LeafClusterCount,
			Result.PreviewData->LeafClusters.Num());
		ValidatePreview(*this, AssetName, *Result.PreviewData);
	}
	return !HasAnyErrors();
}

#endif
