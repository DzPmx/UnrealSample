#include "FoliageBakerTreeHierarchyBaker.h"
#include "FoliageBakerAssetBuilder.h"
#include "FoliageBakerTreeSkeleton.h"

#include "Algo/Reverse.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Math/Float16Color.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoliageBakerTreeHierarchy, Log, All);

namespace
{
	constexpr double MinimumGeometryScale = UE_DOUBLE_SMALL_NUMBER;
	constexpr double MinimumBranchPreviewRadius = 0.5;
	constexpr double TrunkPreviewRadiusScale = 2.5;
	constexpr double JointPreviewRadiusScale = 1.5;

	struct FTreeTriangle
	{
		FTriangleID TriangleID;
		int32 MaterialIndex = INDEX_NONE;
		TStaticArray<FVertexID, 3> VertexIDs;
		TStaticArray<FVertexInstanceID, 3> VertexInstanceIDs;
	};

	struct FTriangleComponent
	{
		TArray<int32> TriangleIndices;
	};

	class FDisjointSet final
	{
	public:
		explicit FDisjointSet(const int32 Count)
		{
			Parents.SetNumUninitialized(Count);
			Ranks.Init(0, Count);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Parents[Index] = Index;
			}
		}

		int32 Find(const int32 Index)
		{
			int32 Root = Index;
			while (Parents[Root] != Root)
			{
				Root = Parents[Root];
			}
			int32 Current = Index;
			while (Parents[Current] != Current)
			{
				const int32 Parent = Parents[Current];
				Parents[Current] = Root;
				Current = Parent;
			}
			return Root;
		}

		void Union(const int32 First, const int32 Second)
		{
			int32 FirstRoot = Find(First);
			int32 SecondRoot = Find(Second);
			if (FirstRoot == SecondRoot)
			{
				return;
			}
			if (Ranks[FirstRoot] < Ranks[SecondRoot])
			{
				Swap(FirstRoot, SecondRoot);
			}
			Parents[SecondRoot] = FirstRoot;
			if (Ranks[FirstRoot] == Ranks[SecondRoot])
			{
				++Ranks[FirstRoot];
			}
		}

