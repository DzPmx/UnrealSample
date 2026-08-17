#include "FoliageBakerTreeHierarchyColorBaker.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "StaticMeshAttributes.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr double TestPositionTolerance = 1.0e-3;

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
		TSet<int32> PreviewBranchIDs;
		for (const FFoliageBakerTreeHierarchyPreviewBranch& Branch :
			PreviewData.Branches)
		{
			if (Branch.BranchID != INDEX_NONE)
			{
				PreviewBranchIDs.Add(Branch.BranchID);
			}
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
		ValidateBranchSubtreeOwnership(Test, AssetName, PreviewData);
		ValidateTrunkSemantics(Test, AssetName, PreviewData);
		Test.TestEqual(
			FString::Printf(
				TEXT("%s exposes exactly two endpoint nodes per skeleton edge"),
				*AssetName),
			PreviewData.SkeletonNodes.Num(),
			PreviewData.SkeletonEdges.Num() * 2);
		for (const FFoliageBakerTreeHierarchyPreviewBranch& Branch :
			PreviewData.Branches)
		{
			int32 MatchingEdgeIndex = INDEX_NONE;
			int32 MatchingEdgeCount = 0;
			for (int32 EdgeIndex = 0;
				EdgeIndex < PreviewData.SkeletonEdges.Num();
				++EdgeIndex)
			{
				const FFoliageBakerTreeHierarchyPreviewEdge& Edge =
					PreviewData.SkeletonEdges[EdgeIndex];
				const bool bMatches = Branch.Label == TEXT("Trunk")
					? Edge.bTrunk
					: !Edge.bTrunk && Edge.BranchID == Branch.BranchID;
				if (bMatches)
				{
					MatchingEdgeIndex = EdgeIndex;
					++MatchingEdgeCount;
				}
			}
			Test.TestEqual(
				FString::Printf(
					TEXT("%s %s has one virtual edge"),
					*AssetName,
					*Branch.Label),
				MatchingEdgeCount,
				1);
			if (!PreviewData.SkeletonEdges.IsValidIndex(MatchingEdgeIndex))
			{
				continue;
			}
			const FFoliageBakerTreeHierarchyPreviewEdge& Edge =
				PreviewData.SkeletonEdges[MatchingEdgeIndex];
			const FFoliageBakerTreeHierarchyBoneRecord& BoneRecord =
				Branch.BoneRecord;
			const int32 ExpectedBakeID = Edge.bTrunk
				? 0
				: Branch.BranchID + 1;
			const int32 ExpectedParentBakeID = (
				Edge.bTrunk || Branch.ParentBranchID == INDEX_NONE)
					? 0
					: Branch.ParentBranchID + 1;
			Test.TestEqual(
				FString::Printf(
					TEXT("%s %s has its stable bake ID"),
					*AssetName,
					*Branch.Label),
				BoneRecord.BakeID,
				ExpectedBakeID);
			Test.TestEqual(
				FString::Printf(
					TEXT("%s %s has its parent bake ID"),
					*AssetName,
					*Branch.Label),
				BoneRecord.ParentBakeID,
				ExpectedParentBakeID);
			Test.TestTrue(
				FString::Printf(
					TEXT("%s %s has a normalized bake axis"),
					*AssetName,
					*Branch.Label),
				BoneRecord.Axis.IsNormalized());
			Test.TestTrue(
				FString::Printf(
					TEXT("%s %s has a positive axis extent"),
					*AssetName,
					*Branch.Label),
				BoneRecord.AxisExtent > 0.0);
			Test.TestTrue(
				FString::Printf(
					TEXT("%s %s stores projection span as axis extent"),
					*AssetName,
					*Branch.Label),
				FMath::IsNearlyEqual(
					BoneRecord.AxisExtent,
					BoneRecord.PositiveAxisExtent
						- BoneRecord.MinimumAxisProjection,
					TestPositionTolerance));
			Test.TestTrue(
				FString::Printf(
					TEXT("%s %s stores its pivot at the preview start"),
					*AssetName,
					*Branch.Label),
				BoneRecord.PivotPosition.Equals(
					Branch.StartPosition,
					TestPositionTolerance));
			if (Edge.bTrunk)
			{
				Test.TestTrue(
					FString::Printf(
						TEXT("%s trunk virtual edge preserves its fitted path"),
						*AssetName),
					Edge.Polyline.Num() >= 2);
			}
			else
			{
				Test.TestEqual(
					FString::Printf(
						TEXT("%s %s virtual edge contains only start and end"),
						*AssetName,
						*Branch.Label),
					Edge.Polyline.Num(),
					2);
				Test.TestTrue(
					FString::Printf(
						TEXT("%s %s virtual end follows its axis positive extent"),
						*AssetName,
						*Branch.Label),
					Branch.EndPosition.Equals(
						BoneRecord.PivotPosition
							+ BoneRecord.Axis
								* BoneRecord.PositiveAxisExtent,
						TestPositionTolerance));
			}
			if (Edge.Polyline.Num() >= 2)
			{
				Test.TestTrue(
					FString::Printf(
						TEXT("%s %s stores its virtual start"),
						*AssetName,
						*Branch.Label),
					Branch.StartPosition.Equals(
						Edge.Polyline[0],
						TestPositionTolerance));
				Test.TestTrue(
					FString::Printf(
						TEXT("%s %s stores its virtual end"),
						*AssetName,
						*Branch.Label),
					Branch.EndPosition.Equals(
						Edge.Polyline.Last(),
						TestPositionTolerance));
			}
			Test.TestEqual(
				FString::Printf(
					TEXT("%s %s exposes its two virtual points"),
					*AssetName,
					*Branch.Label),
				Branch.Joints.Num(),
				2);
		}
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
		TSet<int32> StartNodeIDs;
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
			StartNodeIDs.Add(Edge.StartNodeID);
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
					TEXT("%s virtual node %d has the expected parent count"),
					*AssetName,
					NodeIndex),
				IncomingEdgeCounts[NodeIndex]
					== (StartNodeIDs.Contains(NodeIndex) ? 0 : 1));
		}
		return !Test.HasAnyErrors();
	}

	bool ValidateWoodComponentOwnership(
		FAutomationTestBase& Test,
		const FString& AssetName,
		const UStaticMesh& StaticMesh,
		const int32 SourceLODIndex,
		const FFoliageBakerTreeHierarchyPreviewData& PreviewData)
	{
		const FMeshDescription& MeshDescription =
			*StaticMesh.GetMeshDescription(SourceLODIndex);
		const TVertexAttributesConstRef<FVector3f> VertexPositions =
			MeshDescription.GetVertexPositions();
		TMap<int32, int32> OwnerPreviewBranchIndexByTriangleID;
		bool bValid = true;
		for (int32 PreviewBranchIndex = 0;
			PreviewBranchIndex < PreviewData.Branches.Num();
			++PreviewBranchIndex)
		{
			for (const int32 TriangleIDValue :
				PreviewData.Branches[PreviewBranchIndex].SourceTriangleIDs)
			{
				if (OwnerPreviewBranchIndexByTriangleID.Contains(TriangleIDValue))
				{
					bValid &= Test.TestEqual(
						FString::Printf(
							TEXT("%s wood triangle %d has one final owner"),
							*AssetName,
							TriangleIDValue),
						OwnerPreviewBranchIndexByTriangleID.FindChecked(TriangleIDValue),
						PreviewBranchIndex);
				}
				else
				{
					OwnerPreviewBranchIndexByTriangleID.Add(
						TriangleIDValue,
						PreviewBranchIndex);
				}
			}
		}

		bValid &= Test.TestTrue(
			FString::Printf(TEXT("%s assigns wood triangles"), *AssetName),
			!OwnerPreviewBranchIndexByTriangleID.IsEmpty());
		TMap<FVertexID, int32> OwnerPreviewBranchIndexByVertexID;
		int32 PivotOwnerPreviewBranchIndex = INDEX_NONE;
		double PivotSurfaceDistanceSquared = TNumericLimits<double>::Max();
		TArray<int32> AssignedTriangleIDValues;
		OwnerPreviewBranchIndexByTriangleID.GetKeys(AssignedTriangleIDValues);
		AssignedTriangleIDValues.Sort();
		for (const int32 TriangleIDValue : AssignedTriangleIDValues)
		{
			const int32 OwnerPreviewBranchIndex =
				OwnerPreviewBranchIndexByTriangleID.FindChecked(TriangleIDValue);
			const FTriangleID TriangleID(TriangleIDValue);
			bValid &= Test.TestTrue(
				FString::Printf(
					TEXT("%s assigned triangle %d exists in LOD %d"),
					*AssetName,
					TriangleIDValue,
					SourceLODIndex),
				MeshDescription.IsTriangleValid(TriangleID));
			if (!MeshDescription.IsTriangleValid(TriangleID))
			{
				continue;
			}
			const TArrayView<const FVertexID> TriangleVertexIDs =
				MeshDescription.GetTriangleVertices(TriangleID);
			bValid &= Test.TestEqual(
				FString::Printf(
					TEXT("%s assigned triangle %d has three vertices"),
					*AssetName,
					TriangleIDValue),
				TriangleVertexIDs.Num(),
				3);
			if (TriangleVertexIDs.Num() != 3)
			{
				continue;
			}
			for (const FVertexID VertexID : TriangleVertexIDs)
			{
				if (OwnerPreviewBranchIndexByVertexID.Contains(VertexID))
				{
					bValid &= Test.TestEqual(
						FString::Printf(
							TEXT("%s connected wood at vertex %d has one final owner"),
							*AssetName,
							VertexID.GetValue()),
						OwnerPreviewBranchIndexByVertexID.FindChecked(VertexID),
						OwnerPreviewBranchIndex);
				}
				else
				{
					OwnerPreviewBranchIndexByVertexID.Add(
						VertexID,
						OwnerPreviewBranchIndex);
				}
			}

			const FVector A(VertexPositions[TriangleVertexIDs[0]]);
			const FVector B(VertexPositions[TriangleVertexIDs[1]]);
			const FVector C(VertexPositions[TriangleVertexIDs[2]]);
			const double DistanceSquared = FVector::DistSquared(
				FVector::ZeroVector,
				FMath::ClosestPointOnTriangleToPoint(
					FVector::ZeroVector,
					A,
					B,
					C));
			if (DistanceSquared < PivotSurfaceDistanceSquared)
			{
				PivotSurfaceDistanceSquared = DistanceSquared;
				PivotOwnerPreviewBranchIndex = OwnerPreviewBranchIndex;
			}
		}

		bValid &= Test.TestTrue(
			FString::Printf(
				TEXT("%s pivot wood component resolves to a preview owner"),
				*AssetName),
			PreviewData.Branches.IsValidIndex(PivotOwnerPreviewBranchIndex));
		if (PreviewData.Branches.IsValidIndex(PivotOwnerPreviewBranchIndex))
		{
			bValid &= Test.TestEqual(
				FString::Printf(
					TEXT("%s pivot wood component is owned by Trunk"),
					*AssetName),
				PreviewData.Branches[PivotOwnerPreviewBranchIndex].Label,
				FString(TEXT("Trunk")));
		}
		return bValid;
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

		const FFoliageBakerTreeHierarchyAnalysisResult Result =
			FFoliageBakerTreeHierarchyColorBaker::Analyze(
				*SourceMesh,
				0,
				0);
		const FString AssetName = SourceMesh->GetName();
		Test.AddInfo(Result.Report);
		if (!Test.TestTrue(
			FString::Printf(TEXT("%s hierarchy analysis succeeds"), *AssetName),
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
		ValidatePreview(Test, AssetName, *Result.PreviewData);
		ValidateWoodComponentOwnership(
			Test,
			AssetName,
			*SourceMesh,
			0,
			*Result.PreviewData);
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
	for (const FSoftObjectPath& AssetPath : TreeHierarchyTestAssetPaths)
	{
		ValidateTreeHierarchyAsset(*this, AssetPath);
	}
	return !HasAnyErrors();
}

#endif
