#include "FoliageBakerTreeHierarchyColorBaker.h"
#include "FoliageBakerTreeSkeleton.h"

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
	constexpr double ThinBranchRadius = 0.5;
	constexpr int32 ThinBranchSideCount = 6;

	const TArray<FSoftObjectPath> TreeHierarchyTestAssetPaths = {
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/LP/SM_Tree_Set_03a.SM_Tree_Set_03a")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/LP/SM_Tree_Set_03b.SM_Tree_Set_03b")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/LP/SM_Tree_Set_03c.SM_Tree_Set_03c")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/LP/SM_Tree_Set_03d.SM_Tree_Set_03d")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/LP/SM_Tree_Set_03e.SM_Tree_Set_03e")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/LP/SM_Tree_Set_03f.SM_Tree_Set_03f")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/LP/SM_Tree_Set_03g.SM_Tree_Set_03g")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/LP/SM_Tree_Set_03h.SM_Tree_Set_03h")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/LP/SM_Tree_Set_03i.SM_Tree_Set_03i")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/LP/SM_Tree_Set_03j.SM_Tree_Set_03j")),
		FSoftObjectPath(TEXT("/Game/Foliage_Sets/VOL3_Oaks/Meshes/LP/SM_Tree_Set_03k.SM_Tree_Set_03k")),
		FSoftObjectPath(TEXT("/Game/Demo/BillboardClouds/SM_Londontree001.SM_Londontree001"))};

	TArray<FFoliageBakerTreeSkeletonTriangle> BuildThinBranchTriangles()
	{
		const TStaticArray<double, 3> RingPositions{0.0, 50.0, 100.0};
		TArray<TArray<FVector>> Rings;
		Rings.SetNum(RingPositions.Num());
		for (int32 RingIndex = 0; RingIndex < RingPositions.Num(); ++RingIndex)
		{
			TArray<FVector>& Ring = Rings[RingIndex];
			Ring.Reserve(ThinBranchSideCount);
			for (int32 SideIndex = 0; SideIndex < ThinBranchSideCount; ++SideIndex)
			{
				const double Angle = 2.0 * UE_PI
					* static_cast<double>(SideIndex)
					/ static_cast<double>(ThinBranchSideCount);
				Ring.Add(FVector(
					RingPositions[RingIndex],
					FMath::Cos(Angle) * ThinBranchRadius,
					FMath::Sin(Angle) * ThinBranchRadius));
			}
		}

		TArray<FFoliageBakerTreeSkeletonTriangle> Triangles;
		for (int32 SegmentIndex = 0;
			SegmentIndex + 1 < Rings.Num();
			++SegmentIndex)
		{
			for (int32 SideIndex = 0;
				SideIndex < ThinBranchSideCount;
				++SideIndex)
			{
				const int32 NextSideIndex =
					(SideIndex + 1) % ThinBranchSideCount;
				FFoliageBakerTreeSkeletonTriangle& First =
					Triangles.AddDefaulted_GetRef();
				First.SourceTriangleID = Triangles.Num() - 1;
				First.SourceComponentID = 0;
				First.A = Rings[SegmentIndex][SideIndex];
				First.B = Rings[SegmentIndex + 1][SideIndex];
				First.C = Rings[SegmentIndex + 1][NextSideIndex];

				FFoliageBakerTreeSkeletonTriangle& Second =
					Triangles.AddDefaulted_GetRef();
				Second.SourceTriangleID = Triangles.Num() - 1;
				Second.SourceComponentID = 0;
				Second.A = Rings[SegmentIndex][SideIndex];
				Second.B = Rings[SegmentIndex + 1][NextSideIndex];
				Second.C = Rings[SegmentIndex][NextSideIndex];
			}
		}
		return Triangles;
	}

	bool ValidateThinBranchTerminalGuide(FAutomationTestBase& Test)
	{
		const TArray<FFoliageBakerTreeSkeletonGuide> Guides =
			FFoliageBakerTreeSkeleton::BuildSurfaceGuides(
				BuildThinBranchTriangles(),
				0,
				1.0,
				FVector::ZeroVector);
		double MaximumGuideX = TNumericLimits<double>::Lowest();
		for (const FFoliageBakerTreeSkeletonGuide& Guide : Guides)
		{
			for (const FVector& Point : Guide.Polyline)
			{
				MaximumGuideX = FMath::Max(MaximumGuideX, Point.X);
			}
		}
		Test.TestTrue(
			TEXT("Thin low-poly branch produces a surface guide"),
			!Guides.IsEmpty());
		return Test.TestTrue(
			TEXT("Thin low-poly branch guide reaches the source terminal section"),
			MaximumGuideX >= 100.0 - TestPositionTolerance);
	}

	double SegmentOverlapLength(
		const FVector& FirstStart,
		const FVector& FirstEnd,
		const FVector& SecondStart,
		const FVector& SecondEnd,
		const double DistanceTolerance)
	{
		const FVector FirstVector = FirstEnd - FirstStart;
		const FVector SecondVector = SecondEnd - SecondStart;
		const double FirstLength = FirstVector.Length();
		const double SecondLength = SecondVector.Length();
		if (FirstLength <= TestPositionTolerance
			|| SecondLength <= TestPositionTolerance)
		{
			return 0.0;
		}
		const FVector FirstDirection = FirstVector / FirstLength;
		const FVector SecondDirection = SecondVector / SecondLength;
		if (FMath::Abs(FVector::DotProduct(FirstDirection, SecondDirection)) < 0.98)
		{
			return 0.0;
		}
		const FVector SecondStartOnFirst = FMath::ClosestPointOnInfiniteLine(
			FirstStart,
			FirstEnd,
			SecondStart);
		const FVector SecondEndOnFirst = FMath::ClosestPointOnInfiniteLine(
			FirstStart,
			FirstEnd,
			SecondEnd);
		if (FVector::Distance(SecondStart, SecondStartOnFirst) > DistanceTolerance
			|| FVector::Distance(SecondEnd, SecondEndOnFirst) > DistanceTolerance)
		{
			return 0.0;
		}
		const double ProjectedStart = FVector::DotProduct(
			SecondStartOnFirst - FirstStart,
			FirstDirection);
		const double ProjectedEnd = FVector::DotProduct(
			SecondEndOnFirst - FirstStart,
			FirstDirection);
		return FMath::Max(
			0.0,
			FMath::Min(FirstLength, FMath::Max(ProjectedStart, ProjectedEnd))
				- FMath::Max(0.0, FMath::Min(ProjectedStart, ProjectedEnd)));
	}

	bool ValidateNoDuplicateSkeletonPaths(
		FAutomationTestBase& Test,
		const FString& AssetName,
		const FFoliageBakerTreeHierarchyPreviewData& PreviewData)
	{
		const double DistanceTolerance = FMath::Max(
			PreviewData.SkeletonCellSize * 0.4,
			TestPositionTolerance);
		const double MinimumSignificantOverlap = FMath::Max(
			PreviewData.SkeletonCellSize * 1.5,
			TestPositionTolerance * 2.0);
		double MaximumDuplicateOverlap = 0.0;
		for (int32 FirstEdgeIndex = 0;
			FirstEdgeIndex < PreviewData.SkeletonEdges.Num();
			++FirstEdgeIndex)
		{
			const FFoliageBakerTreeHierarchyPreviewEdge& FirstEdge =
				PreviewData.SkeletonEdges[FirstEdgeIndex];
			for (int32 SecondEdgeIndex = FirstEdgeIndex + 1;
				SecondEdgeIndex < PreviewData.SkeletonEdges.Num();
				++SecondEdgeIndex)
			{
				const FFoliageBakerTreeHierarchyPreviewEdge& SecondEdge =
					PreviewData.SkeletonEdges[SecondEdgeIndex];
				for (int32 FirstPointIndex = 1;
					FirstPointIndex < FirstEdge.Polyline.Num();
					++FirstPointIndex)
				{
					for (int32 SecondPointIndex = 1;
						SecondPointIndex < SecondEdge.Polyline.Num();
						++SecondPointIndex)
					{
						MaximumDuplicateOverlap = FMath::Max(
							MaximumDuplicateOverlap,
							SegmentOverlapLength(
								FirstEdge.Polyline[FirstPointIndex - 1],
								FirstEdge.Polyline[FirstPointIndex],
								SecondEdge.Polyline[SecondPointIndex - 1],
								SecondEdge.Polyline[SecondPointIndex],
								DistanceTolerance));
					}
				}
			}
		}
		return Test.TestTrue(
			FString::Printf(
				TEXT("%s has no significant duplicate skeleton path (maximum %.3f, limit %.3f)"),
				*AssetName,
				MaximumDuplicateOverlap,
				MinimumSignificantOverlap),
			MaximumDuplicateOverlap <= MinimumSignificantOverlap);
	}

	bool ValidateNoParallelSkeletonPaths(
		FAutomationTestBase& Test,
		const FString& AssetName,
		const FFoliageBakerTreeHierarchyPreviewData& PreviewData)
	{
		const double SampleSpacing = FMath::Max(
			PreviewData.SkeletonCellSize * 0.5,
			TestPositionTolerance);
		const double DistanceTolerance = FMath::Max(
			PreviewData.SkeletonCellSize * 0.55,
			TestPositionTolerance);
		const double MaximumAllowedParallelLength = FMath::Max(
			PreviewData.SkeletonCellSize * 2.0,
			TestPositionTolerance * 2.0);
		double MaximumParallelLength = 0.0;
		for (int32 FirstEdgeIndex = 0;
			FirstEdgeIndex < PreviewData.SkeletonEdges.Num();
			++FirstEdgeIndex)
		{
			const FFoliageBakerTreeHierarchyPreviewEdge& FirstEdge =
				PreviewData.SkeletonEdges[FirstEdgeIndex];
			for (int32 SecondEdgeIndex = FirstEdgeIndex + 1;
				SecondEdgeIndex < PreviewData.SkeletonEdges.Num();
				++SecondEdgeIndex)
			{
				const FFoliageBakerTreeHierarchyPreviewEdge& SecondEdge =
					PreviewData.SkeletonEdges[SecondEdgeIndex];
				double CurrentParallelLength = 0.0;
				for (int32 PointIndex = 1;
					PointIndex < FirstEdge.Polyline.Num();
					++PointIndex)
				{
					const FVector Start = FirstEdge.Polyline[PointIndex - 1];
					const FVector End = FirstEdge.Polyline[PointIndex];
					const FVector Direction = (End - Start).GetSafeNormal();
					const double SegmentLength = FVector::Distance(Start, End);
					const int32 StepCount = FMath::Max(
						1,
						FMath::CeilToInt(SegmentLength / SampleSpacing));
					for (int32 StepIndex = 0; StepIndex < StepCount; ++StepIndex)
					{
						const FVector Sample = FMath::Lerp(
							Start,
							End,
							(static_cast<double>(StepIndex) + 0.5)
								/ static_cast<double>(StepCount));
						bool bParallelCoverage = false;
						for (int32 OtherPointIndex = 1;
							OtherPointIndex < SecondEdge.Polyline.Num();
							++OtherPointIndex)
						{
							const FVector OtherStart =
								SecondEdge.Polyline[OtherPointIndex - 1];
							const FVector OtherEnd =
								SecondEdge.Polyline[OtherPointIndex];
							const FVector OtherDirection =
								(OtherEnd - OtherStart).GetSafeNormal();
							if (FMath::Abs(FVector::DotProduct(Direction, OtherDirection)) < 0.9)
							{
								continue;
							}
							const FVector Closest = FMath::ClosestPointOnSegment(
								Sample,
								OtherStart,
								OtherEnd);
							if (FVector::Distance(Sample, Closest) <= DistanceTolerance)
							{
								bParallelCoverage = true;
								break;
							}
						}
						CurrentParallelLength = bParallelCoverage
							? CurrentParallelLength + SegmentLength / StepCount
							: 0.0;
						MaximumParallelLength = FMath::Max(
							MaximumParallelLength,
							CurrentParallelLength);
					}
				}
			}
		}
		return Test.TestTrue(
			FString::Printf(
				TEXT("%s has no long nearby parallel skeleton path (maximum %.3f, limit %.3f)"),
				*AssetName,
				MaximumParallelLength,
				MaximumAllowedParallelLength),
			MaximumParallelLength <= MaximumAllowedParallelLength);
	}

	bool ValidateBranchSubtreeOwnership(
		FAutomationTestBase& Test,
		const FString& AssetName,
		const FFoliageBakerTreeHierarchyPreviewData& PreviewData)
	{
		TMap<int32, int32> IncomingEdgeByNode;
		TMap<int32, TArray<int32>> EdgeIndicesByBranch;
		for (int32 EdgeIndex = 0;
			EdgeIndex < PreviewData.SkeletonEdges.Num();
			++EdgeIndex)
		{
			const FFoliageBakerTreeHierarchyPreviewEdge& Edge =
				PreviewData.SkeletonEdges[EdgeIndex];
			IncomingEdgeByNode.Add(Edge.EndNodeID, EdgeIndex);
			if (!Edge.bTrunk && Edge.BranchID != INDEX_NONE)
			{
				EdgeIndicesByBranch.FindOrAdd(Edge.BranchID).Add(EdgeIndex);
			}
		}

		bool bValid = true;
		for (const TPair<int32, TArray<int32>>& BranchEdges : EdgeIndicesByBranch)
		{
			int32 DepartureCount = 0;
			for (const int32 EdgeIndex : BranchEdges.Value)
			{
				if (!PreviewData.SkeletonEdges.IsValidIndex(EdgeIndex))
				{
					continue;
				}
				const FFoliageBakerTreeHierarchyPreviewEdge& Edge =
					PreviewData.SkeletonEdges[EdgeIndex];
				if (!IncomingEdgeByNode.Contains(Edge.StartNodeID))
				{
					++DepartureCount;
					continue;
				}
				const int32 ParentEdgeIndex = IncomingEdgeByNode.FindRef(
					Edge.StartNodeID);
				const FFoliageBakerTreeHierarchyPreviewEdge& ParentEdge =
					PreviewData.SkeletonEdges[ParentEdgeIndex];
				if (ParentEdge.bTrunk)
				{
					++DepartureCount;
				}
				else
				{
					bValid &= Test.TestEqual(
						FString::Printf(
							TEXT("%s branch %d edge %d inherits its parent branch ID"),
							*AssetName,
							BranchEdges.Key,
							Edge.EdgeID),
						ParentEdge.BranchID,
						BranchEdges.Key);
				}
			}
			bValid &= Test.TestEqual(
				FString::Printf(
					TEXT("%s branch %d has exactly one trunk departure"),
					*AssetName,
					BranchEdges.Key),
				DepartureCount,
				1);
		}
		return bValid;
	}

	bool ValidateTrunkSemantics(
		FAutomationTestBase& Test,
		const FString& AssetName,
		const FFoliageBakerTreeHierarchyPreviewData& PreviewData)
	{
		TMultiMap<int32, int32> TrunkEdgesByStartNode;
		int32 ExpectedTrunkCylinderCount = 0;
		for (const FFoliageBakerTreeHierarchyPreviewEdge& Edge :
			PreviewData.SkeletonEdges)
		{
			int32 EdgeCylinderCount = 0;
			for (int32 PointIndex = 1;
				PointIndex < Edge.Polyline.Num();
				++PointIndex)
			{
				EdgeCylinderCount += Edge.Polyline[PointIndex - 1].Equals(
					Edge.Polyline[PointIndex],
					UE_DOUBLE_SMALL_NUMBER)
					? 0
					: 1;
			}
			if (Edge.bTrunk)
			{
				ExpectedTrunkCylinderCount += EdgeCylinderCount;
				Test.TestEqual(
					FString::Printf(
						TEXT("%s trunk edge %d has no branch ID"),
						*AssetName,
						Edge.EdgeID),
					Edge.BranchID,
					INDEX_NONE);
				TrunkEdgesByStartNode.Add(Edge.StartNodeID, Edge.EdgeID);
			}
		}

		TSet<int32> ReachedTrunkEdgeIDs;
		TArray<int32> TrunkNodeQueue;
		TrunkNodeQueue.Add(PreviewData.RootNodeID);
		for (int32 QueueIndex = 0; QueueIndex < TrunkNodeQueue.Num(); ++QueueIndex)
		{
			TArray<int32> OutgoingTrunkEdgeIDs;
			TrunkEdgesByStartNode.MultiFind(
				TrunkNodeQueue[QueueIndex],
				OutgoingTrunkEdgeIDs);
			for (const int32 EdgeID : OutgoingTrunkEdgeIDs)
			{
				const int32 TrunkEdgeIndex =
					PreviewData.SkeletonEdges.IndexOfByPredicate(
						[EdgeID](const FFoliageBakerTreeHierarchyPreviewEdge& Edge)
						{
							return Edge.EdgeID == EdgeID;
						});
				if (!PreviewData.SkeletonEdges.IsValidIndex(TrunkEdgeIndex))
				{
					continue;
				}
				ReachedTrunkEdgeIDs.Add(EdgeID);
				TrunkNodeQueue.Add(
					PreviewData.SkeletonEdges[TrunkEdgeIndex].EndNodeID);
			}
		}
		Test.TestEqual(
			FString::Printf(
				TEXT("%s trunk and merged roots form one rooted subtree"),
				*AssetName),
			ReachedTrunkEdgeIDs.Num(),
			TrunkEdgesByStartNode.Num());

		int32 TrunkPreviewCount = 0;
		int32 RootPreviewCount = 0;
		int32 ActualTrunkCylinderCount = 0;
		for (const FFoliageBakerTreeHierarchyPreviewBranch& Branch :
			PreviewData.Branches)
		{
			if (Branch.Label == TEXT("Trunk"))
			{
				++TrunkPreviewCount;
				ActualTrunkCylinderCount += Branch.Cylinders.Num();
			}
			else if (Branch.Label == TEXT("Root"))
			{
				++RootPreviewCount;
			}
		}
		bool bValid = Test.TestEqual(
			FString::Printf(TEXT("%s has exactly one trunk preview"), *AssetName),
			TrunkPreviewCount,
			1);
		bValid &= Test.TestEqual(
			FString::Printf(
				TEXT("%s trunk preview contains only trunk cylinders"),
				*AssetName),
			ActualTrunkCylinderCount,
			ExpectedTrunkCylinderCount);
		bValid &= Test.TestEqual(
			FString::Printf(
				TEXT("%s does not expose a separate root preview group"),
				*AssetName),
			RootPreviewCount,
			0);
		Test.AddInfo(FString::Printf(
			TEXT("%s semantic preview: %d trunk/root cylinder(s), %d branch group(s)"),
			*AssetName,
			ActualTrunkCylinderCount,
			PreviewData.Branches.Num()));
		return bValid;
	}

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
		if (!PreviewBranchIDs.IsEmpty())
		{
			Test.TestTrue(
				FString::Printf(
					TEXT("%s assigns foliage to at least one branch"),
					*AssetName),
				bHasBranchOwnedLeafCluster);
		}

		if (!Test.TestTrue(
			FString::Printf(TEXT("%s has skeleton nodes"), *AssetName),
			!PreviewData.SkeletonNodes.IsEmpty())
			|| !Test.TestTrue(
				FString::Printf(TEXT("%s has skeleton edges"), *AssetName),
				!PreviewData.SkeletonEdges.IsEmpty()))
		{
			return false;
		}
		Test.TestTrue(
			FString::Printf(TEXT("%s has voxelized wood"), *AssetName),
			PreviewData.WoodVolumeComponentCount > 0);
		Test.TestEqual(
			FString::Printf(TEXT("%s covers every source guide terminal"), *AssetName),
			PreviewData.UncoveredGuideTerminalCount,
			0);
		ValidateNoDuplicateSkeletonPaths(Test, AssetName, PreviewData);
		ValidateNoParallelSkeletonPaths(Test, AssetName, PreviewData);
		ValidateBranchSubtreeOwnership(Test, AssetName, PreviewData);
		ValidateTrunkSemantics(Test, AssetName, PreviewData);
		Test.TestTrue(
			FString::Printf(TEXT("%s has a valid root node"), *AssetName),
			PreviewData.SkeletonNodes.IsValidIndex(PreviewData.RootNodeID));
		if (!PreviewData.SkeletonNodes.IsValidIndex(PreviewData.RootNodeID))
		{
			return false;
		}
		const FFoliageBakerTreeHierarchyPreviewNode& Root =
			PreviewData.SkeletonNodes[PreviewData.RootNodeID];
		Test.TestTrue(
			FString::Printf(TEXT("%s root starts at the mesh pivot"), *AssetName),
			Root.Position.IsNearlyZero(TestPositionTolerance));
		Test.TestTrue(
			FString::Printf(TEXT("%s root is marked as root"), *AssetName),
			Root.Kind == EFoliageBakerTreeHierarchyPreviewNodeKind::Root);

		TArray<int32> IncomingEdgeCounts;
		IncomingEdgeCounts.Init(0, PreviewData.SkeletonNodes.Num());
		TArray<TArray<int32>> ChildNodes;
		ChildNodes.SetNum(PreviewData.SkeletonNodes.Num());
		for (const FFoliageBakerTreeHierarchyPreviewEdge& Edge :
			PreviewData.SkeletonEdges)
		{
			const bool bValidNodes =
				PreviewData.SkeletonNodes.IsValidIndex(Edge.StartNodeID)
				&& PreviewData.SkeletonNodes.IsValidIndex(Edge.EndNodeID);
			Test.TestTrue(
				FString::Printf(
					TEXT("%s edge %d references valid nodes"),
					*AssetName,
					Edge.EdgeID),
				bValidNodes);
			if (!bValidNodes)
			{
				continue;
			}
			Test.TestTrue(
				FString::Printf(
					TEXT("%s edge %d has a polyline"),
					*AssetName,
					Edge.EdgeID),
				Edge.Polyline.Num() >= 2);
			if (Edge.Polyline.Num() >= 2)
			{
				Test.TestTrue(
					FString::Printf(
						TEXT("%s edge %d starts at its node"),
						*AssetName,
						Edge.EdgeID),
					Edge.Polyline[0].Equals(
						PreviewData.SkeletonNodes[Edge.StartNodeID].Position,
						TestPositionTolerance));
				Test.TestTrue(
					FString::Printf(
						TEXT("%s edge %d ends at its node"),
						*AssetName,
						Edge.EdgeID),
					Edge.Polyline.Last().Equals(
						PreviewData.SkeletonNodes[Edge.EndNodeID].Position,
						TestPositionTolerance));
			}
			++IncomingEdgeCounts[Edge.EndNodeID];
			ChildNodes[Edge.StartNodeID].Add(Edge.EndNodeID);
			Test.TestTrue(
				FString::Printf(
					TEXT("%s non-trunk edge %d has a branch ID"),
					*AssetName,
					Edge.EdgeID),
				Edge.bTrunk
					|| Edge.BranchID != INDEX_NONE);
			Test.TestTrue(
				FString::Printf(
					TEXT("%s edge %d references a valid parent branch"),
					*AssetName,
					Edge.EdgeID),
				Edge.ParentBranchID == INDEX_NONE
					|| PreviewBranchIDs.Contains(Edge.ParentBranchID));
		}
		for (int32 NodeIndex = 0;
			NodeIndex < PreviewData.SkeletonNodes.Num();
			++NodeIndex)
		{
			Test.TestTrue(
				FString::Printf(
					TEXT("%s node %d has one rooted parent"),
					*AssetName,
					NodeIndex),
				IncomingEdgeCounts[NodeIndex]
					== (NodeIndex == PreviewData.RootNodeID ? 0 : 1));
			if (ChildNodes[NodeIndex].Num() > 1
				&& NodeIndex != PreviewData.RootNodeID)
			{
				Test.TestTrue(
					FString::Printf(
						TEXT("%s branching node %d is an explicit fork"),
						*AssetName,
						NodeIndex),
					PreviewData.SkeletonNodes[NodeIndex].Kind
						== EFoliageBakerTreeHierarchyPreviewNodeKind::Fork);
			}
		}

		TBitArray<> Reached(false, PreviewData.SkeletonNodes.Num());
		TArray<int32> Queue;
		Queue.Add(PreviewData.RootNodeID);
		Reached[PreviewData.RootNodeID] = true;
		for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
		{
			for (const int32 ChildNodeID : ChildNodes[Queue[QueueIndex]])
			{
				if (!Reached[ChildNodeID])
				{
					Reached[ChildNodeID] = true;
					Queue.Add(ChildNodeID);
				}
			}
		}
		Test.TestEqual(
			FString::Printf(TEXT("%s all nodes are pivot reachable"), *AssetName),
			Queue.Num(),
			PreviewData.SkeletonNodes.Num());
		return !Test.HasAnyErrors();
	}

	void ValidateTreeHierarchyAsset(
		FAutomationTestBase& Test,
		const FSoftObjectPath& AssetPath)
	{
		TStrongObjectPtr<UStaticMesh> SourceMesh(
			Cast<UStaticMesh>(AssetPath.TryLoad()));
		if (!Test.TestNotNull(
			FString::Printf(TEXT("Load %s"), *AssetPath.ToString()),
			SourceMesh.Get()))
		{
			return;
		}

		TStrongObjectPtr<UStaticMesh> TestMesh =
			CloneMeshForHierarchyTest(*SourceMesh);
		const FFoliageBakerTreeHierarchyColorBakeResult Result =
			FFoliageBakerTreeHierarchyColorBaker::Bake(*TestMesh, 0);
		const FString AssetName = SourceMesh->GetName();
		Test.AddInfo(Result.Report);
		if (!Test.TestTrue(
			FString::Printf(TEXT("%s hierarchy bake succeeds"), *AssetName),
			Result.bSucceeded))
		{
			Test.AddError(Result.Report);
			return;
		}
		if (!Test.TestNotNull(
			FString::Printf(TEXT("%s produces preview data"), *AssetName),
			Result.PreviewData.Get()))
		{
			return;
		}
		Test.TestEqual(
			FString::Printf(
				TEXT("%s reports every assigned leaf cluster"),
				*AssetName),
			Result.LeafClusterCount,
			Result.PreviewData->LeafClusters.Num());
		ValidatePreview(Test, AssetName, *Result.PreviewData);
	}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(
	FFoliageBakerTreeHierarchyAssetTest,
	"FoliageBaker.TreeHierarchy.Asset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FFoliageBakerTreeHierarchyAssetTest::GetTests(
	TArray<FString>& OutBeautifiedNames,
	TArray<FString>& OutTestCommands) const
{
	for (const FSoftObjectPath& AssetPath : TreeHierarchyTestAssetPaths)
	{
		OutBeautifiedNames.Add(AssetPath.GetAssetName());
		OutTestCommands.Add(AssetPath.ToString());
	}
}

bool FFoliageBakerTreeHierarchyAssetTest::RunTest(const FString& Parameters)
{
	const FScopedTransaction Transaction(NSLOCTEXT(
		"FoliageBaker",
		"TestSingleTreeHierarchyBake",
		"Test single tree hierarchy bake"));
	ValidateTreeHierarchyAsset(*this, FSoftObjectPath(Parameters));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFoliageBakerTreeHierarchyAllOakAssetsTest,
	"FoliageBaker.TreeHierarchy.AllOakAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFoliageBakerTreeHierarchyAllOakAssetsTest::RunTest(
	const FString& Parameters)
{
	ValidateThinBranchTerminalGuide(*this);
	const FScopedTransaction Transaction(NSLOCTEXT(
		"FoliageBaker",
		"TestTreeHierarchyBake",
		"Test tree hierarchy bake"));
	for (const FSoftObjectPath& AssetPath : TreeHierarchyTestAssetPaths)
	{
		ValidateTreeHierarchyAsset(*this, AssetPath);
	}
	return !HasAnyErrors();
}

#endif