	private:
		TArray<int32> Parents;
		TArray<uint8> Ranks;
	};

	bool ReadTriangles(
		const UStaticMesh& StaticMesh,
		const FMeshDescription& MeshDescription,
		TArray<FTreeTriangle>& OutTriangles,
		FString& OutError)
	{
		if (!MeshDescription.PolygonGroupAttributes().HasAttribute(
				MeshAttribute::PolygonGroup::ImportedMaterialSlotName))
		{
			OutError = TEXT("Source LOD has no polygon-group material slot names.");
			return false;
		}

		const TPolygonGroupAttributesConstRef<FName> MaterialSlotNames =
			FStaticMeshConstAttributes(MeshDescription).GetPolygonGroupMaterialSlotNames();
		OutTriangles.Reserve(MeshDescription.Triangles().Num());
		for (const FTriangleID TriangleID : MeshDescription.Triangles().GetElementIDs())
		{
			const TArrayView<const FVertexID> TriangleVertexIDs =
				MeshDescription.GetTriangleVertices(TriangleID);
			const TArrayView<const FVertexInstanceID> TriangleVertexInstanceIDs =
				MeshDescription.GetTriangleVertexInstances(TriangleID);
			if (TriangleVertexIDs.Num() != 3
				|| TriangleVertexInstanceIDs.Num() != 3)
			{
				continue;
			}

			const FPolygonGroupID PolygonGroupID =
				MeshDescription.GetTrianglePolygonGroup(TriangleID);
			if (!MeshDescription.IsPolygonGroupValid(PolygonGroupID))
			{
				OutError = FString::Printf(
					TEXT("Triangle %d has no valid polygon group."),
					TriangleID.GetValue());
				return false;
			}

			const FName MaterialSlotName = MaterialSlotNames[PolygonGroupID];
			int32 MaterialIndex = StaticMesh.GetMaterialIndex(MaterialSlotName);
			if (MaterialIndex == INDEX_NONE)
			{
				MaterialIndex = StaticMesh.GetMaterialIndexFromImportedMaterialSlotName(
					MaterialSlotName);
			}
			if (MaterialIndex == INDEX_NONE)
			{
				OutError = FString::Printf(
					TEXT("Material slot '%s' used by source LOD geometry cannot be resolved."),
					*MaterialSlotName.ToString());
				return false;
			}

			FTreeTriangle& Triangle = OutTriangles.AddDefaulted_GetRef();
			Triangle.TriangleID = TriangleID;
			Triangle.MaterialIndex = MaterialIndex;
			for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex)
			{
				Triangle.VertexIDs[CornerIndex] = TriangleVertexIDs[CornerIndex];
				Triangle.VertexInstanceIDs[CornerIndex] =
					TriangleVertexInstanceIDs[CornerIndex];
			}
		}

		if (OutTriangles.IsEmpty())
		{
			OutError = TEXT("Source LOD contains no triangles.");
			return false;
		}
		return true;
	}

	TArray<FTriangleComponent> BuildTriangleComponents(
		const TArray<FTreeTriangle>& Triangles,
		const TArray<int32>& SelectedTriangleIndices)
	{
		TArray<FTriangleComponent> Components;
		if (SelectedTriangleIndices.IsEmpty())
		{
			return Components;
		}

		FDisjointSet DisjointSet(SelectedTriangleIndices.Num());
		TMap<FVertexID, int32> FirstTriangleByVertex;
		for (int32 LocalTriangleIndex = 0;
			LocalTriangleIndex < SelectedTriangleIndices.Num();
			++LocalTriangleIndex)
		{
			const FTreeTriangle& Triangle =
				Triangles[SelectedTriangleIndices[LocalTriangleIndex]];
			for (const FVertexID VertexID : Triangle.VertexIDs)
			{
				if (FirstTriangleByVertex.Contains(VertexID))
				{
					DisjointSet.Union(
						LocalTriangleIndex,
						FirstTriangleByVertex.FindChecked(VertexID));
				}
				else
				{
					FirstTriangleByVertex.Add(VertexID, LocalTriangleIndex);
				}
			}
		}

		TMap<int32, int32> ComponentIndexByRoot;
		for (int32 LocalTriangleIndex = 0;
			LocalTriangleIndex < SelectedTriangleIndices.Num();
			++LocalTriangleIndex)
		{
			const int32 Root = DisjointSet.Find(LocalTriangleIndex);
			int32& ComponentIndex = ComponentIndexByRoot.FindOrAdd(Root, INDEX_NONE);
			if (ComponentIndex == INDEX_NONE)
			{
				ComponentIndex = Components.AddDefaulted();
			}
			Components[ComponentIndex].TriangleIndices.Add(
				SelectedTriangleIndices[LocalTriangleIndex]);
		}

		Components.Sort(
			[&Triangles](const FTriangleComponent& First, const FTriangleComponent& Second)
			{
				return Triangles[First.TriangleIndices[0]].TriangleID.GetValue()
					< Triangles[Second.TriangleIndices[0]].TriangleID.GetValue();
			});
		return Components;
	}

	FString MakeUVPointToken(const FVector2f& UV)
	{
		const float CanonicalU = UV.X == 0.0f ? 0.0f : UV.X;
		const float CanonicalV = UV.Y == 0.0f ? 0.0f : UV.Y;
		return FString::Printf(
			TEXT("%08x,%08x"),
			FMath::AsUInt(CanonicalU),
			FMath::AsUInt(CanonicalV));
	}

	FString MakeUVTriangleSignature(
		const FVector2f& FirstUV,
		const FVector2f& SecondUV,
		const FVector2f& ThirdUV)
	{
		FString PointTokens[3] =
		{
			MakeUVPointToken(FirstUV),
			MakeUVPointToken(SecondUV),
			MakeUVPointToken(ThirdUV)
		};
		if (PointTokens[1] < PointTokens[0])
		{
			Swap(PointTokens[0], PointTokens[1]);
		}
		if (PointTokens[2] < PointTokens[1])
		{
			Swap(PointTokens[1], PointTokens[2]);
		}
		if (PointTokens[1] < PointTokens[0])
		{
			Swap(PointTokens[0], PointTokens[1]);
		}
		return PointTokens[0]
			+ TEXT("|")
			+ PointTokens[1]
			+ TEXT("|")
			+ PointTokens[2];
	}

	bool IsPointInsideUVTriangle(
		const FVector2f& Point,
		const TStaticArray<FVector2f, 3>& TriangleUVs)
	{
		const float FirstCross = FVector2f::CrossProduct(
			TriangleUVs[1] - TriangleUVs[0],
			Point - TriangleUVs[0]);
		const float SecondCross = FVector2f::CrossProduct(
			TriangleUVs[2] - TriangleUVs[1],
			Point - TriangleUVs[1]);
		const float ThirdCross = FVector2f::CrossProduct(
			TriangleUVs[0] - TriangleUVs[2],
			Point - TriangleUVs[2]);
		const bool bHasNegative =
			FirstCross < -UE_KINDA_SMALL_NUMBER
			|| SecondCross < -UE_KINDA_SMALL_NUMBER
			|| ThirdCross < -UE_KINDA_SMALL_NUMBER;
		const bool bHasPositive =
			FirstCross > UE_KINDA_SMALL_NUMBER
			|| SecondCross > UE_KINDA_SMALL_NUMBER
			|| ThirdCross > UE_KINDA_SMALL_NUMBER;
		return !(bHasNegative && bHasPositive);
	}

	bool ResolveUVPointPosition(
		const FVector2f& Point,
		const TStaticArray<FVector2f, 3>& TriangleUVs,
		const TStaticArray<FVector, 3>& TrianglePositions,
		FVector& OutPosition)
	{
		if (!IsPointInsideUVTriangle(Point, TriangleUVs))
		{
			return false;
		}

		const double Denominator =
			static_cast<double>(TriangleUVs[1].Y - TriangleUVs[2].Y)
				* static_cast<double>(TriangleUVs[0].X - TriangleUVs[2].X)
			+ static_cast<double>(TriangleUVs[2].X - TriangleUVs[1].X)
				* static_cast<double>(TriangleUVs[0].Y - TriangleUVs[2].Y);
		if (FMath::IsNearlyZero(Denominator))
		{
			return false;
		}
		const double FirstWeight =
			(static_cast<double>(TriangleUVs[1].Y - TriangleUVs[2].Y)
					* static_cast<double>(Point.X - TriangleUVs[2].X)
				+ static_cast<double>(TriangleUVs[2].X - TriangleUVs[1].X)
					* static_cast<double>(Point.Y - TriangleUVs[2].Y))
			/ Denominator;
		const double SecondWeight =
			(static_cast<double>(TriangleUVs[2].Y - TriangleUVs[0].Y)
					* static_cast<double>(Point.X - TriangleUVs[2].X)
				+ static_cast<double>(TriangleUVs[0].X - TriangleUVs[2].X)
					* static_cast<double>(Point.Y - TriangleUVs[2].Y))
			/ Denominator;
		const double ThirdWeight = 1.0 - FirstWeight - SecondWeight;
		OutPosition = TrianglePositions[0] * FirstWeight
			+ TrianglePositions[1] * SecondWeight
			+ TrianglePositions[2] * ThirdWeight;
		return true;
	}

	FVector4f MakeBranchColor(
		const UStaticMesh& StaticMesh,
		const int32 BranchID)
	{
		constexpr uint32 ColorMask = 0x00ffffffu;
		constexpr uint32 BranchPermutationMultiplier = 0x009e3779u;
		constexpr float MinimumChannelValue = 0.15f;
		constexpr float ChannelValueScale = 0.8f / 255.0f;
		// An odd multiplier is a permutation modulo 2^24, so distinct
		// Branch IDs below 2^24 cannot receive the same packed RGB value.
		const uint32 MeshOffset =
			GetTypeHash(StaticMesh.GetPathName()) & ColorMask;
		const uint32 PackedColor = (
			static_cast<uint32>(BranchID) * BranchPermutationMultiplier
			+ MeshOffset) & ColorMask;
		const float Red = MinimumChannelValue
			+ static_cast<float>(PackedColor & 0xffu) * ChannelValueScale;
		const float Green = MinimumChannelValue
			+ static_cast<float>((PackedColor >> 8) & 0xffu) * ChannelValueScale;
		const float Blue = MinimumChannelValue
			+ static_cast<float>((PackedColor >> 16) & 0xffu) * ChannelValueScale;
		return FVector4f(Red, Green, Blue, 1.0f);
	}

	double MeasureCenterlineLength(const TArray<FVector>& Centerline)
	{
		double Length = 0.0;
		for (int32 PointIndex = 1; PointIndex < Centerline.Num(); ++PointIndex)
		{
			Length += FVector::Distance(
				Centerline[PointIndex - 1],
				Centerline[PointIndex]);
		}
		return Length;
	}

	struct FSkeletonClosestLocation
	{
		int32 EdgeIndex = INDEX_NONE;
		double DistanceSquared = TNumericLimits<double>::Max();
	};

	struct FTrunkSkeletonPath
	{
		int32 TerminalEdgeIndex = INDEX_NONE;
		FVector StartPosition = FVector::ZeroVector;
		FVector EndPosition = FVector::ZeroVector;
		TArray<FVector> Polyline;
	};

	FVector CalculateTriangleAreaWeightedCenter(
		const TArray<FTreeTriangle>& Triangles,
		const TVertexAttributesConstRef<FVector3f>& VertexPositions,
		const TArray<int32>& TriangleIndices,
		const FVector& DefaultCenter)
	{
		FVector WeightedCenter = FVector::ZeroVector;
		double TotalArea = 0.0;
		for (const int32 TriangleIndex : TriangleIndices)
		{
			if (!Triangles.IsValidIndex(TriangleIndex))
			{
				continue;
			}
			const FTreeTriangle& Triangle = Triangles[TriangleIndex];
			const FVector FirstPosition(VertexPositions[Triangle.VertexIDs[0]]);
			const FVector SecondPosition(VertexPositions[Triangle.VertexIDs[1]]);
			const FVector ThirdPosition(VertexPositions[Triangle.VertexIDs[2]]);
			const double TriangleArea = FVector::CrossProduct(
				SecondPosition - FirstPosition,
				ThirdPosition - FirstPosition).Size() * 0.5;
			WeightedCenter += (
				FirstPosition + SecondPosition + ThirdPosition)
				/ 3.0 * TriangleArea;
			TotalArea += TriangleArea;
		}
		return TotalArea > MinimumGeometryScale
			? WeightedCenter / TotalArea
			: DefaultCenter;
	}

	void BuildHierarchyBoneRecord(
		const TArray<FTreeTriangle>& Triangles,
		const TVertexAttributesConstRef<FVector3f>& VertexPositions,
		const TArray<int32>& TriangleIndices,
		const FVector& PivotPosition,
		FFoliageBakerTreeHierarchyBoneRecord& BoneRecord)
	{
		BoneRecord.PivotPosition = PivotPosition;
		const FVector SurfaceCenter = CalculateTriangleAreaWeightedCenter(
			Triangles,
			VertexPositions,
			TriangleIndices,
			PivotPosition);
		BoneRecord.Axis = (SurfaceCenter - PivotPosition).GetSafeNormal();

		double MinimumProjection = TNumericLimits<double>::Max();
		double MaximumProjection = TNumericLimits<double>::Lowest();
		for (const int32 TriangleIndex : TriangleIndices)
		{
			if (!Triangles.IsValidIndex(TriangleIndex))
			{
				continue;
			}
			for (const FVertexID VertexID : Triangles[TriangleIndex].VertexIDs)
			{
				const double Projection = FVector::DotProduct(
					FVector(VertexPositions[VertexID]) - PivotPosition,
					BoneRecord.Axis);
				MinimumProjection = FMath::Min(MinimumProjection, Projection);
				MaximumProjection = FMath::Max(MaximumProjection, Projection);
			}
		}
		if (MinimumProjection <= MaximumProjection)
		{
			BoneRecord.MinimumAxisProjection = MinimumProjection;
			BoneRecord.PositiveAxisExtent = MaximumProjection;
			BoneRecord.AxisExtent = MaximumProjection - MinimumProjection;
		}
	}

	FSkeletonClosestLocation FindClosestSkeletonEdge(
		const FVector& Position,
		const TArray<FFoliageBakerTreeSkeletonEdge>& Edges)
	{
		FSkeletonClosestLocation Closest;
		for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
		{
			const TArray<FVector>& Polyline = Edges[EdgeIndex].Polyline;
			for (int32 PointIndex = 1; PointIndex < Polyline.Num(); ++PointIndex)
			{
				const double DistanceSquared = FVector::DistSquared(
					Position,
					FMath::ClosestPointOnSegment(
						Position,
						Polyline[PointIndex - 1],
						Polyline[PointIndex]));
				if (DistanceSquared < Closest.DistanceSquared)
				{
					Closest.EdgeIndex = EdgeIndex;
					Closest.DistanceSquared = DistanceSquared;
				}
			}
		}
		return Closest;
	}

	FSkeletonClosestLocation FindClosestSkeletonEdgeForBranch(
		const FVector& Position,
		const TArray<FFoliageBakerTreeSkeletonEdge>& Edges,
		const bool bTrunk,
		const int32 BranchID)
	{
		FSkeletonClosestLocation Closest;
		for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
		{
			const FFoliageBakerTreeSkeletonEdge& Edge = Edges[EdgeIndex];
			if (Edge.bTrunk != bTrunk
				|| (!bTrunk && Edge.BranchID != BranchID))
			{
				continue;
			}
			for (int32 PointIndex = 1; PointIndex < Edge.Polyline.Num(); ++PointIndex)
			{
				const double DistanceSquared = FVector::DistSquared(
					Position,
					FMath::ClosestPointOnSegment(
						Position,
						Edge.Polyline[PointIndex - 1],
						Edge.Polyline[PointIndex]));
				if (DistanceSquared < Closest.DistanceSquared)
				{
					Closest.EdgeIndex = EdgeIndex;
					Closest.DistanceSquared = DistanceSquared;
				}
			}
		}
		return Closest;
	}

	bool IsSkeletonEdgeInBranch(
		const FFoliageBakerTreeSkeletonEdge& Edge,
		const int32 BranchID)
	{
		return !Edge.bTrunk && Edge.BranchID == BranchID;
	}

	bool FindBranchStartPosition(
		const FFoliageBakerTreeSkeletonResult& Skeleton,
		const int32 BranchID,
		FVector& OutStartPosition)
	{
		TSet<int32> BranchEndNodeIDs;
		for (const FFoliageBakerTreeSkeletonEdge& Edge : Skeleton.Edges)
		{
			if (IsSkeletonEdgeInBranch(Edge, BranchID))
			{
				BranchEndNodeIDs.Add(Edge.EndNodeID);
			}
		}
		for (const FFoliageBakerTreeSkeletonEdge& Edge : Skeleton.Edges)
		{
			if (IsSkeletonEdgeInBranch(Edge, BranchID)
				&& !BranchEndNodeIDs.Contains(Edge.StartNodeID)
				&& Skeleton.Nodes.IsValidIndex(Edge.StartNodeID))
			{
				OutStartPosition = Skeleton.Nodes[Edge.StartNodeID].Position;
				return true;
			}
		}
		return false;
	}

	FVector FindTerminalDirection(
		const FFoliageBakerTreeSkeletonEdge& Edge,
		const FVector& StartPosition,
		const FVector& EndPosition)
	{
		for (int32 PointIndex = Edge.Polyline.Num() - 1;
			PointIndex > 0;
			--PointIndex)
		{
			const FVector Direction = (
				Edge.Polyline[PointIndex]
					- Edge.Polyline[PointIndex - 1]).GetSafeNormal();
			if (!Direction.IsNearlyZero())
			{
				return Direction;
			}
		}
		return (EndPosition - StartPosition).GetSafeNormal();
	}

	bool BuildTrunkSkeletonPath(
		const FFoliageBakerTreeSkeletonResult& Skeleton,
		FTrunkSkeletonPath& OutPath)
	{
		TArray<TArray<int32>> OutgoingEdgeIndices;
		OutgoingEdgeIndices.SetNum(Skeleton.Nodes.Num());
		TArray<int32> IncomingEdgeIndices;
		IncomingEdgeIndices.Init(INDEX_NONE, Skeleton.Nodes.Num());
		for (int32 EdgeIndex = 0; EdgeIndex < Skeleton.Edges.Num(); ++EdgeIndex)
		{
			const FFoliageBakerTreeSkeletonEdge& Edge = Skeleton.Edges[EdgeIndex];
			if (!Edge.bTrunk)
			{
				continue;
			}
			if (OutgoingEdgeIndices.IsValidIndex(Edge.StartNodeID))
			{
				OutgoingEdgeIndices[Edge.StartNodeID].Add(EdgeIndex);
			}
			if (IncomingEdgeIndices.IsValidIndex(Edge.EndNodeID))
			{
				IncomingEdgeIndices[Edge.EndNodeID] = EdgeIndex;
			}
		}

		const int32 StartNodeID = Skeleton.RootNodeID;
		if (!Skeleton.Nodes.IsValidIndex(StartNodeID)
			|| !OutgoingEdgeIndices.IsValidIndex(StartNodeID)
			|| OutgoingEdgeIndices[StartNodeID].IsEmpty())
		{
			return false;
		}

		TArray<double> PathScoreByNode;
		PathScoreByNode.Init(TNumericLimits<double>::Lowest(), Skeleton.Nodes.Num());
		PathScoreByNode[StartNodeID] = 0.0;
		TArray<int32> PendingNodeIDs;
		PendingNodeIDs.Add(StartNodeID);
		for (int32 PendingIndex = 0;
			PendingIndex < PendingNodeIDs.Num();
			++PendingIndex)
		{
			const int32 NodeID = PendingNodeIDs[PendingIndex];
			for (const int32 EdgeIndex : OutgoingEdgeIndices[NodeID])
			{
				const FFoliageBakerTreeSkeletonEdge& Edge = Skeleton.Edges[EdgeIndex];
				const double EdgeLength = MeasureCenterlineLength(Edge.Polyline);
				const double CandidateScore =
					PathScoreByNode[NodeID] + EdgeLength;
				if (!PathScoreByNode.IsValidIndex(Edge.EndNodeID)
					|| CandidateScore <= PathScoreByNode[Edge.EndNodeID])
				{
					continue;
				}
				PathScoreByNode[Edge.EndNodeID] = CandidateScore;
				IncomingEdgeIndices[Edge.EndNodeID] = EdgeIndex;
				PendingNodeIDs.Add(Edge.EndNodeID);
			}
		}

		int32 EndNodeID = INDEX_NONE;
		double MaximumPathScore = TNumericLimits<double>::Lowest();
		for (const int32 NodeID : PendingNodeIDs)
		{
			if (!OutgoingEdgeIndices.IsValidIndex(NodeID)
				|| !OutgoingEdgeIndices[NodeID].IsEmpty()
				|| !PathScoreByNode.IsValidIndex(NodeID)
				|| PathScoreByNode[NodeID] <= MaximumPathScore)
			{
				continue;
			}
			EndNodeID = NodeID;
			MaximumPathScore = PathScoreByNode[NodeID];
		}
		if (!Skeleton.Nodes.IsValidIndex(EndNodeID)
			|| !IncomingEdgeIndices.IsValidIndex(EndNodeID)
			|| !Skeleton.Edges.IsValidIndex(IncomingEdgeIndices[EndNodeID]))
		{
			return false;
		}

		OutPath.TerminalEdgeIndex = IncomingEdgeIndices[EndNodeID];
		OutPath.StartPosition = Skeleton.Nodes[StartNodeID].Position;
		OutPath.EndPosition = Skeleton.Nodes[EndNodeID].Position;
		TArray<int32> PathEdgeIndices;
		int32 PathNodeID = EndNodeID;
		while (PathNodeID != StartNodeID)
		{
			const int32 PathEdgeIndex = IncomingEdgeIndices[PathNodeID];
			PathEdgeIndices.Add(PathEdgeIndex);
			PathNodeID = Skeleton.Edges[PathEdgeIndex].StartNodeID;
		}
		Algo::Reverse(PathEdgeIndices);
		for (const int32 PathEdgeIndex : PathEdgeIndices)
		{
			const TArray<FVector>& EdgePolyline =
				Skeleton.Edges[PathEdgeIndex].Polyline;
			const int32 FirstPointIndex = OutPath.Polyline.IsEmpty() ? 0 : 1;
			for (int32 PointIndex = FirstPointIndex;
				PointIndex < EdgePolyline.Num();
				++PointIndex)
			{
				OutPath.Polyline.Add(EdgePolyline[PointIndex]);
			}
		}
		return true;
	}

	void ExtendTrunkPathToSourceTerminal(
		const TArray<FTreeTriangle>& Triangles,
		const TVertexAttributesConstRef<FVector3f>& VertexPositions,
		const TArray<int32>& TriangleSkeletonEdgeIndices,
		const FFoliageBakerTreeSkeletonResult& Skeleton,
		FTrunkSkeletonPath& Path)
	{
		if (!Skeleton.Edges.IsValidIndex(Path.TerminalEdgeIndex))
		{
			return;
		}
		const FVector TerminalDirection = FindTerminalDirection(
			Skeleton.Edges[Path.TerminalEdgeIndex],
			Path.StartPosition,
			Path.EndPosition);
		if (TerminalDirection.IsNearlyZero())
		{
			return;
		}
		double TerminalExtension = 0.0;
		for (int32 TriangleIndex = 0;
			TriangleIndex < TriangleSkeletonEdgeIndices.Num();
			++TriangleIndex)
		{
			if (TriangleSkeletonEdgeIndices[TriangleIndex]
					!= Path.TerminalEdgeIndex
				|| !Triangles.IsValidIndex(TriangleIndex))
			{
				continue;
			}
			for (const FVertexID VertexID : Triangles[TriangleIndex].VertexIDs)
			{
				TerminalExtension = FMath::Max(
					TerminalExtension,
					FVector::DotProduct(
						FVector(VertexPositions[VertexID]) - Path.EndPosition,
						TerminalDirection));
			}
		}
		Path.EndPosition += TerminalDirection * TerminalExtension;
		if (!Path.Polyline.IsEmpty())
		{
			Path.Polyline.Last() = Path.EndPosition;
		}
	}

	TSharedPtr<FFoliageBakerTreeHierarchyPreviewData> BuildSkeletonPreviewData(
		UStaticMesh& StaticMesh,
		const int32 SourceLODIndex,
		const TArray<FTreeTriangle>& Triangles,
		const TVertexAttributesConstRef<FVector3f>& VertexPositions,
		const TArray<int32>& WoodTriangleIndices,
		const TArray<FTriangleComponent>& WoodTriangleComponents,
		const FFoliageBakerTreeSkeletonResult& Skeleton,
		const FVector& Pivot)
	{
		TSharedRef<FFoliageBakerTreeHierarchyPreviewData> PreviewData =
			MakeShared<FFoliageBakerTreeHierarchyPreviewData>();
		PreviewData->AssetName = StaticMesh.GetName();
		for (const FTreeTriangle& Triangle : Triangles)
		{
			for (const FVertexID VertexID : Triangle.VertexIDs)
			{
				const FVector Position(VertexPositions[VertexID]);
				if (!Position.ContainsNaN())
				{
					PreviewData->Bounds += Position;
				}
			}
		}
		PreviewData->SourceStaticMesh = &StaticMesh;
		PreviewData->SourceLODIndex = SourceLODIndex;
		PreviewData->SkeletonCellSize = Skeleton.CellSize;
		PreviewData->WoodVolumeComponentCount = Skeleton.OccupiedComponentCount;

		TMap<int32, int32> PreviewBranchIndexByID;
		const int32 TrunkPreviewBranchIndex = PreviewData->Branches.AddDefaulted();
		PreviewData->Branches[TrunkPreviewBranchIndex].BranchID = INDEX_NONE;
		PreviewData->Branches[TrunkPreviewBranchIndex].Color = FLinearColor::White;
		PreviewData->Branches[TrunkPreviewBranchIndex].Label = TEXT("Trunk");

		const double MinimumPreviewRadius = FMath::Max(
			MinimumBranchPreviewRadius,
			Skeleton.CellSize * 0.45);
		for (const FFoliageBakerTreeSkeletonEdge& SkeletonEdge : Skeleton.Edges)
		{
			if (SkeletonEdge.bTrunk)
			{
				continue;
			}
			const int32 BranchID = SkeletonEdge.BranchID;
			int32& BranchIndex = PreviewBranchIndexByID.FindOrAdd(
				BranchID,
				INDEX_NONE);
			if (BranchIndex == INDEX_NONE)
			{
				BranchIndex = PreviewData->Branches.AddDefaulted();
				FFoliageBakerTreeHierarchyPreviewBranch& Branch =
					PreviewData->Branches[BranchIndex];
				Branch.BranchID = BranchID;
				Branch.ParentBranchID = SkeletonEdge.ParentBranchID;
				Branch.Color = FLinearColor(MakeBranchColor(StaticMesh, BranchID));
				Branch.Label = FString::Printf(TEXT("ID %d"), BranchID);
			}
		}
		PreviewData->Branches[TrunkPreviewBranchIndex].BoneRecord.BoneID = 0;
		PreviewData->Branches[TrunkPreviewBranchIndex].BoneRecord.ParentBoneID = 0;
		for (int32 PreviewBranchIndex = 0;
			PreviewBranchIndex < PreviewData->Branches.Num();
			++PreviewBranchIndex)
		{
			FFoliageBakerTreeHierarchyPreviewBranch& Branch =
				PreviewData->Branches[PreviewBranchIndex];
			if (PreviewBranchIndex == TrunkPreviewBranchIndex)
			{
				continue;
			}
			Branch.BoneRecord.BoneID = Branch.BranchID + 1;
			Branch.BoneRecord.ParentBoneID = Branch.ParentBranchID == INDEX_NONE
				? 0
				: Branch.ParentBranchID + 1;
		}
		TArray<int32> ProvisionalPreviewBranchIndexByTriangle;
		ProvisionalPreviewBranchIndexByTriangle.Init(INDEX_NONE, Triangles.Num());
		TArray<int32> TriangleSkeletonEdgeIndices;
		TriangleSkeletonEdgeIndices.Init(INDEX_NONE, Triangles.Num());
		for (const int32 TriangleIndex : WoodTriangleIndices)
		{
			if (!Triangles.IsValidIndex(TriangleIndex))
			{
				continue;
			}
			const FTreeTriangle& Triangle = Triangles[TriangleIndex];
			const FVector Center = (
				FVector(VertexPositions[Triangle.VertexIDs[0]])
				+ FVector(VertexPositions[Triangle.VertexIDs[1]])
				+ FVector(VertexPositions[Triangle.VertexIDs[2]])) / 3.0;
			const FSkeletonClosestLocation Closest = FindClosestSkeletonEdge(
				Center,
				Skeleton.Edges);
			if (!Skeleton.Edges.IsValidIndex(Closest.EdgeIndex))
			{
				continue;
			}
			const FFoliageBakerTreeSkeletonEdge& Edge = Skeleton.Edges[Closest.EdgeIndex];
			const int32 BranchID = Edge.bTrunk
				? INDEX_NONE
				: Edge.BranchID;
			int32 PreviewBranchIndex = INDEX_NONE;
			if (Edge.bTrunk)
			{
				PreviewBranchIndex = TrunkPreviewBranchIndex;
			}
			else if (PreviewBranchIndexByID.Contains(BranchID))
			{
				PreviewBranchIndex = PreviewBranchIndexByID.FindChecked(BranchID);
			}
			if (PreviewData->Branches.IsValidIndex(PreviewBranchIndex))
			{
				ProvisionalPreviewBranchIndexByTriangle[TriangleIndex] =
					PreviewBranchIndex;
			}
		}

		TArray<TArray<double>> CandidateAreaByComponentAndPreviewBranch;
		CandidateAreaByComponentAndPreviewBranch.SetNum(
			WoodTriangleComponents.Num());
		int32 PivotWoodComponentIndex = INDEX_NONE;
		double PivotSurfaceDistanceSquared = TNumericLimits<double>::Max();
		for (int32 ComponentIndex = 0;
			ComponentIndex < WoodTriangleComponents.Num();
			++ComponentIndex)
		{
			TArray<double>& CandidateAreas =
				CandidateAreaByComponentAndPreviewBranch[ComponentIndex];
			CandidateAreas.Init(-1.0, PreviewData->Branches.Num());
			for (const int32 TriangleIndex :
				WoodTriangleComponents[ComponentIndex].TriangleIndices)
			{
				if (!Triangles.IsValidIndex(TriangleIndex))
				{
					continue;
				}
				const FTreeTriangle& Triangle = Triangles[TriangleIndex];
				const FVector A(VertexPositions[Triangle.VertexIDs[0]]);
				const FVector B(VertexPositions[Triangle.VertexIDs[1]]);
				const FVector C(VertexPositions[Triangle.VertexIDs[2]]);
				const double DistanceSquared = FVector::DistSquared(
					Pivot,
					FMath::ClosestPointOnTriangleToPoint(Pivot, A, B, C));
				if (DistanceSquared < PivotSurfaceDistanceSquared)
				{
					PivotSurfaceDistanceSquared = DistanceSquared;
					PivotWoodComponentIndex = ComponentIndex;
				}

				const int32 PreviewBranchIndex =
					ProvisionalPreviewBranchIndexByTriangle[TriangleIndex];
				if (!CandidateAreas.IsValidIndex(PreviewBranchIndex))
				{
					continue;
				}
				double& CandidateArea = CandidateAreas[PreviewBranchIndex];
				CandidateArea = FMath::Max(CandidateArea, 0.0)
					+ FVector::CrossProduct(B - A, C - A).Size() * 0.5;
			}
		}

		TArray<TArray<int32>> OwnedTriangleIndicesByPreviewBranchIndex;
		OwnedTriangleIndicesByPreviewBranchIndex.SetNum(
			PreviewData->Branches.Num());
		int32 MixedWoodComponentCount = 0;
		int32 ResolvedWoodComponentCount = 0;
		int32 ReassignedWoodTriangleCount = 0;
		for (int32 ComponentIndex = 0;
			ComponentIndex < WoodTriangleComponents.Num();
			++ComponentIndex)
		{
			const TArray<double>& CandidateAreas =
				CandidateAreaByComponentAndPreviewBranch[ComponentIndex];
			int32 CandidateCount = 0;
			int32 OwnerPreviewBranchIndex = INDEX_NONE;
			double OwnerArea = -1.0;
			for (int32 PreviewBranchIndex = 0;
				PreviewBranchIndex < CandidateAreas.Num();
				++PreviewBranchIndex)
			{
				const double CandidateArea = CandidateAreas[PreviewBranchIndex];
				if (CandidateArea < 0.0)
				{
					continue;
				}
				++CandidateCount;
				if (CandidateArea > OwnerArea)
				{
					OwnerArea = CandidateArea;
					OwnerPreviewBranchIndex = PreviewBranchIndex;
				}
			}
			if (ComponentIndex == PivotWoodComponentIndex)
			{
				OwnerPreviewBranchIndex = TrunkPreviewBranchIndex;
			}
			if (!PreviewData->Branches.IsValidIndex(OwnerPreviewBranchIndex))
			{
				continue;
			}
			++ResolvedWoodComponentCount;
			MixedWoodComponentCount += CandidateCount > 1 ? 1 : 0;
			const FFoliageBakerTreeHierarchyPreviewBranch& OwnerBranch =
				PreviewData->Branches[OwnerPreviewBranchIndex];
			for (const int32 TriangleIndex :
				WoodTriangleComponents[ComponentIndex].TriangleIndices)
			{
				if (!Triangles.IsValidIndex(TriangleIndex))
				{
					continue;
				}
				ReassignedWoodTriangleCount +=
					ProvisionalPreviewBranchIndexByTriangle[TriangleIndex]
						!= OwnerPreviewBranchIndex
						? 1
						: 0;
				const FTreeTriangle& Triangle = Triangles[TriangleIndex];
				PreviewData->Branches[OwnerPreviewBranchIndex].SourceTriangleIDs.Add(
					Triangle.TriangleID.GetValue());
				OwnedTriangleIndicesByPreviewBranchIndex[OwnerPreviewBranchIndex].Add(
					TriangleIndex);
				const FVector Center = (
					FVector(VertexPositions[Triangle.VertexIDs[0]])
						+ FVector(VertexPositions[Triangle.VertexIDs[1]])
						+ FVector(VertexPositions[Triangle.VertexIDs[2]])) / 3.0;
				const FSkeletonClosestLocation OwnerClosest =
					FindClosestSkeletonEdgeForBranch(
						Center,
						Skeleton.Edges,
						OwnerPreviewBranchIndex == TrunkPreviewBranchIndex,
						OwnerBranch.BranchID);
				TriangleSkeletonEdgeIndices[TriangleIndex] =
					OwnerClosest.EdgeIndex;
			}
		}
		UE_LOG(
			LogFoliageBakerTreeHierarchy,
			Display,
			TEXT("%s: consolidated wood ownership for %d of %d topology component(s): pivot component %d at %.6f cm, %d mixed candidate component(s), %d reassigned triangle(s)."),
			*StaticMesh.GetName(),
			ResolvedWoodComponentCount,
			WoodTriangleComponents.Num(),
			PivotWoodComponentIndex,
			FMath::Sqrt(PivotSurfaceDistanceSquared),
			MixedWoodComponentCount,
			ReassignedWoodTriangleCount);

		for (int32 PreviewBranchIndex = 0;
			PreviewBranchIndex < PreviewData->Branches.Num();
			++PreviewBranchIndex)
		{
			FFoliageBakerTreeHierarchyPreviewBranch& Branch =
				PreviewData->Branches[PreviewBranchIndex];
			const bool bTrunk = PreviewBranchIndex == TrunkPreviewBranchIndex;
			FVector PivotPosition = FVector::ZeroVector;
			TArray<FVector> PreviewPolyline;
			if (bTrunk)
			{
				FTrunkSkeletonPath TrunkPath;
				if (!BuildTrunkSkeletonPath(Skeleton, TrunkPath))
				{
					continue;
				}
				ExtendTrunkPathToSourceTerminal(
					Triangles,
					VertexPositions,
					TriangleSkeletonEdgeIndices,
					Skeleton,
					TrunkPath);
				PivotPosition = TrunkPath.StartPosition;
				Branch.StartPosition = TrunkPath.StartPosition;
				Branch.EndPosition = TrunkPath.EndPosition;
				PreviewPolyline = MoveTemp(TrunkPath.Polyline);
			}
			else if (FindBranchStartPosition(
				Skeleton,
				Branch.BranchID,
				PivotPosition))
			{
				Branch.StartPosition = PivotPosition;
			}
			else
			{
				continue;
			}
			BuildHierarchyBoneRecord(
				Triangles,
				VertexPositions,
				OwnedTriangleIndicesByPreviewBranchIndex[PreviewBranchIndex],
				PivotPosition,
				Branch.BoneRecord);
			if (!bTrunk)
			{
				Branch.EndPosition = PivotPosition
					+ Branch.BoneRecord.Axis
						* Branch.BoneRecord.PositiveAxisExtent;
				PreviewPolyline.Add(Branch.StartPosition);
				PreviewPolyline.Add(Branch.EndPosition);
			}
			Branch.LabelPosition =
				(Branch.StartPosition + Branch.EndPosition) * 0.5;

			FFoliageBakerTreeHierarchyPreviewNode& StartNode =
				PreviewData->SkeletonNodes.AddDefaulted_GetRef();
			const int32 StartNodeID = PreviewData->SkeletonNodes.Num() - 1;
			StartNode.NodeID = StartNodeID;
			StartNode.Position = Branch.StartPosition;
			StartNode.Radius = MinimumPreviewRadius;
			StartNode.Kind = bTrunk
				? EFoliageBakerTreeHierarchyPreviewNodeKind::Root
				: EFoliageBakerTreeHierarchyPreviewNodeKind::Fork;
			if (bTrunk)
			{
				PreviewData->RootNodeID = StartNodeID;
			}
			FFoliageBakerTreeHierarchyPreviewNode& EndNode =
				PreviewData->SkeletonNodes.AddDefaulted_GetRef();
			EndNode.NodeID = PreviewData->SkeletonNodes.Num() - 1;
			EndNode.ParentNodeID = StartNodeID;
			EndNode.Position = Branch.EndPosition;
			EndNode.Radius = MinimumPreviewRadius;
			EndNode.Kind = EFoliageBakerTreeHierarchyPreviewNodeKind::Tip;

			FFoliageBakerTreeHierarchyPreviewEdge& Edge =
				PreviewData->SkeletonEdges.AddDefaulted_GetRef();
			Edge.EdgeID = PreviewData->SkeletonEdges.Num() - 1;
			Edge.StartNodeID = StartNodeID;
			Edge.EndNodeID = EndNode.NodeID;
			Edge.BranchID = Branch.BranchID;
			Edge.ParentBranchID = Branch.ParentBranchID;
			Edge.bTrunk = bTrunk;
			Edge.Polyline = MoveTemp(PreviewPolyline);

			const double PreviewRadius = bTrunk
				? MinimumPreviewRadius * TrunkPreviewRadiusScale
				: MinimumPreviewRadius;
			for (int32 PointIndex = 1;
				PointIndex < Edge.Polyline.Num();
				++PointIndex)
			{
				const FVector& SegmentStart = Edge.Polyline[PointIndex - 1];
				const FVector& SegmentEnd = Edge.Polyline[PointIndex];
				if (SegmentStart.Equals(SegmentEnd, MinimumGeometryScale))
				{
					continue;
				}
				FFoliageBakerTreeHierarchyPreviewCylinder& Cylinder =
					Branch.Cylinders.AddDefaulted_GetRef();
				Cylinder.Start = SegmentStart;
				Cylinder.End = SegmentEnd;
				Cylinder.Radius = PreviewRadius;
			}
			FFoliageBakerTreeHierarchyPreviewJoint& StartJoint =
				Branch.Joints.AddDefaulted_GetRef();
			StartJoint.Position = Branch.StartPosition;
			StartJoint.Radius = MinimumPreviewRadius * JointPreviewRadiusScale;
			FFoliageBakerTreeHierarchyPreviewJoint& EndJoint =
				Branch.Joints.AddDefaulted_GetRef();
			EndJoint.Position = Branch.EndPosition;
			EndJoint.Radius = MinimumPreviewRadius * JointPreviewRadiusScale;
		}

		return PreviewData;
	}
}

FFoliageBakerTreeHierarchyAnalysisResult
FFoliageBakerTreeHierarchyBaker::Analyze(
	UStaticMesh& StaticMesh,
	const int32 SourceLODIndex,
	const int32 LeafMaterialIndex)
{
	FFoliageBakerTreeHierarchyAnalysisResult Result;
	if (SourceLODIndex < 0
		|| !StaticMesh.IsSourceModelValid(SourceLODIndex)
		|| !StaticMesh.IsMeshDescriptionValid(SourceLODIndex))
	{
		Result.Report = FString::Printf(
			TEXT("%s\n  failed: Source LOD %d has no editable MeshDescription."),
			*StaticMesh.GetName(),
			SourceLODIndex);
		return Result;
	}

	const FMeshDescription& SourceMeshDescription =
		*StaticMesh.GetMeshDescription(SourceLODIndex);
	TArray<FTreeTriangle> Triangles;
	FString Error;
	if (!ReadTriangles(StaticMesh, SourceMeshDescription, Triangles, Error))
	{
		Result.Report = FString::Printf(
			TEXT("%s\n  failed: %s"),
			*StaticMesh.GetName(),
			*Error);
		return Result;
	}

	TArray<int32> WoodTriangleIndices;
	int32 LeafTriangleCount = 0;
	for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
	{
		if (Triangles[TriangleIndex].MaterialIndex != LeafMaterialIndex)
		{
			WoodTriangleIndices.Add(TriangleIndex);
		}
		else
		{
			++LeafTriangleCount;
		}
	}
	if (LeafMaterialIndex == INDEX_NONE || LeafTriangleCount == 0)
	{
		Result.Report = FString::Printf(
			TEXT("%s\n  failed: Selected Leaf Material Section %d is not referenced by Source LOD %d."),
			*StaticMesh.GetName(),
			LeafMaterialIndex,
			SourceLODIndex);
		return Result;
	}
	const TArray<FTriangleComponent> WoodTriangleComponents =
		BuildTriangleComponents(Triangles, WoodTriangleIndices);
	TArray<int32> WoodComponentTriangleCounts;
	WoodComponentTriangleCounts.Reserve(WoodTriangleComponents.Num());
	int32 MaximumWoodComponentTriangleCount = 0;
	for (const FTriangleComponent& Component : WoodTriangleComponents)
	{
		WoodComponentTriangleCounts.Add(Component.TriangleIndices.Num());
		MaximumWoodComponentTriangleCount = FMath::Max(
			MaximumWoodComponentTriangleCount,
			Component.TriangleIndices.Num());
	}
	WoodComponentTriangleCounts.Sort();
	const int32 MedianWoodComponentTriangleCount =
		WoodComponentTriangleCounts.IsEmpty()
			? 0
			: WoodComponentTriangleCounts[WoodComponentTriangleCounts.Num() / 2];
	if (WoodTriangleComponents.IsEmpty())
	{
		Result.Report = FString::Printf(
			TEXT("%s\n  failed: No wood geometry remains after excluding Leaf Material Section %d."),
			*StaticMesh.GetName(),
			LeafMaterialIndex);
		return Result;
	}

	const TVertexAttributesConstRef<FVector3f> VertexPositions =
		SourceMeshDescription.GetVertexPositions();
	TArray<int32> WoodComponentIDByTriangle;
	WoodComponentIDByTriangle.Init(INDEX_NONE, Triangles.Num());
	for (int32 ComponentIndex = 0;
		ComponentIndex < WoodTriangleComponents.Num();
		++ComponentIndex)
	{
		for (const int32 TriangleIndex :
			WoodTriangleComponents[ComponentIndex].TriangleIndices)
		{
			if (WoodComponentIDByTriangle.IsValidIndex(TriangleIndex))
			{
				WoodComponentIDByTriangle[TriangleIndex] = ComponentIndex;
			}
		}
	}
	TArray<FFoliageBakerTreeSkeletonTriangle> SkeletonTriangles;
	SkeletonTriangles.Reserve(WoodTriangleIndices.Num());
	for (const int32 TriangleIndex : WoodTriangleIndices)
	{
		if (!Triangles.IsValidIndex(TriangleIndex))
		{
			continue;
		}
		const FTreeTriangle& Triangle = Triangles[TriangleIndex];
		FFoliageBakerTreeSkeletonTriangle& SkeletonTriangle =
			SkeletonTriangles.AddDefaulted_GetRef();
		SkeletonTriangle.SourceComponentID =
			WoodComponentIDByTriangle[TriangleIndex];
		SkeletonTriangle.A = FVector(VertexPositions[Triangle.VertexIDs[0]]);
		SkeletonTriangle.B = FVector(VertexPositions[Triangle.VertexIDs[1]]);
		SkeletonTriangle.C = FVector(VertexPositions[Triangle.VertexIDs[2]]);
	}
	UE_LOG(
		LogFoliageBakerTreeHierarchy,
		Display,
		TEXT("%s: starting global GPU skeletonization with %d wood triangles."),
		*StaticMesh.GetName(),
		SkeletonTriangles.Num());
	const double SkeletonStartSeconds = FPlatformTime::Seconds();
	const FVector TreePivot = FVector::ZeroVector;
	const FFoliageBakerTreeSkeletonResult Skeleton =
		FFoliageBakerTreeSkeleton::Build(
			SkeletonTriangles,
			TreePivot);
	UE_LOG(
		LogFoliageBakerTreeHierarchy,
		Display,
		TEXT("%s: global skeletonization finished in %.3f seconds: %s"),
		*StaticMesh.GetName(),
		FPlatformTime::Seconds() - SkeletonStartSeconds,
		*Skeleton.Report);
	if (!Skeleton.bSucceeded)
	{
		Result.Report = FString::Printf(
			TEXT("%s\n  failed: %s"),
			*StaticMesh.GetName(),
			*Skeleton.Report);
		return Result;
	}
	for (const FFoliageBakerTreeSkeletonEdge& Edge : Skeleton.Edges)
	{
		if (!Edge.bTrunk && Edge.BranchID != INDEX_NONE)
		{
			Result.BranchCount = FMath::Max(Result.BranchCount, Edge.BranchID + 1);
		}
	}

	Result.PreviewData = BuildSkeletonPreviewData(
		StaticMesh,
		SourceLODIndex,
		Triangles,
		VertexPositions,
		WoodTriangleIndices,
		WoodTriangleComponents,
		Skeleton,
		TreePivot);

	Result.bSucceeded = true;
	int32 BoneRecordCount = 0;
	int32 ZeroAxisBoneRecordCount = 0;
	double MaximumAxisExtent = 0.0;
	double MaximumBehindPivotProjection = 0.0;
	if (Result.PreviewData.IsValid())
	{
		for (const FFoliageBakerTreeHierarchyPreviewBranch& Branch :
			Result.PreviewData->Branches)
		{
			if (Branch.BoneRecord.BoneID == INDEX_NONE)
			{
				continue;
			}
			++BoneRecordCount;
			ZeroAxisBoneRecordCount += Branch.BoneRecord.Axis.IsNearlyZero()
				? 1
				: 0;
			MaximumAxisExtent = FMath::Max(
				MaximumAxisExtent,
				Branch.BoneRecord.AxisExtent);
			MaximumBehindPivotProjection = FMath::Max(
				MaximumBehindPivotProjection,
				FMath::Max(-Branch.BoneRecord.MinimumAxisProjection, 0.0));
		}
	}
	Result.Report = FString::Printf(
		TEXT("%s\n  analyzed LOD %d wood hierarchy with Leaf Material Section %d excluded: %d branch group(s), %d virtual bone(s), %d trunk/branch axis record(s) (%d zero-axis, maximum %.3f cm axis extent, %.3f cm behind-pivot projection), %d source wood component(s) (median %d, maximum %d triangles). No Leaf Cluster was created and no asset data was written.\n  %s"),
		*StaticMesh.GetName(),
		SourceLODIndex,
		LeafMaterialIndex,
		Result.BranchCount,
		Result.PreviewData.IsValid()
			? Result.PreviewData->SkeletonEdges.Num()
			: 0,
		BoneRecordCount,
		ZeroAxisBoneRecordCount,
		MaximumAxisExtent,
		MaximumBehindPivotProjection,
		WoodTriangleComponents.Num(),
		MedianWoodComponentTriangleCount,
		MaximumWoodComponentTriangleCount,
		*Skeleton.Report);
	return Result;
}

FFoliageBakerLeafOwnershipResolveResult
FFoliageBakerTreeHierarchyBaker::ResolveLeafOwnership(
	const UStaticMesh& StaticMesh,
	const int32 SourceLODIndex,
	const int32 LeafMaterialIndex,
	const FFoliageBakerTreeHierarchyPreviewData& AnalysisData,
	const TArray<FFoliageBakerLeafTemplateAnnotation>& Annotations)
{
	FFoliageBakerLeafOwnershipResolveResult Result;
	if (SourceLODIndex < 0
		|| !StaticMesh.IsSourceModelValid(SourceLODIndex)
		|| !StaticMesh.IsMeshDescriptionValid(SourceLODIndex))
	{
		Result.Report = FString::Printf(
			TEXT("%s\n  failed: Source LOD %d has no editable MeshDescription."),
			*StaticMesh.GetName(),
			SourceLODIndex);
		return Result;
	}

	const FMeshDescription& SourceMeshDescription =
		*StaticMesh.GetMeshDescription(SourceLODIndex);
	TArray<FTreeTriangle> Triangles;
	FString Error;
	if (!ReadTriangles(StaticMesh, SourceMeshDescription, Triangles, Error))
	{
		Result.Report = FString::Printf(
			TEXT("%s\n  failed: %s"),
			*StaticMesh.GetName(),
			*Error);
		return Result;
	}

	const FStaticMeshConstAttributes Attributes(SourceMeshDescription);
	const TVertexInstanceAttributesConstRef<FVector2f> VertexInstanceUVs =
		Attributes.GetVertexInstanceUVs();
	if (VertexInstanceUVs.GetNumChannels() <= 0)
	{
		Result.Report = FString::Printf(
			TEXT("%s\n  failed: Source LOD %d has no UV0 channel."),
			*StaticMesh.GetName(),
			SourceLODIndex);
		return Result;
	}
	const TVertexAttributesConstRef<FVector3f> VertexPositions =
		SourceMeshDescription.GetVertexPositions();

	TArray<int32> LeafTriangleIndices;
	TMap<int32, int32> TriangleIndexBySourceTriangleID;
	for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
	{
		const FTreeTriangle& Triangle = Triangles[TriangleIndex];
		TriangleIndexBySourceTriangleID.Add(
			Triangle.TriangleID.GetValue(),
			TriangleIndex);
		if (Triangle.MaterialIndex == LeafMaterialIndex)
		{
			LeafTriangleIndices.Add(TriangleIndex);
		}
	}
	const TArray<FTriangleComponent> PhysicalLeafComponents =
		BuildTriangleComponents(Triangles, LeafTriangleIndices);
	Result.PhysicalLeafClusterCount = PhysicalLeafComponents.Num();

	UE::Geometry::FDynamicMesh3 BranchSurfaceMesh;
	TMap<int32, int32> ParentBranchIDBySurfaceTriangleID;
	for (const FFoliageBakerTreeHierarchyPreviewBranch& Branch :
		AnalysisData.Branches)
	{
		for (const int32 SourceTriangleID : Branch.SourceTriangleIDs)
		{
			if (!TriangleIndexBySourceTriangleID.Contains(SourceTriangleID))
			{
				continue;
			}
			const FTreeTriangle& Triangle = Triangles[
				TriangleIndexBySourceTriangleID.FindChecked(SourceTriangleID)];
			const int32 FirstVertexID = BranchSurfaceMesh.AppendVertex(
				FVector3d(VertexPositions[Triangle.VertexIDs[0]]));
			const int32 SecondVertexID = BranchSurfaceMesh.AppendVertex(
				FVector3d(VertexPositions[Triangle.VertexIDs[1]]));
			const int32 ThirdVertexID = BranchSurfaceMesh.AppendVertex(
				FVector3d(VertexPositions[Triangle.VertexIDs[2]]));
			const int32 SurfaceTriangleID = BranchSurfaceMesh.AppendTriangle(
				FirstVertexID,
				SecondVertexID,
				ThirdVertexID);
			if (SurfaceTriangleID >= 0)
			{
				ParentBranchIDBySurfaceTriangleID.Add(
					SurfaceTriangleID,
					Branch.BranchID);
			}
		}
	}
	if (BranchSurfaceMesh.TriangleCount() == 0)
	{
		Result.Report = FString::Printf(
			TEXT("%s\n  failed: Analyze Hierarchy produced no trunk/branch surface triangles for leaf ownership."),
			*StaticMesh.GetName());
		return Result;
	}
	UE::Geometry::FDynamicMeshAABBTree3 BranchSurfaceSpatial(
		&BranchSurfaceMesh,
		true);

	for (const FTriangleComponent& Component : PhysicalLeafComponents)
	{
		bool bResolvedComponent = false;
		for (const FFoliageBakerLeafTemplateAnnotation& Annotation : Annotations)
		{
			TArray<int32> MatchingTriangleIndices;
			for (const int32 TriangleIndex : Component.TriangleIndices)
			{
				const FTreeTriangle& Triangle = Triangles[TriangleIndex];
				const FString TriangleSignature = MakeUVTriangleSignature(
					VertexInstanceUVs.Get(Triangle.VertexInstanceIDs[0], 0),
					VertexInstanceUVs.Get(Triangle.VertexInstanceIDs[1], 0),
					VertexInstanceUVs.Get(Triangle.VertexInstanceIDs[2], 0));
				if (Annotation.TriangleSignatures.Contains(TriangleSignature))
				{
					MatchingTriangleIndices.Add(TriangleIndex);
				}
			}
			if (MatchingTriangleIndices.IsEmpty())
			{
				continue;
			}

			bool bResolvedPivot = false;
			bool bResolvedTip = false;
			FVector PivotPosition = FVector::ZeroVector;
			FVector TipPosition = FVector::ZeroVector;
			for (const int32 TriangleIndex : MatchingTriangleIndices)
			{
				const FTreeTriangle& Triangle = Triangles[TriangleIndex];
				const TStaticArray<FVector2f, 3> TriangleUVs =
				{
					VertexInstanceUVs.Get(Triangle.VertexInstanceIDs[0], 0),
					VertexInstanceUVs.Get(Triangle.VertexInstanceIDs[1], 0),
					VertexInstanceUVs.Get(Triangle.VertexInstanceIDs[2], 0)
				};
				const TStaticArray<FVector, 3> TrianglePositions =
				{
					FVector(VertexPositions[Triangle.VertexIDs[0]]),
					FVector(VertexPositions[Triangle.VertexIDs[1]]),
					FVector(VertexPositions[Triangle.VertexIDs[2]])
				};
				if (!bResolvedPivot)
				{
					bResolvedPivot = ResolveUVPointPosition(
						Annotation.PivotUV,
						TriangleUVs,
						TrianglePositions,
						PivotPosition);
				}
				if (!bResolvedTip)
				{
					bResolvedTip = ResolveUVPointPosition(
						Annotation.TipUV,
						TriangleUVs,
						TrianglePositions,
						TipPosition);
				}
				if (bResolvedPivot && bResolvedTip)
				{
					break;
				}
			}
			if (!bResolvedPivot || !bResolvedTip)
			{
				continue;
			}

			FFoliageBakerResolvedLeafCluster& ResolvedCluster =
				Result.ResolvedLeafClusters.AddDefaulted_GetRef();
			ResolvedCluster.UVIslandSignature = Annotation.UVIslandSignature;
			ResolvedCluster.PivotPosition = PivotPosition;
			ResolvedCluster.TipPosition = TipPosition;
			for (const int32 TriangleIndex : MatchingTriangleIndices)
			{
				ResolvedCluster.SourceTriangleIDs.Add(
					Triangles[TriangleIndex].TriangleID.GetValue());
			}
			double NearestDistanceSquared = TNumericLimits<double>::Max();
			const int32 NearestSurfaceTriangleID =
				BranchSurfaceSpatial.FindNearestTriangle(
					FVector3d(PivotPosition),
					NearestDistanceSquared);
			if (ParentBranchIDBySurfaceTriangleID.Contains(
					NearestSurfaceTriangleID))
			{
				ResolvedCluster.ParentBranchID =
					ParentBranchIDBySurfaceTriangleID.FindChecked(
						NearestSurfaceTriangleID);
			}
			bResolvedComponent = true;
		}
		if (!bResolvedComponent)
		{
			++Result.UnresolvedLeafClusterCount;
		}
	}

	Result.ResolvedLeafClusterCount = Result.ResolvedLeafClusters.Num();
	Result.bSucceeded = true;
	Result.Report = FString::Printf(
		TEXT("%s\n  resolved Leaf Material Section %d on LOD %d: %d physical leaf component(s), %d resolved UV-template instance(s), %d physical component(s) without a complete matching Pivot/Tip annotation. Parent ownership used the resolved world-space Pivot nearest to the analyzed trunk/branch source surface. No asset data was written."),
		*StaticMesh.GetName(),
		LeafMaterialIndex,
		SourceLODIndex,
		Result.PhysicalLeafClusterCount,
		Result.ResolvedLeafClusterCount,
		Result.UnresolvedLeafClusterCount);
	return Result;
}

namespace
{
	constexpr uint16 WindDataParentIDBase = 1024;
	constexpr double WindDataGeometrySpanRangesCentimeters[] =
	{
		256.0,
		512.0,
		1024.0,
		2048.0,
		4096.0
	};
	constexpr EObjectFlags WindDataAssetFlags =
		RF_Public | RF_Standalone | RF_Transactional;

	struct FWindDataBoneRecord
	{
		int32 ParentID = 0;
		FVector PivotPosition = FVector::ZeroVector;
		FVector Axis = FVector::ZeroVector;
		double GeometryProjectionSpanCentimeters = 0.0;
	};

	double SelectWindDataGeometrySpanRangeCentimeters(
		const double MaximumGeometryProjectionSpanCentimeters)
	{
		for (const double RangeCentimeters :
			WindDataGeometrySpanRangesCentimeters)
		{
			if (MaximumGeometryProjectionSpanCentimeters <= RangeCentimeters)
			{
				return RangeCentimeters;
			}
		}
		return WindDataGeometrySpanRangesCentimeters[
			UE_ARRAY_COUNT(WindDataGeometrySpanRangesCentimeters) - 1];
	}

	FVector2f MakeWindDataTexelUV(
		const int32 BoneID,
		const int32 TextureWidth,
		const int32 TextureHeight)
	{
		return FVector2f(
			(static_cast<float>(BoneID % TextureWidth) + 0.5f)
				/ static_cast<float>(TextureWidth),
			(static_cast<float>(BoneID / TextureWidth) + 0.5f)
				/ static_cast<float>(TextureHeight));
	}

	uint8 EncodeWindDataUnitValue(const double Value)
	{
		return static_cast<uint8>(FMath::RoundToInt(FMath::Clamp(
			Value,
			0.0,
			1.0) * 255.0));
	}

	FColor EncodeWindDataAxis(
		const FWindDataBoneRecord& Record,
		const double GeometrySpanRangeCentimeters)
	{
		const FVector Axis = Record.Axis.GetSafeNormal();
		const double EncodedGeometryProjectionSpan =
			Record.GeometryProjectionSpanCentimeters
			/ GeometrySpanRangeCentimeters;
		return FColor(
			EncodeWindDataUnitValue(Axis.X * 0.5 + 0.5),
			EncodeWindDataUnitValue(Axis.Y * 0.5 + 0.5),
			EncodeWindDataUnitValue(Axis.Z * 0.5 + 0.5),
			EncodeWindDataUnitValue(EncodedGeometryProjectionSpan));
	}

	TStrongObjectPtr<UTexture2D> FindOrCreateWindDataTexture(
		const UStaticMesh& StaticMesh,
		const FString& AssetSuffix,
		FFoliageBakerAssetTransaction& AssetTransaction,
		FString& OutError)
	{
		OutError.Reset();
		const FString AssetName = StaticMesh.GetName() + AssetSuffix;
		const FString PackagePath = FPackageName::GetLongPackagePath(
			StaticMesh.GetOutermost()->GetName());
		const FString PackageName = PackagePath + TEXT("/") + AssetName;
		const FString ObjectPath = PackageName + TEXT(".") + AssetName;
		const TStrongObjectPtr<UObject> ExistingObject(
			StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath));
		TStrongObjectPtr<UTexture2D> Texture(
			Cast<UTexture2D>(ExistingObject.Get()));
		if (ExistingObject && !Texture)
		{
			OutError = FString::Printf(
				TEXT("Cannot bake wind data because %s is not a Texture2D."),
				*ObjectPath);
			return nullptr;
		}

		TStrongObjectPtr<UPackage> Package(
			Texture ? Texture->GetOutermost() : CreatePackage(*PackageName));
		if (!Package)
		{
			OutError = FString::Printf(
				TEXT("Could not create package %s."),
				*PackageName);
			return nullptr;
		}
		Package->FullyLoad();

		if (!Texture)
		{
			Texture.Reset(NewObject<UTexture2D>(
				Package.Get(),
				*AssetName,
				WindDataAssetFlags));
			if (!Texture)
			{
				OutError = FString::Printf(
					TEXT("Could not create Texture2D %s."),
					*AssetName);
				return nullptr;
			}
			AssetTransaction.Track(*Texture);
		}
		else if (!AssetTransaction.Snapshot(*Texture, OutError))
		{
			return nullptr;
		}
		else
		{
			Texture->Modify();
		}
		return Texture;
	}

	void ConfigureWindDataTexture(UTexture2D& Texture)
	{
		Texture.CompressionNone = true;
		Texture.DeferCompression = false;
		Texture.SRGB = false;
		Texture.MipGenSettings = TMGS_NoMipmaps;
		Texture.Filter = TF_Nearest;
		Texture.AddressX = TA_Clamp;
		Texture.AddressY = TA_Clamp;
		Texture.NeverStream = true;
	}
}

FFoliageBakerWindDataBakeResult
FFoliageBakerTreeHierarchyBaker::BakeWindData(
	UStaticMesh& StaticMesh,
	const int32 SourceLODIndex,
	const FFoliageBakerTreeHierarchyPreviewData& AnalysisData,
	const TArray<FFoliageBakerResolvedLeafCluster>& ResolvedLeafClusters)
{
	FFoliageBakerWindDataBakeResult Result;
	if (SourceLODIndex < 0
		|| !StaticMesh.IsSourceModelValid(SourceLODIndex)
		|| !StaticMesh.IsMeshDescriptionValid(SourceLODIndex))
	{
		Result.Report = FString::Printf(
			TEXT("%s\n  failed: Source LOD %d has no editable MeshDescription."),
			*StaticMesh.GetName(),
			SourceLODIndex);
		return Result;
	}

	const FMeshDescription& SourceMeshDescription =
		*StaticMesh.GetMeshDescription(SourceLODIndex);
	TArray<FTreeTriangle> Triangles;
	FString Error;
	if (!ReadTriangles(StaticMesh, SourceMeshDescription, Triangles, Error))
	{
		Result.Report = FString::Printf(
			TEXT("%s\n  failed: %s"),
			*StaticMesh.GetName(),
			*Error);
		return Result;
	}
	const TVertexAttributesConstRef<FVector3f> VertexPositions =
		SourceMeshDescription.GetVertexPositions();
	TMap<int32, int32> TriangleIndexBySourceTriangleID;
	for (int32 TriangleIndex = 0;
		TriangleIndex < Triangles.Num();
		++TriangleIndex)
	{
		TriangleIndexBySourceTriangleID.Add(
			Triangles[TriangleIndex].TriangleID.GetValue(),
			TriangleIndex);
	}

	TArray<int32> SortedBranchIndices;
	SortedBranchIndices.Reserve(AnalysisData.Branches.Num());
	int32 TrunkBranchIndex = INDEX_NONE;
	for (int32 BranchIndex = 0;
		BranchIndex < AnalysisData.Branches.Num();
		++BranchIndex)
	{
		const FFoliageBakerTreeHierarchyPreviewBranch& Branch =
			AnalysisData.Branches[BranchIndex];
		if (Branch.BranchID == INDEX_NONE)
		{
			TrunkBranchIndex = BranchIndex;
		}
		else
		{
			SortedBranchIndices.Add(BranchIndex);
		}
	}
	SortedBranchIndices.Sort(
		[&AnalysisData](const int32 FirstIndex, const int32 SecondIndex)
		{
			return AnalysisData.Branches[FirstIndex].BranchID
				< AnalysisData.Branches[SecondIndex].BranchID;
		});

	TArray<FWindDataBoneRecord> BoneRecords;
	TMap<int32, int32> BoneIDByBranchID;
	TMap<int32, int32> BoneIDBySourceTriangleID;
	FWindDataBoneRecord& TrunkRecord = BoneRecords.AddDefaulted_GetRef();
	if (AnalysisData.Branches.IsValidIndex(TrunkBranchIndex))
	{
		const FFoliageBakerTreeHierarchyPreviewBranch& TrunkBranch =
			AnalysisData.Branches[TrunkBranchIndex];
		TrunkRecord.PivotPosition = TrunkBranch.BoneRecord.PivotPosition;
		TrunkRecord.Axis = TrunkBranch.BoneRecord.Axis;
		TrunkRecord.GeometryProjectionSpanCentimeters =
			TrunkBranch.BoneRecord.AxisExtent;
		for (const int32 SourceTriangleID : TrunkBranch.SourceTriangleIDs)
		{
			BoneIDBySourceTriangleID.Add(SourceTriangleID, 0);
		}
	}

	for (const int32 BranchIndex : SortedBranchIndices)
	{
		const FFoliageBakerTreeHierarchyPreviewBranch& Branch =
			AnalysisData.Branches[BranchIndex];
		const int32 BoneID = BoneRecords.Num();
		BoneIDByBranchID.Add(Branch.BranchID, BoneID);
		FWindDataBoneRecord& Record = BoneRecords.AddDefaulted_GetRef();
		Record.PivotPosition = Branch.BoneRecord.PivotPosition;
		Record.Axis = Branch.BoneRecord.Axis;
		Record.GeometryProjectionSpanCentimeters =
			Branch.BoneRecord.AxisExtent;
		for (const int32 SourceTriangleID : Branch.SourceTriangleIDs)
		{
			BoneIDBySourceTriangleID.Add(SourceTriangleID, BoneID);
		}
	}
	for (const int32 BranchIndex : SortedBranchIndices)
	{
		const FFoliageBakerTreeHierarchyPreviewBranch& Branch =
			AnalysisData.Branches[BranchIndex];
		const int32 BoneID = BoneIDByBranchID.FindRef(Branch.BranchID);
		FWindDataBoneRecord& Record = BoneRecords[BoneID];
		Record.ParentID = Branch.ParentBranchID == INDEX_NONE
			? 0
			: BoneIDByBranchID.FindRef(Branch.ParentBranchID);
	}

	const int32 BranchBoneCount = BoneRecords.Num();
	for (const FFoliageBakerResolvedLeafCluster& LeafCluster :
		ResolvedLeafClusters)
	{
		const int32 BoneID = BoneRecords.Num();
		FWindDataBoneRecord& Record = BoneRecords.AddDefaulted_GetRef();
		Record.ParentID = LeafCluster.ParentBranchID == INDEX_NONE
			? 0
			: BoneIDByBranchID.FindRef(LeafCluster.ParentBranchID);
		Record.PivotPosition = LeafCluster.PivotPosition;
		const FVector PivotToTip =
			LeafCluster.TipPosition - LeafCluster.PivotPosition;
		Record.Axis = PivotToTip.GetSafeNormal();
		double MinimumProjection = TNumericLimits<double>::Max();
		double MaximumProjection = TNumericLimits<double>::Lowest();
		for (const int32 SourceTriangleID : LeafCluster.SourceTriangleIDs)
		{
			BoneIDBySourceTriangleID.Add(SourceTriangleID, BoneID);
			if (!TriangleIndexBySourceTriangleID.Contains(SourceTriangleID))
			{
				continue;
			}
			const FTreeTriangle& Triangle = Triangles[
				TriangleIndexBySourceTriangleID.FindChecked(SourceTriangleID)];
			for (const FVertexID VertexID : Triangle.VertexIDs)
			{
				const double Projection = FVector::DotProduct(
					FVector(VertexPositions[VertexID])
						- Record.PivotPosition,
					Record.Axis);
				MinimumProjection = FMath::Min(
					MinimumProjection,
					Projection);
				MaximumProjection = FMath::Max(
					MaximumProjection,
					Projection);
			}
		}
		if (MinimumProjection <= MaximumProjection)
		{
			Record.GeometryProjectionSpanCentimeters =
				MaximumProjection - MinimumProjection;
		}
	}

	const int32 BoneRecordCount = BoneRecords.Num();
	double MaximumGeometryProjectionSpanCentimeters = 0.0;
	for (const FWindDataBoneRecord& Record : BoneRecords)
	{
		MaximumGeometryProjectionSpanCentimeters = FMath::Max(
			MaximumGeometryProjectionSpanCentimeters,
			Record.GeometryProjectionSpanCentimeters);
	}
	const double WindDataGeometrySpanRangeCentimeters =
		SelectWindDataGeometrySpanRangeCentimeters(
			MaximumGeometryProjectionSpanCentimeters);
	const double WindDataControlLengthScaleMeters =
		WindDataGeometrySpanRangeCentimeters / 16.0;
	int32 SaturatedGeometryProjectionSpanCount = 0;
	for (const FWindDataBoneRecord& Record : BoneRecords)
	{
		SaturatedGeometryProjectionSpanCount +=
			Record.GeometryProjectionSpanCentimeters
				> WindDataGeometrySpanRangeCentimeters
			? 1
			: 0;
	}
	const int32 RequiredTexelCount = BoneRecordCount;
	const int32 TextureWidth = FMath::CeilToInt(
		FMath::Sqrt(static_cast<double>(RequiredTexelCount)));
	const int32 TextureHeight = FMath::DivideAndRoundUp(
		RequiredTexelCount,
		TextureWidth);
	const int32 TextureTexelCount = TextureWidth * TextureHeight;

	TArray<FFloat16Color> PivotPositionPixels;
	PivotPositionPixels.SetNum(TextureTexelCount);
	TArray<FColor> PivotAxisPixels;
	PivotAxisPixels.Init(FColor::Black, TextureTexelCount);
	for (int32 BoneID = 0; BoneID < BoneRecordCount; ++BoneID)
	{
		const FWindDataBoneRecord& Record = BoneRecords[BoneID];
		FFloat16Color& Pixel = PivotPositionPixels[BoneID];
		Pixel.R = static_cast<float>(Record.PivotPosition.X);
		Pixel.G = static_cast<float>(Record.PivotPosition.Y);
		Pixel.B = static_cast<float>(Record.PivotPosition.Z);
		Pixel.A.Encoded = static_cast<uint16>(
			Record.ParentID + WindDataParentIDBase);
		PivotAxisPixels[BoneID] = EncodeWindDataAxis(
			Record,
			WindDataGeometrySpanRangeCentimeters);
	}
	for (int32 TexelIndex = BoneRecordCount;
		TexelIndex < TextureTexelCount;
		++TexelIndex)
	{
		FFloat16Color& Pixel = PivotPositionPixels[TexelIndex];
		Pixel.R.SetZero();
		Pixel.G.SetZero();
		Pixel.B.SetZero();
		Pixel.A.SetZero();
	}

	FFoliageBakerAssetTransaction AssetTransaction;
	if (!AssetTransaction.Snapshot(StaticMesh, Error))
	{
		Result.Report = FString::Printf(
			TEXT("%s\n  failed: %s"),
			*StaticMesh.GetName(),
			*Error);
		return Result;
	}

	TStrongObjectPtr<UTexture2D> PivotPositionTexture =
		FindOrCreateWindDataTexture(
			StaticMesh,
			TEXT("_PivPos"),
			AssetTransaction,
			Error);
	if (!PivotPositionTexture)
	{
		Result.Report = FString::Printf(
			TEXT("%s\n  failed: %s"),
			*StaticMesh.GetName(),
			*Error);
		return Result;
	}
	PivotPositionTexture->PreEditChange(nullptr);
	PivotPositionTexture->Source.Init(
		TextureWidth,
		TextureHeight,
		1,
		1,
		TSF_RGBA16F,
		reinterpret_cast<const uint8*>(PivotPositionPixels.GetData()));
	PivotPositionTexture->CompressionSettings = TC_HDR;
	ConfigureWindDataTexture(*PivotPositionTexture);
	PivotPositionTexture->PostEditChange();
	PivotPositionTexture->MarkPackageDirty();

	TStrongObjectPtr<UTexture2D> PivotAxisTexture =
		FindOrCreateWindDataTexture(
			StaticMesh,
			FString::Printf(
				TEXT("_%.0f_PivAxis"),
				WindDataGeometrySpanRangeCentimeters),
			AssetTransaction,
			Error);
	if (!PivotAxisTexture)
	{
		Result.Report = FString::Printf(
			TEXT("%s\n  failed: %s"),
			*StaticMesh.GetName(),
			*Error);
		return Result;
	}
	PivotAxisTexture->PreEditChange(nullptr);
	PivotAxisTexture->Source.Init(
		TextureWidth,
		TextureHeight,
		1,
		1,
		TSF_BGRA8,
		reinterpret_cast<const uint8*>(PivotAxisPixels.GetData()));
	PivotAxisTexture->CompressionSettings = TC_VectorDisplacementmap;
	ConfigureWindDataTexture(*PivotAxisTexture);
	PivotAxisTexture->PostEditChange();
	PivotAxisTexture->MarkPackageDirty();

	StaticMesh.Modify();
	StaticMesh.ModifyMeshDescription(SourceLODIndex);
	StaticMesh.PreEditChange(nullptr);
	TArray<int32> MeshDescriptionLODIndicesToCommit;
	MeshDescriptionLODIndicesToCommit.Add(SourceLODIndex);
	FMeshDescription& MeshDescription =
		*StaticMesh.GetMeshDescription(SourceLODIndex);
	TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs =
		FStaticMeshAttributes(MeshDescription).GetVertexInstanceUVs();
	VertexInstanceUVs.SetNumChannels(FMath::Max(
		VertexInstanceUVs.GetNumChannels(),
		2));
	const FVector2f RootBoneUV = MakeWindDataTexelUV(
		0,
		TextureWidth,
		TextureHeight);
	for (const FVertexInstanceID VertexInstanceID :
		MeshDescription.VertexInstances().GetElementIDs())
	{
		VertexInstanceUVs.Set(VertexInstanceID, 1, RootBoneUV);
	}

	TSet<int32> AssignedSourceTriangleIDs;
	for (const FTreeTriangle& Triangle : Triangles)
	{
		const int32 SourceTriangleID = Triangle.TriangleID.GetValue();
		const bool bHasAssignedBone =
			BoneIDBySourceTriangleID.Contains(SourceTriangleID);
		const int32 BoneID = bHasAssignedBone
			? BoneIDBySourceTriangleID.FindRef(SourceTriangleID)
			: 0;
		if (bHasAssignedBone)
		{
			AssignedSourceTriangleIDs.Add(SourceTriangleID);
		}
		const FVector2f BoneUV = MakeWindDataTexelUV(
			BoneID,
			TextureWidth,
			TextureHeight);
		for (const FVertexInstanceID VertexInstanceID :
			Triangle.VertexInstanceIDs)
		{
			VertexInstanceUVs.Set(VertexInstanceID, 1, BoneUV);
		}
	}
	for (int32 LODIndex = 0;
		LODIndex < StaticMesh.GetNumSourceModels();
		++LODIndex)
	{
		if (LODIndex != SourceLODIndex
			&& StaticMesh.IsMeshDescriptionValid(LODIndex))
		{
			const FMeshDescription& OtherMeshDescription =
				*StaticMesh.GetMeshDescription(LODIndex);
			const TVertexInstanceAttributesConstRef<FVector2f> OtherUVs =
				FStaticMeshConstAttributes(OtherMeshDescription)
					.GetVertexInstanceUVs();
			if (OtherUVs.GetNumChannels() < 2)
			{
				StaticMesh.ModifyMeshDescription(LODIndex);
				FMeshDescription& MutableOtherMeshDescription =
					*StaticMesh.GetMeshDescription(LODIndex);
				FStaticMeshAttributes(MutableOtherMeshDescription)
					.GetVertexInstanceUVs()
					.SetNumChannels(2);
				MeshDescriptionLODIndicesToCommit.Add(LODIndex);
			}
		}

		FStaticMeshSourceModel& SourceModel =
			StaticMesh.GetSourceModel(LODIndex);
		SourceModel.BuildSettings.bGenerateLightmapUVs = true;
		SourceModel.BuildSettings.SrcLightmapIndex = 0;
		SourceModel.BuildSettings.DstLightmapIndex = 2;
	}
	StaticMesh.SetLightMapCoordinateIndex(2);

	UStaticMesh::FCommitMeshDescriptionParams CommitParams;
	CommitParams.bMarkPackageDirty = true;
	CommitParams.bUseHashAsGuid = false;
	for (const int32 LODIndex : MeshDescriptionLODIndicesToCommit)
	{
		StaticMesh.CommitMeshDescription(LODIndex, CommitParams);
	}
	StaticMesh.PostEditChange();
	StaticMesh.MarkPackageDirty();
	AssetTransaction.Commit();

	Result.bSucceeded = true;
	Result.BoneRecordCount = BoneRecordCount;
	Result.BranchBoneCount = BranchBoneCount;
	Result.LeafBoneCount = ResolvedLeafClusters.Num();
	Result.UnassignedTriangleCount =
		Triangles.Num() - AssignedSourceTriangleIDs.Num();
	Result.PivotPositionTexturePath = PivotPositionTexture->GetPathName();
	Result.PivotAxisTexturePath = PivotAxisTexture->GetPathName();
	Result.Report = FString::Printf(
		TEXT("%s\n  baked LOD %d wind data: UV1 bone texel centers, %d linked bone record(s) (%d trunk/branch, %d leaf), %d source triangle(s) without hierarchy ownership mapped to trunk BoneID 0. Every source LOD generates lightmap UVs from UV0 into UV2, and the lightmap coordinate index is fixed to UV2 so UV1 remains dedicated to wind data.\n  PivPos: %s (%dx%d RGBA16F; RGB object-space centimeters, A raw half bits = ParentID + 1024).\n  PivAxis: %s (%dx%d RGBA8; RGB = Axis * 0.5 + 0.5, A * %.0f cm = geometry projection span). Material control length must decode A * %.0f m, preserving the reference 6.25x geometry-span ratio before clamping. Maximum geometry projection span %.3f cm; %d bone record(s) saturated at the %.0f cm encoding range. RGBA8 length step %.3f cm, maximum rounding error %.3f cm."),
		*StaticMesh.GetName(),
		SourceLODIndex,
		Result.BoneRecordCount,
		Result.BranchBoneCount,
		Result.LeafBoneCount,
		Result.UnassignedTriangleCount,
		*Result.PivotPositionTexturePath,
		TextureWidth,
		TextureHeight,
		*Result.PivotAxisTexturePath,
		TextureWidth,
		TextureHeight,
		WindDataGeometrySpanRangeCentimeters,
		WindDataControlLengthScaleMeters,
		MaximumGeometryProjectionSpanCentimeters,
		SaturatedGeometryProjectionSpanCount,
		WindDataGeometrySpanRangeCentimeters,
		WindDataGeometrySpanRangeCentimeters / 255.0,
		WindDataGeometrySpanRangeCentimeters / 510.0);
	return Result;
}
