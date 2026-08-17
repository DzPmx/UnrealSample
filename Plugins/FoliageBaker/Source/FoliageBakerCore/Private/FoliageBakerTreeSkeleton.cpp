#include "FoliageBakerTreeSkeleton.h"
#include "FoliageBakerTreeSkeletonGpu.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "Implicit/Solidify.h"
#include "Implicit/SweepingMeshSDF.h"
#include "Spatial/FastWinding.h"
#include "Spatial/MeshAABBTree3.h"

namespace
{
	using UE::Geometry::FDynamicMesh3;
	using UE::Geometry::FAxisAlignedBox3d;
	using UE::Geometry::FVector3i;
	using UE::Geometry::TMeshAABBTree3;
	using UE::Geometry::TFastWindingTree;
	using UE::Geometry::TImplicitSolidify;
	using UE::Geometry::TSweepingMeshSDF;

	constexpr int32 TargetVoxelResolution = 800;
	constexpr int32 CpuFallbackVoxelResolution = 800;
	constexpr double OccupancyExpansionCellScale = 0.0;
	constexpr double CoverageRadiusScale = 1.6;
	constexpr double MinimumExtractedPathCellCount = 4.0;
	constexpr double MinimumExtractedPathRadiusScale = 5.0;
	constexpr double CenterlineSimplificationCellScale = 0.35;
	constexpr double MaximumComponentBridgeCellDistance = 3.5;
	constexpr double MaximumChildEndpointRatio = 0.3;
	constexpr double ComponentBridgeAmbiguityMargin = 0.45;
	constexpr int32 MinimumComponentInteriorCellCount = 3;
	constexpr int32 MinimumBridgeSupportSampleCount = 2;
	constexpr double SkeletonRootBandBaseDiameterScale = 2.0;
	constexpr double SkeletonMinimumRootBandLengthFraction = 0.02;
	constexpr double SkeletonMaximumRootBandLengthFraction = 0.08;
	constexpr double SkeletonRootMaximumRisePersistenceFraction = 0.25;

	struct FQueueEntry
	{
		int32 Index = INDEX_NONE;
		double Distance = TNumericLimits<double>::Max();
	};

	struct FQueueEntryLess
	{
		bool operator()(const FQueueEntry& First, const FQueueEntry& Second) const
		{
			return First.Distance == Second.Distance
				? First.Index < Second.Index
				: First.Distance < Second.Distance;
		}
	};

	struct FVolumeComponent
	{
		int32 OccupiedCellCount = 0;
		int32 InteriorCellCount = 0;
		int32 EndpointA = INDEX_NONE;
		int32 EndpointB = INDEX_NONE;
		double EndpointSpan = 0.0;
		FVector InteriorPositionSum = FVector::ZeroVector;
		TArray<int32> InteriorCellIndices;
	};

	struct FComponentContact
	{
		int32 ComponentA = INDEX_NONE;
		int32 ComponentB = INDEX_NONE;
		int32 CellA = INDEX_NONE;
		int32 CellB = INDEX_NONE;
		int32 SupportSampleCount = 0;
		double GapSquared = TNumericLimits<double>::Max();
	};

	struct FSelectedComponentBridge
	{
		int32 ParentComponentID = INDEX_NONE;
		int32 ChildComponentID = INDEX_NONE;
		int32 ParentCellIndex = INDEX_NONE;
		int32 ChildCellIndex = INDEX_NONE;
		double Score = TNumericLimits<double>::Max();
	};

	struct FBridgeNeighbor
	{
		int32 Index = INDEX_NONE;
		double Length = 0.0;
	};

	int32 LocalNeighborhoodIndex(
		const int32 OffsetX,
		const int32 OffsetY,
		const int32 OffsetZ)
	{
		return OffsetX + 1
			+ 3 * (OffsetY + 1)
			+ 9 * (OffsetZ + 1);
	}

	int32 GridIndex(const FVector3i& Coordinate, const FVector3i& Dimensions)
	{
		return Coordinate.X
			+ Dimensions.X * (Coordinate.Y + Dimensions.Y * Coordinate.Z);
	}

	FVector3i GridCoordinate(const int32 Index, const FVector3i& Dimensions)
	{
		const int32 PlaneSize = Dimensions.X * Dimensions.Y;
		const int32 Z = Index / PlaneSize;
		const int32 Remainder = Index - Z * PlaneSize;
		const int32 Y = Remainder / Dimensions.X;
		return FVector3i(Remainder - Y * Dimensions.X, Y, Z);
	}

	bool IsInsideGrid(const FVector3i& Coordinate, const FVector3i& Dimensions)
	{
		return Coordinate.X >= 0 && Coordinate.Y >= 0 && Coordinate.Z >= 0
			&& Coordinate.X < Dimensions.X
			&& Coordinate.Y < Dimensions.Y
			&& Coordinate.Z < Dimensions.Z;
	}

	FVector GridPosition(
		const int32 Index,
		const FVector3i& Dimensions,
		const FVector3f& Origin,
		const double CellSize)
	{
		const FVector3i Coordinate = GridCoordinate(Index, Dimensions);
		return FVector(Origin)
			+ FVector(Coordinate) * CellSize;
	}

	uint64 ComponentPairKey(const int32 ComponentA, const int32 ComponentB)
	{
		const uint32 MinimumID = static_cast<uint32>(FMath::Min(ComponentA, ComponentB));
		const uint32 MaximumID = static_cast<uint32>(FMath::Max(ComponentA, ComponentB));
		return (static_cast<uint64>(MinimumID) << 32)
			| static_cast<uint64>(MaximumID);
	}

	double ComponentEndpointRatio(
		const int32 CellIndex,
		const FVolumeComponent& Component,
		const FVector3i& Dimensions,
		const FVector3f& GridOrigin,
		const double CellSize)
	{
		if (Component.EndpointA == INDEX_NONE
			|| Component.EndpointB == INDEX_NONE
			|| Component.EndpointSpan <= CellSize)
		{
			return TNumericLimits<double>::Max();
		}
		const FVector Position = GridPosition(
			CellIndex,
			Dimensions,
			GridOrigin,
			CellSize);
		const FVector EndpointA = GridPosition(
			Component.EndpointA,
			Dimensions,
			GridOrigin,
			CellSize);
		const FVector EndpointB = GridPosition(
			Component.EndpointB,
			Dimensions,
			GridOrigin,
			CellSize);
		return FMath::Min(
			FVector::Distance(Position, EndpointA),
			FVector::Distance(Position, EndpointB))
			/ Component.EndpointSpan;
	}

	bool IsVolumeBoundaryCell(
		const int32 Index,
		const FVector3i& Dimensions,
		const TBitArray<>& Volume)
	{
		const FVector3i Coordinate = GridCoordinate(Index, Dimensions);
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			for (int32 Direction = -1; Direction <= 1; Direction += 2)
			{
				FVector3i Neighbor = Coordinate;
				Neighbor[Axis] += Direction;
				if (!IsInsideGrid(Neighbor, Dimensions)
					|| !Volume[GridIndex(Neighbor, Dimensions)])
				{
					return true;
				}
			}
		}
		return false;
	}

	int32 CountLocalNeighborhoodComponents(
		const TBitArray<>& LocalValues,
		const bool bUseFaceConnectivity)
	{
		bool Visited[27] = {};
		int32 ComponentCount = 0;
		TArray<int32> Queue;
		for (int32 SeedIndex = 0; SeedIndex < 27; ++SeedIndex)
		{
			if (!LocalValues[SeedIndex] || Visited[SeedIndex])
			{
				continue;
			}
			++ComponentCount;
			if (ComponentCount > 1)
			{
				return ComponentCount;
			}
			Queue.Reset();
			Queue.Add(SeedIndex);
			Visited[SeedIndex] = true;
			for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
			{
				const int32 LocalIndex = Queue[QueueIndex];
				const int32 LocalZ = LocalIndex / 9;
				const int32 LocalRemainder = LocalIndex - LocalZ * 9;
				const int32 LocalY = LocalRemainder / 3;
				const int32 LocalX = LocalRemainder - LocalY * 3;
				for (int32 OffsetZ = -1; OffsetZ <= 1; ++OffsetZ)
				{
					for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
					{
						for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
						{
							const int32 ManhattanDistance = FMath::Abs(OffsetX)
								+ FMath::Abs(OffsetY)
								+ FMath::Abs(OffsetZ);
							if (ManhattanDistance == 0
								|| (bUseFaceConnectivity && ManhattanDistance != 1))
							{
								continue;
							}
							const int32 NeighborX = LocalX + OffsetX;
							const int32 NeighborY = LocalY + OffsetY;
							const int32 NeighborZ = LocalZ + OffsetZ;
							if (NeighborX < 0 || NeighborX >= 3
								|| NeighborY < 0 || NeighborY >= 3
								|| NeighborZ < 0 || NeighborZ >= 3)
							{
								continue;
							}
							const int32 NeighborIndex = NeighborX
								+ 3 * NeighborY
								+ 9 * NeighborZ;
							if (!LocalValues[NeighborIndex]
								|| Visited[NeighborIndex])
							{
								continue;
							}
							Visited[NeighborIndex] = true;
							Queue.Add(NeighborIndex);
						}
					}
				}
			}
		}
		return ComponentCount;
	}

	bool IsSimpleVolumeCell(
		const int32 Index,
		const FVector3i& Dimensions,
		const TBitArray<>& Volume)
	{
		const FVector3i Coordinate = GridCoordinate(Index, Dimensions);
		TBitArray<> ObjectNeighborhood(false, 27);
		TBitArray<> BackgroundNeighborhood(false, 27);
		int32 ObjectNeighborCount = 0;
		for (int32 OffsetZ = -1; OffsetZ <= 1; ++OffsetZ)
		{
			for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
			{
				for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
				{
					const int32 LocalIndex = LocalNeighborhoodIndex(
						OffsetX,
						OffsetY,
						OffsetZ);
					const bool bCenter = OffsetX == 0
						&& OffsetY == 0
						&& OffsetZ == 0;
					const FVector3i Neighbor = Coordinate
						+ FVector3i(OffsetX, OffsetY, OffsetZ);
					const bool bObject = !bCenter
						&& IsInsideGrid(Neighbor, Dimensions)
						&& Volume[GridIndex(Neighbor, Dimensions)];
					ObjectNeighborhood[LocalIndex] = bObject;
					BackgroundNeighborhood[LocalIndex] = !bObject;
					if (bObject)
					{
						++ObjectNeighborCount;
					}
				}
			}
		}
		if (ObjectNeighborCount <= 1)
		{
			return false;
		}
		return CountLocalNeighborhoodComponents(ObjectNeighborhood, false) == 1
			&& CountLocalNeighborhoodComponents(BackgroundNeighborhood, true) == 1;
	}



	void ThinWoodVolume(
		const FVector3i& Dimensions,
		const TSweepingMeshSDF<FDynamicMesh3>& SDF,
		const TBitArray<>& Anchors,
		TBitArray<>& Volume)
	{
		TArray<FQueueEntry> Queue;
		const FQueueEntryLess QueueEntryLess;
		TBitArray<> Queued(false, Volume.Num());
		for (int32 Index = 0; Index < Volume.Num(); ++Index)
		{
			if (!Volume[Index]
				|| Anchors[Index]
				|| !IsVolumeBoundaryCell(Index, Dimensions, Volume))
			{
				continue;
			}
			Queue.HeapPush(
				FQueueEntry{Index, -static_cast<double>(SDF.Grid[Index])},
				QueueEntryLess);
			Queued[Index] = true;
		}
		while (!Queue.IsEmpty())
		{
			FQueueEntry Entry;
			Queue.HeapPop(Entry, QueueEntryLess, EAllowShrinking::No);
			Queued[Entry.Index] = false;
			if (!Volume[Entry.Index]
				|| Anchors[Entry.Index]
				|| !IsVolumeBoundaryCell(Entry.Index, Dimensions, Volume)
				|| !IsSimpleVolumeCell(Entry.Index, Dimensions, Volume))
			{
				continue;
			}
			Volume[Entry.Index] = false;
			const FVector3i Coordinate = GridCoordinate(Entry.Index, Dimensions);
			for (int32 OffsetZ = -1; OffsetZ <= 1; ++OffsetZ)
			{
				for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
				{
					for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
					{
						if (OffsetX == 0 && OffsetY == 0 && OffsetZ == 0)
						{
							continue;
						}
						const FVector3i Neighbor = Coordinate
							+ FVector3i(OffsetX, OffsetY, OffsetZ);
						if (!IsInsideGrid(Neighbor, Dimensions))
						{
							continue;
						}
						const int32 NeighborIndex = GridIndex(Neighbor, Dimensions);
						if (!Volume[NeighborIndex]
							|| Anchors[NeighborIndex]
							|| Queued[NeighborIndex]
							|| !IsVolumeBoundaryCell(NeighborIndex, Dimensions, Volume))
						{
							continue;
						}
						Queue.HeapPush(
							FQueueEntry{
								NeighborIndex,
								-static_cast<double>(SDF.Grid[NeighborIndex])},
							QueueEntryLess);
						Queued[NeighborIndex] = true;
					}
				}
			}
		}
	}

	void BuildRootedSkeletonTree(
		const int32 RootIndex,
		const FVector3i& Dimensions,
		const FVector3f& GridOrigin,
		const double CellSize,
		const TBitArray<>& Skeleton,
		const TArray<int32>& ComponentIDs,
		const TMultiMap<int32, FBridgeNeighbor>& BridgeNeighbors,
		TArray<double>& OutDistances,
		TArray<int32>& OutPredecessors)
	{
		OutDistances.Init(TNumericLimits<double>::Max(), Skeleton.Num());
		OutPredecessors.Init(INDEX_NONE, Skeleton.Num());
		if (RootIndex < 0 || RootIndex >= Skeleton.Num() || !Skeleton[RootIndex])
		{
			return;
		}
		TArray<FQueueEntry> Queue;
		const FQueueEntryLess QueueEntryLess;
		OutDistances[RootIndex] = 0.0;
		Queue.HeapPush(FQueueEntry{RootIndex, 0.0}, QueueEntryLess);
		while (!Queue.IsEmpty())
		{
			FQueueEntry Entry;
			Queue.HeapPop(Entry, QueueEntryLess, EAllowShrinking::No);
			if (Entry.Distance > OutDistances[Entry.Index])
			{
				continue;
			}
			const FVector3i Coordinate = GridCoordinate(Entry.Index, Dimensions);
			for (int32 OffsetZ = -1; OffsetZ <= 1; ++OffsetZ)
			{
				for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
				{
					for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
					{
						if (OffsetX == 0 && OffsetY == 0 && OffsetZ == 0)
						{
							continue;
						}
						const FVector3i Neighbor = Coordinate
							+ FVector3i(OffsetX, OffsetY, OffsetZ);
						if (!IsInsideGrid(Neighbor, Dimensions))
						{
							continue;
						}
						const int32 NeighborIndex = GridIndex(Neighbor, Dimensions);
						if (!Skeleton[NeighborIndex]
							|| ComponentIDs[NeighborIndex] != ComponentIDs[Entry.Index])
						{
							continue;
						}
						const double StepLength = CellSize * FVector(
							static_cast<double>(OffsetX),
							static_cast<double>(OffsetY),
							static_cast<double>(OffsetZ)).Size();
						const double CandidateDistance = Entry.Distance + StepLength;
						if (CandidateDistance >= OutDistances[NeighborIndex])
						{
							continue;
						}
						OutDistances[NeighborIndex] = CandidateDistance;
						OutPredecessors[NeighborIndex] = Entry.Index;
						Queue.HeapPush(
							FQueueEntry{NeighborIndex, CandidateDistance},
							QueueEntryLess);
					}
				}
			}
			TArray<FBridgeNeighbor> CrossComponentNeighbors;
			BridgeNeighbors.MultiFind(Entry.Index, CrossComponentNeighbors);
			for (const FBridgeNeighbor& BridgeNeighbor : CrossComponentNeighbors)
			{
				if (BridgeNeighbor.Index < 0
					|| BridgeNeighbor.Index >= Skeleton.Num()
					|| !Skeleton[BridgeNeighbor.Index])
				{
					continue;
				}
				const double CandidateDistance = Entry.Distance + BridgeNeighbor.Length;
				if (CandidateDistance >= OutDistances[BridgeNeighbor.Index])
				{
					continue;
				}
				OutDistances[BridgeNeighbor.Index] = CandidateDistance;
				OutPredecessors[BridgeNeighbor.Index] = Entry.Index;
				Queue.HeapPush(
					FQueueEntry{BridgeNeighbor.Index, CandidateDistance},
					QueueEntryLess);
			}
		}
	}

	double PointSegmentDistanceSquared(
		const FVector& Point,
		const FVector& Start,
		const FVector& End)
	{
		return FVector::DistSquared(
			Point,
			FMath::ClosestPointOnSegment(Point, Start, End));
	}

	void SimplifyPolylineRecursive(
		const TArray<FVector>& Points,
		const int32 FirstIndex,
		const int32 LastIndex,
		const double ToleranceSquared,
		TBitArray<>& KeepPoints)
	{
		if (LastIndex <= FirstIndex + 1)
		{
			return;
		}

		double MaximumDistanceSquared = 0.0;
		int32 MaximumIndex = INDEX_NONE;
		for (int32 PointIndex = FirstIndex + 1; PointIndex < LastIndex; ++PointIndex)
		{
			const double DistanceSquared = PointSegmentDistanceSquared(
				Points[PointIndex],
				Points[FirstIndex],
				Points[LastIndex]);
			if (DistanceSquared > MaximumDistanceSquared)
			{
				MaximumDistanceSquared = DistanceSquared;
				MaximumIndex = PointIndex;
			}
		}
		if (MaximumIndex == INDEX_NONE || MaximumDistanceSquared <= ToleranceSquared)
		{
			return;
		}

		KeepPoints[MaximumIndex] = true;
		SimplifyPolylineRecursive(
			Points,
			FirstIndex,
			MaximumIndex,
			ToleranceSquared,
			KeepPoints);
		SimplifyPolylineRecursive(
			Points,
			MaximumIndex,
			LastIndex,
			ToleranceSquared,
			KeepPoints);
	}

	void SmoothAndSimplifyPolyline(
		TArray<FVector>& Polyline,
		const double CellSize)
	{
		if (Polyline.Num() <= 2)
		{
			return;
		}

		for (int32 PassIndex = 0; PassIndex < 2; ++PassIndex)
		{
			TArray<FVector> Smoothed = Polyline;
			for (int32 PointIndex = 1; PointIndex + 1 < Polyline.Num(); ++PointIndex)
			{
				Smoothed[PointIndex] = (
					Polyline[PointIndex - 1]
					+ Polyline[PointIndex] * 2.0
					+ Polyline[PointIndex + 1]) / 4.0;
			}
			Polyline = MoveTemp(Smoothed);
		}

		TBitArray<> KeepPoints(false, Polyline.Num());
		KeepPoints[0] = true;
		KeepPoints[Polyline.Num() - 1] = true;
		SimplifyPolylineRecursive(
			Polyline,
			0,
			Polyline.Num() - 1,
			FMath::Square(CellSize * CenterlineSimplificationCellScale),
			KeepPoints);
		TArray<FVector> Simplified;
		for (int32 PointIndex = 0; PointIndex < Polyline.Num(); ++PointIndex)
		{
			if (KeepPoints[PointIndex])
			{
				Simplified.Add(Polyline[PointIndex]);
			}
		}
		Polyline = MoveTemp(Simplified);
	}

	double PolylineLength(const TArray<FVector>& Polyline)
	{
		double Length = 0.0;
		for (int32 PointIndex = 1; PointIndex < Polyline.Num(); ++PointIndex)
		{
			Length += FVector::Distance(
				Polyline[PointIndex - 1],
				Polyline[PointIndex]);
		}
		return Length;
	}

	struct FSkeletonEdgeLocation
	{
		int32 EdgeIndex = INDEX_NONE;
		double DistanceSquared = TNumericLimits<double>::Max();
	};

	FSkeletonEdgeLocation FindClosestSkeletonEdgeLocation(
		const FVector& Position,
		const TArray<FFoliageBakerTreeSkeletonEdge>& Edges)
	{
		FSkeletonEdgeLocation Result;
		for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
		{
			const FFoliageBakerTreeSkeletonEdge& Edge = Edges[EdgeIndex];
			for (int32 SegmentIndex = 0;
				SegmentIndex + 1 < Edge.Polyline.Num();
				++SegmentIndex)
			{
				const FVector Candidate = FMath::ClosestPointOnSegment(
					Position,
					Edge.Polyline[SegmentIndex],
					Edge.Polyline[SegmentIndex + 1]);
				const double DistanceSquared = FVector::DistSquared(
					Position,
					Candidate);
				if (DistanceSquared < Result.DistanceSquared)
				{
					Result.EdgeIndex = EdgeIndex;
					Result.DistanceSquared = DistanceSquared;
				}
			}
		}
		return Result;
	}

	double MeasureDominantPathScore(
		const int32 NodeID,
		const TArray<TArray<int32>>& OutgoingEdges,
		const TArray<FFoliageBakerTreeSkeletonEdge>& Edges,
		TArray<double>& Scores)
	{
		if (Scores.IsValidIndex(NodeID) && Scores[NodeID] >= 0.0)
		{
			return Scores[NodeID];
		}
		double Score = 0.0;
		if (OutgoingEdges.IsValidIndex(NodeID))
		{
			for (const int32 EdgeIndex : OutgoingEdges[NodeID])
			{
				if (!Edges.IsValidIndex(EdgeIndex))
				{
					continue;
				}
				const FFoliageBakerTreeSkeletonEdge& Edge = Edges[EdgeIndex];
				const double CandidateScore = PolylineLength(Edge.Polyline)
					* FMath::Square(FMath::Max(Edge.Radius, 1.0))
					+ MeasureDominantPathScore(
						Edge.EndNodeID,
						OutgoingEdges,
						Edges,
						Scores);
				Score = FMath::Max(Score, CandidateScore);
			}
		}
		if (Scores.IsValidIndex(NodeID))
		{
			Scores[NodeID] = Score;
		}
		return Score;
	}

	double MeasureSubtreeMaximumProjection(
		const int32 NodeID,
		const FVector& Pivot,
		const FVector& TrunkAxis,
		const TArray<TArray<int32>>& OutgoingEdges,
		const TArray<FFoliageBakerTreeSkeletonNode>& Nodes,
		const TArray<FFoliageBakerTreeSkeletonEdge>& Edges)
	{
		double MaximumProjection = Nodes.IsValidIndex(NodeID)
			? FVector::DotProduct(Nodes[NodeID].Position - Pivot, TrunkAxis)
			: 0.0;
		if (!OutgoingEdges.IsValidIndex(NodeID))
		{
			return MaximumProjection;
		}
		for (const int32 EdgeIndex : OutgoingEdges[NodeID])
		{
			if (!Edges.IsValidIndex(EdgeIndex))
			{
				continue;
			}
			MaximumProjection = FMath::Max(
				MaximumProjection,
				MeasureSubtreeMaximumProjection(
					Edges[EdgeIndex].EndNodeID,
					Pivot,
					TrunkAxis,
					OutgoingEdges,
					Nodes,
					Edges));
		}
		return MaximumProjection;
	}

	double MeasureSubtreePersistenceLength(
		const int32 NodeID,
		const TArray<TArray<int32>>& OutgoingEdges,
		const TArray<FFoliageBakerTreeSkeletonEdge>& Edges)
	{
		double MaximumLength = 0.0;
		if (!OutgoingEdges.IsValidIndex(NodeID))
		{
			return MaximumLength;
		}
		for (const int32 EdgeIndex : OutgoingEdges[NodeID])
		{
			if (!Edges.IsValidIndex(EdgeIndex))
			{
				continue;
			}
			const FFoliageBakerTreeSkeletonEdge& Edge = Edges[EdgeIndex];
			MaximumLength = FMath::Max(
				MaximumLength,
				PolylineLength(Edge.Polyline)
					+ MeasureSubtreePersistenceLength(
						Edge.EndNodeID,
						OutgoingEdges,
						Edges));
		}
		return MaximumLength;
	}

	void MarkRootSubtreeAsTrunk(
		const int32 EdgeIndex,
		const TArray<TArray<int32>>& OutgoingEdges,
		TArray<FFoliageBakerTreeSkeletonEdge>& Edges)
	{
		if (!Edges.IsValidIndex(EdgeIndex) || Edges[EdgeIndex].bTrunk)
		{
			return;
		}
		FFoliageBakerTreeSkeletonEdge& Edge = Edges[EdgeIndex];
		Edge.bTrunk = true;
		Edge.BranchID = INDEX_NONE;
		Edge.ParentBranchID = INDEX_NONE;
		if (!OutgoingEdges.IsValidIndex(Edge.EndNodeID))
		{
			return;
		}
		for (const int32 ChildEdgeIndex : OutgoingEdges[Edge.EndNodeID])
		{
			MarkRootSubtreeAsTrunk(ChildEdgeIndex, OutgoingEdges, Edges);
		}
	}

	void AssignBranchSubtree(
		const int32 EdgeIndex,
		const int32 BranchID,
		const int32 ParentBranchID,
		const TArray<TArray<int32>>& OutgoingEdges,
		TArray<FFoliageBakerTreeSkeletonEdge>& Edges)
	{
		if (!Edges.IsValidIndex(EdgeIndex)
			|| Edges[EdgeIndex].bTrunk)
		{
			return;
		}
		FFoliageBakerTreeSkeletonEdge& Edge = Edges[EdgeIndex];
		Edge.BranchID = BranchID;
		Edge.ParentBranchID = ParentBranchID;
		if (!OutgoingEdges.IsValidIndex(Edge.EndNodeID))
		{
			return;
		}
		const TArray<int32>& ChildEdges = OutgoingEdges[Edge.EndNodeID];
		for (const int32 ChildEdgeIndex : ChildEdges)
		{
			if (!Edges.IsValidIndex(ChildEdgeIndex)
				|| Edges[ChildEdgeIndex].bTrunk)
			{
				continue;
			}
			AssignBranchSubtree(
				ChildEdgeIndex,
				BranchID,
				ParentBranchID,
				OutgoingEdges,
				Edges);
		}
	}

	double MeasureSourceRadius(
		const FVector& Position,
		const TMeshAABBTree3<FDynamicMesh3>& SourceSpatial,
		const double CellSize)
	{
		double DistanceSquared = TNumericLimits<double>::Max();
		const int32 TriangleID = SourceSpatial.FindNearestTriangle(
			FVector3d(Position),
			DistanceSquared);
		return TriangleID == INDEX_NONE || !FMath::IsFinite(DistanceSquared)
			? CellSize
			: FMath::Max(CellSize, FMath::Sqrt(FMath::Max(0.0, DistanceSquared)));
	}

	int32 ApplyTreeSemantics(
		const FVector& Pivot,
		FFoliageBakerTreeSkeletonResult& Result)
	{
		TArray<TArray<int32>> OutgoingEdges;
		OutgoingEdges.SetNum(Result.Nodes.Num());
		for (int32 EdgeIndex = 0; EdgeIndex < Result.Edges.Num(); ++EdgeIndex)
		{
			const FFoliageBakerTreeSkeletonEdge& Edge = Result.Edges[EdgeIndex];
			if (OutgoingEdges.IsValidIndex(Edge.StartNodeID))
			{
				OutgoingEdges[Edge.StartNodeID].Add(EdgeIndex);
			}
		}
		TArray<double> SubtreeScores;
		SubtreeScores.Init(-1.0, Result.Nodes.Num());
		MeasureDominantPathScore(
			Result.RootNodeID,
			OutgoingEdges,
			Result.Edges,
			SubtreeScores);
		int32 TrunkNodeID = Result.RootNodeID;
		while (OutgoingEdges.IsValidIndex(TrunkNodeID)
			&& !OutgoingEdges[TrunkNodeID].IsEmpty())
		{
			int32 BestEdgeIndex = INDEX_NONE;
			double BestScore = -1.0;
			for (const int32 EdgeIndex : OutgoingEdges[TrunkNodeID])
			{
				const FFoliageBakerTreeSkeletonEdge& Edge = Result.Edges[EdgeIndex];
				const double Score = PolylineLength(Edge.Polyline)
					* FMath::Square(FMath::Max(Edge.Radius, 1.0))
					+ (SubtreeScores.IsValidIndex(Edge.EndNodeID)
						? SubtreeScores[Edge.EndNodeID]
						: 0.0);
				if (Score > BestScore)
				{
					BestScore = Score;
					BestEdgeIndex = EdgeIndex;
				}
			}
			if (!Result.Edges.IsValidIndex(BestEdgeIndex))
			{
				break;
			}
			Result.Edges[BestEdgeIndex].bTrunk = true;
			Result.Edges[BestEdgeIndex].BranchID = INDEX_NONE;
			Result.Edges[BestEdgeIndex].ParentBranchID = INDEX_NONE;
			TrunkNodeID = Result.Edges[BestEdgeIndex].EndNodeID;
		}

		const FVector TrunkAxis = Result.Nodes.IsValidIndex(TrunkNodeID)
			? (Result.Nodes[TrunkNodeID].Position - Pivot).GetSafeNormal()
			: FVector::UpVector;
		TArray<int32> TrunkNodeIDs;
		TArray<double> TrunkNodeDistances;
		int32 CurrentTrunkNodeID = Result.RootNodeID;
		double TrunkLength = 0.0;
		double FirstTrunkEdgeRadius = 0.0;
		while (Result.Nodes.IsValidIndex(CurrentTrunkNodeID))
		{
			TrunkNodeIDs.Add(CurrentTrunkNodeID);
			TrunkNodeDistances.Add(TrunkLength);
			int32 NextTrunkEdgeIndex = INDEX_NONE;
			if (OutgoingEdges.IsValidIndex(CurrentTrunkNodeID))
			{
				for (const int32 EdgeIndex : OutgoingEdges[CurrentTrunkNodeID])
				{
					if (Result.Edges.IsValidIndex(EdgeIndex)
						&& Result.Edges[EdgeIndex].bTrunk)
					{
						NextTrunkEdgeIndex = EdgeIndex;
						break;
					}
				}
			}
			if (!Result.Edges.IsValidIndex(NextTrunkEdgeIndex))
			{
				break;
			}
			if (FirstTrunkEdgeRadius <= 0.0)
			{
				FirstTrunkEdgeRadius = Result.Edges[NextTrunkEdgeIndex].Radius;
			}
			TrunkLength += PolylineLength(
				Result.Edges[NextTrunkEdgeIndex].Polyline);
			CurrentTrunkNodeID = Result.Edges[NextTrunkEdgeIndex].EndNodeID;
		}
		const double RootNodeRadius = Result.Nodes.IsValidIndex(Result.RootNodeID)
			? Result.Nodes[Result.RootNodeID].Radius
			: 0.0;
		const double BaseTrunkDiameter = FMath::Max(
			RootNodeRadius,
			FirstTrunkEdgeRadius) * 2.0;
		const double RootBandLength = FMath::Clamp(
			BaseTrunkDiameter * SkeletonRootBandBaseDiameterScale,
			TrunkLength * SkeletonMinimumRootBandLengthFraction,
			TrunkLength * SkeletonMaximumRootBandLengthFraction);
		for (int32 TrunkNodeIndex = 0;
			TrunkNodeIndex < TrunkNodeIDs.Num();
			++TrunkNodeIndex)
		{
			if (!TrunkNodeDistances.IsValidIndex(TrunkNodeIndex)
				|| TrunkNodeDistances[TrunkNodeIndex] > RootBandLength)
			{
				continue;
			}
			const int32 CandidateNodeID = TrunkNodeIDs[TrunkNodeIndex];
			if (!OutgoingEdges.IsValidIndex(CandidateNodeID))
			{
				continue;
			}
			for (const int32 EdgeIndex : OutgoingEdges[CandidateNodeID])
			{
				if (!Result.Edges.IsValidIndex(EdgeIndex)
					|| Result.Edges[EdgeIndex].bTrunk)
				{
					continue;
				}
				const FFoliageBakerTreeSkeletonEdge& CandidateEdge =
					Result.Edges[EdgeIndex];
				const double SubtreeMaximumProjection =
					MeasureSubtreeMaximumProjection(
						CandidateEdge.EndNodeID,
						Pivot,
						TrunkAxis,
						OutgoingEdges,
						Result.Nodes,
						Result.Edges);
				const double PersistenceLength = PolylineLength(
					CandidateEdge.Polyline)
					+ MeasureSubtreePersistenceLength(
						CandidateEdge.EndNodeID,
						OutgoingEdges,
						Result.Edges);
				const double InitialRise = CandidateEdge.Polyline.Num() >= 2
					? FVector::DotProduct(
						CandidateEdge.Polyline.Last()
							- CandidateEdge.Polyline[0],
						TrunkAxis)
					: 0.0;
				const double MaximumInitialRise = FMath::Max(
					PersistenceLength * SkeletonRootMaximumRisePersistenceFraction,
					CandidateEdge.Radius * 2.0);
				if (SubtreeMaximumProjection <= RootBandLength
					&& InitialRise <= MaximumInitialRise)
				{
					MarkRootSubtreeAsTrunk(
						EdgeIndex,
						OutgoingEdges,
						Result.Edges);
				}
			}
		}

		int32 NextBranchID = 0;
		for (const FFoliageBakerTreeSkeletonEdge& TrunkEdge : Result.Edges)
		{
			if (!TrunkEdge.bTrunk
				|| !OutgoingEdges.IsValidIndex(TrunkEdge.EndNodeID))
			{
				continue;
			}
			for (const int32 EdgeIndex : OutgoingEdges[TrunkEdge.EndNodeID])
			{
				if (Result.Edges[EdgeIndex].bTrunk
					|| Result.Edges[EdgeIndex].BranchID != INDEX_NONE)
				{
					continue;
				}
				AssignBranchSubtree(
					EdgeIndex,
					NextBranchID++,
					INDEX_NONE,
					OutgoingEdges,
					Result.Edges);
			}
		}
		if (OutgoingEdges.IsValidIndex(Result.RootNodeID))
		{
			for (const int32 EdgeIndex : OutgoingEdges[Result.RootNodeID])
			{
				if (Result.Edges[EdgeIndex].bTrunk
					|| Result.Edges[EdgeIndex].BranchID != INDEX_NONE)
				{
					continue;
				}
				AssignBranchSubtree(
					EdgeIndex,
					NextBranchID++,
					INDEX_NONE,
					OutgoingEdges,
					Result.Edges);
			}
		}

		return NextBranchID;
	}

	bool BuildSparseGpuSkeletonGraph(
		const FFoliageBakerGpuTreeSkeletonResult& GpuResult,
		const FVector& Pivot,
		const TMeshAABBTree3<FDynamicMesh3>& SourceSpatial,
		FFoliageBakerTreeSkeletonResult& Result)
	{
		if (!GpuResult.bSucceeded || GpuResult.SkeletonVoxelIndices.IsEmpty())
		{
			return false;
		}
		TArray<int32> VoxelIndices = GpuResult.SkeletonVoxelIndices;
		const TArray<float>& VoxelRadii = GpuResult.SkeletonVoxelRadii;
		TMap<int32, double> RadiusByVoxelIndex;
		RadiusByVoxelIndex.Reserve(VoxelIndices.Num());
		for (int32 SparseIndex = 0;
			SparseIndex < VoxelIndices.Num();
			++SparseIndex)
		{
			if (VoxelRadii.IsValidIndex(SparseIndex))
			{
				RadiusByVoxelIndex.Add(
					VoxelIndices[SparseIndex],
					static_cast<double>(VoxelRadii[SparseIndex]));
			}
		}
		VoxelIndices.Sort();
		const FVector3i Dimensions(
			GpuResult.Dimensions.X,
			GpuResult.Dimensions.Y,
			GpuResult.Dimensions.Z);
		const FVector3f GridOrigin(GpuResult.GridOrigin);
		TMap<int32, int32> SparseIndexByVoxel;
		SparseIndexByVoxel.Reserve(VoxelIndices.Num());
		for (int32 SparseIndex = 0;
			SparseIndex < VoxelIndices.Num();
			++SparseIndex)
		{
			SparseIndexByVoxel.Add(
				VoxelIndices[SparseIndex],
				SparseIndex);
		}

		int32 RootSparseIndex = INDEX_NONE;
		double RootDistanceSquared = TNumericLimits<double>::Max();
		for (int32 SparseIndex = 0;
			SparseIndex < VoxelIndices.Num();
			++SparseIndex)
		{
			const double DistanceSquared = FVector::DistSquared(
				GridPosition(
					VoxelIndices[SparseIndex],
					Dimensions,
					GridOrigin,
					GpuResult.CellSize),
				Pivot);
			if (DistanceSquared < RootDistanceSquared)
			{
				RootDistanceSquared = DistanceSquared;
				RootSparseIndex = SparseIndex;
			}
		}
		if (RootSparseIndex == INDEX_NONE)
		{
			return false;
		}

		TArray<double> RadiusBySparseIndex;
		RadiusBySparseIndex.Init(-1.0, VoxelIndices.Num());
		for (int32 SparseIndex = 0;
			SparseIndex < VoxelIndices.Num();
			++SparseIndex)
		{
			const int32 VoxelIndex = VoxelIndices[SparseIndex];
			RadiusBySparseIndex[SparseIndex] = RadiusByVoxelIndex.Contains(VoxelIndex)
				? FMath::Max(
					GpuResult.CellSize,
					RadiusByVoxelIndex.FindRef(VoxelIndex))
				: MeasureSourceRadius(
					GridPosition(
						VoxelIndex,
						Dimensions,
						GridOrigin,
						GpuResult.CellSize),
					SourceSpatial,
					GpuResult.CellSize);
		}
		TArray<double> Distances;
		Distances.Init(TNumericLimits<double>::Max(), VoxelIndices.Num());
		TArray<int32> Predecessors;
		Predecessors.Init(INDEX_NONE, VoxelIndices.Num());
		TArray<FQueueEntry> Queue;
		const FQueueEntryLess QueueEntryLess;
		Distances[RootSparseIndex] = 0.0;
		Queue.HeapPush(FQueueEntry{RootSparseIndex, 0.0}, QueueEntryLess);
		while (!Queue.IsEmpty())
		{
			FQueueEntry Entry;
			Queue.HeapPop(Entry, QueueEntryLess, EAllowShrinking::No);
			if (Entry.Distance > Distances[Entry.Index])
			{
				continue;
			}
			const int32 VoxelIndex = VoxelIndices[Entry.Index];
			const FVector3i Coordinate = GridCoordinate(VoxelIndex, Dimensions);
			for (int32 OffsetZ = -1; OffsetZ <= 1; ++OffsetZ)
			{
				for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
				{
					for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
					{
						if (OffsetX == 0 && OffsetY == 0 && OffsetZ == 0)
						{
							continue;
						}
						const FVector3i NeighborCoordinate = Coordinate
							+ FVector3i(OffsetX, OffsetY, OffsetZ);
						if (!IsInsideGrid(NeighborCoordinate, Dimensions))
						{
							continue;
						}
						const int32 NeighborVoxelIndex = GridIndex(
							NeighborCoordinate,
							Dimensions);
						if (!SparseIndexByVoxel.Contains(NeighborVoxelIndex))
						{
							continue;
						}
						const int32 NeighborSparseIndex = SparseIndexByVoxel.FindRef(
							NeighborVoxelIndex);
						const double StepLength = GpuResult.CellSize * FMath::Sqrt(
							static_cast<double>(OffsetX * OffsetX + OffsetY * OffsetY + OffsetZ * OffsetZ));
						const double ClearanceInCells = FMath::Max(
							1.0,
							RadiusBySparseIndex[NeighborSparseIndex] / GpuResult.CellSize);
						const double CenterlineCostScale = 1.0
							+ 16.0 / FMath::Square(ClearanceInCells);
						const double CandidateDistance = Entry.Distance
							+ StepLength * CenterlineCostScale;
						const bool bShorter = CandidateDistance
							< Distances[NeighborSparseIndex] - UE_DOUBLE_SMALL_NUMBER;
						const bool bStableTie = FMath::IsNearlyEqual(
							CandidateDistance,
							Distances[NeighborSparseIndex],
							UE_DOUBLE_SMALL_NUMBER)
							&& (Predecessors[NeighborSparseIndex] == INDEX_NONE
								|| Entry.Index < Predecessors[NeighborSparseIndex]);
						if (bShorter || bStableTie)
						{
							Distances[NeighborSparseIndex] = CandidateDistance;
							Predecessors[NeighborSparseIndex] = Entry.Index;
							Queue.HeapPush(
								FQueueEntry{NeighborSparseIndex, CandidateDistance},
								QueueEntryLess);
						}
					}
				}
			}
		}

		int32 ReachableVoxelCount = 0;
		for (int32 SparseIndex = 0; SparseIndex < Distances.Num(); ++SparseIndex)
		{
			if (Distances[SparseIndex] == TNumericLimits<double>::Max())
			{
				continue;
			}
			++ReachableVoxelCount;
		}

		TBitArray<> Active(false, VoxelIndices.Num());
		TBitArray<> Covered(false, VoxelIndices.Num());
		Active[RootSparseIndex] = true;
		TArray<int32> CandidateOrder;
		CandidateOrder.Reserve(ReachableVoxelCount);
		for (int32 SparseIndex = 0; SparseIndex < Distances.Num(); ++SparseIndex)
		{
			if (Distances[SparseIndex] != TNumericLimits<double>::Max())
			{
				CandidateOrder.Add(SparseIndex);
			}
		}
		CandidateOrder.Sort(
			[&Distances, &VoxelIndices](const int32 FirstIndex, const int32 SecondIndex)
			{
				return Distances[FirstIndex] == Distances[SecondIndex]
					? VoxelIndices[FirstIndex] > VoxelIndices[SecondIndex]
					: Distances[FirstIndex] > Distances[SecondIndex];
			});

		const auto MarkPathCoverage = [
			&Covered,
			&Dimensions,
			&GpuResult,
			&RadiusBySparseIndex,
			&SparseIndexByVoxel,
			&VoxelIndices](const TArray<int32>& PathSparseIndices)
			{
				for (const int32 PathSparseIndex : PathSparseIndices)
				{
					const FVector3i PathCoordinate = GridCoordinate(
						VoxelIndices[PathSparseIndex],
						Dimensions);
					const double CoverageRadius = RadiusBySparseIndex[PathSparseIndex]
						* CoverageRadiusScale
						+ GpuResult.CellSize * 0.75;
					const int32 CellRadius = FMath::CeilToInt(
						CoverageRadius / GpuResult.CellSize);
					const double CoverageRadiusSquared = FMath::Square(CoverageRadius);
					for (int32 OffsetZ = -CellRadius; OffsetZ <= CellRadius; ++OffsetZ)
					{
						for (int32 OffsetY = -CellRadius; OffsetY <= CellRadius; ++OffsetY)
						{
							for (int32 OffsetX = -CellRadius; OffsetX <= CellRadius; ++OffsetX)
							{
								const double OffsetDistanceSquared = FMath::Square(GpuResult.CellSize)
									* static_cast<double>(
										OffsetX * OffsetX
										+ OffsetY * OffsetY
										+ OffsetZ * OffsetZ);
								if (OffsetDistanceSquared > CoverageRadiusSquared)
								{
									continue;
								}
								const FVector3i CoveredCoordinate = PathCoordinate
									+ FVector3i(OffsetX, OffsetY, OffsetZ);
								if (!IsInsideGrid(CoveredCoordinate, Dimensions))
								{
									continue;
								}
								const int32 CoveredVoxelIndex = GridIndex(
									CoveredCoordinate,
									Dimensions);
								if (SparseIndexByVoxel.Contains(CoveredVoxelIndex))
								{
									Covered[SparseIndexByVoxel.FindRef(CoveredVoxelIndex)] = true;
								}
							}
						}
					}
				}
			};

		TArray<int32> RootPath;
		RootPath.Add(RootSparseIndex);
		MarkPathCoverage(RootPath);
		for (;;)
		{
			int32 TipSparseIndex = INDEX_NONE;
			for (const int32 CandidateSparseIndex : CandidateOrder)
			{
				if (!Covered[CandidateSparseIndex])
				{
					TipSparseIndex = CandidateSparseIndex;
					break;
				}
			}
			if (TipSparseIndex == INDEX_NONE)
			{
				break;
			}

			TArray<int32> ExtractedPath;
			TArray<double> ExtractedRadii;
			double ExtractedLength = 0.0;
			int32 CurrentSparseIndex = TipSparseIndex;
			while (CurrentSparseIndex != INDEX_NONE
				&& !Active[CurrentSparseIndex])
			{
				ExtractedPath.Add(CurrentSparseIndex);
				ExtractedRadii.Add(RadiusBySparseIndex[CurrentSparseIndex]);
				const int32 ParentSparseIndex = Predecessors[CurrentSparseIndex];
				if (!VoxelIndices.IsValidIndex(ParentSparseIndex))
				{
					break;
				}
				ExtractedLength += FVector::Distance(
					GridPosition(
						VoxelIndices[CurrentSparseIndex],
						Dimensions,
						GridOrigin,
						GpuResult.CellSize),
					GridPosition(
						VoxelIndices[ParentSparseIndex],
						Dimensions,
						GridOrigin,
						GpuResult.CellSize));
				CurrentSparseIndex = ParentSparseIndex;
			}
			if (Active.IsValidIndex(CurrentSparseIndex))
			{
				ExtractedPath.Add(CurrentSparseIndex);
			}
			MarkPathCoverage(ExtractedPath);
			if (ExtractedRadii.IsEmpty())
			{
				continue;
			}
			ExtractedRadii.Sort();
			const double MedianRadius = ExtractedRadii[
				ExtractedRadii.Num() / 2];
			const double MinimumPathLength = FMath::Max(
				GpuResult.CellSize * MinimumExtractedPathCellCount,
				MedianRadius * MinimumExtractedPathRadiusScale);
			if (ExtractedLength < MinimumPathLength)
			{
				continue;
			}
			for (const int32 PathSparseIndex : ExtractedPath)
			{
				Active[PathSparseIndex] = true;
			}
		}

		for (;;)
		{
			TArray<int32> PruneChildCounts;
			PruneChildCounts.Init(0, VoxelIndices.Num());
			for (int32 SparseIndex = 0;
				SparseIndex < Predecessors.Num();
				++SparseIndex)
			{
				if (Active[SparseIndex]
					&& Active.IsValidIndex(Predecessors[SparseIndex])
					&& Active[Predecessors[SparseIndex]])
				{
					++PruneChildCounts[Predecessors[SparseIndex]];
				}
			}

			bool bRemovedPath = false;
			for (int32 TipSparseIndex = 0;
				TipSparseIndex < VoxelIndices.Num();
				++TipSparseIndex)
			{
				if (!Active[TipSparseIndex]
					|| TipSparseIndex == RootSparseIndex
					|| PruneChildCounts[TipSparseIndex] != 0)
				{
					continue;
				}

				TArray<int32> TerminalPath;
				TArray<double> TerminalRadii;
				double TerminalLength = 0.0;
				int32 CurrentSparseIndex = TipSparseIndex;
				while (CurrentSparseIndex != INDEX_NONE
					&& CurrentSparseIndex != RootSparseIndex)
				{
					TerminalPath.Add(CurrentSparseIndex);
					TerminalRadii.Add(
						RadiusBySparseIndex[CurrentSparseIndex]);
					const int32 ParentSparseIndex =
						Predecessors[CurrentSparseIndex];
					if (!VoxelIndices.IsValidIndex(ParentSparseIndex))
					{
						break;
					}
					TerminalLength += FVector::Distance(
						GridPosition(
							VoxelIndices[CurrentSparseIndex],
							Dimensions,
							GridOrigin,
							GpuResult.CellSize),
						GridPosition(
							VoxelIndices[ParentSparseIndex],
							Dimensions,
							GridOrigin,
							GpuResult.CellSize));
					if (ParentSparseIndex == RootSparseIndex
						|| PruneChildCounts[ParentSparseIndex] > 1)
					{
						break;
					}
					CurrentSparseIndex = ParentSparseIndex;
				}
				if (TerminalRadii.IsEmpty())
				{
					continue;
				}

				TerminalRadii.Sort();
				const double MedianRadius = TerminalRadii[
					TerminalRadii.Num() / 2];
				const double MinimumLength = FMath::Max(
					GpuResult.CellSize * MinimumExtractedPathCellCount,
					MedianRadius * MinimumExtractedPathRadiusScale);
				if (TerminalLength >= MinimumLength)
				{
					continue;
				}
				for (const int32 PathSparseIndex : TerminalPath)
				{
					Active[PathSparseIndex] = false;
				}
				bRemovedPath = true;
			}
			if (!bRemovedPath)
			{
				break;
			}
		}

		TArray<TArray<int32>> Children;
		Children.SetNum(VoxelIndices.Num());
		for (int32 SparseIndex = 0; SparseIndex < Predecessors.Num(); ++SparseIndex)
		{
			if (Active[SparseIndex]
				&& Active.IsValidIndex(Predecessors[SparseIndex])
				&& Active[Predecessors[SparseIndex]])
			{
				Children[Predecessors[SparseIndex]].Add(SparseIndex);
			}
		}
		TArray<int32> NodeIDBySparseIndex;
		NodeIDBySparseIndex.Init(INDEX_NONE, VoxelIndices.Num());
		for (int32 SparseIndex = 0; SparseIndex < Distances.Num(); ++SparseIndex)
		{
			if (!Active[SparseIndex]
				|| (SparseIndex != RootSparseIndex && Children[SparseIndex].Num() == 1))
			{
				continue;
			}
			FFoliageBakerTreeSkeletonNode& Node = Result.Nodes.AddDefaulted_GetRef();
			Node.NodeID = Result.Nodes.Num() - 1;
			Node.Position = SparseIndex == RootSparseIndex
				? Pivot
				: GridPosition(
					VoxelIndices[SparseIndex],
					Dimensions,
					GridOrigin,
					GpuResult.CellSize);
			Node.Radius = RadiusBySparseIndex[SparseIndex];
			Node.Kind = SparseIndex == RootSparseIndex
				? EFoliageBakerTreeSkeletonNodeKind::Root
				: Children[SparseIndex].Num() > 1
					? EFoliageBakerTreeSkeletonNodeKind::Fork
					: EFoliageBakerTreeSkeletonNodeKind::Tip;
			NodeIDBySparseIndex[SparseIndex] = Node.NodeID;
		}
		Result.RootNodeID = NodeIDBySparseIndex[RootSparseIndex];

		for (int32 StartSparseIndex = 0;
			StartSparseIndex < NodeIDBySparseIndex.Num();
			++StartSparseIndex)
		{
			const int32 StartNodeID = NodeIDBySparseIndex[StartSparseIndex];
			if (StartNodeID == INDEX_NONE)
			{
				continue;
			}
			for (const int32 FirstChildSparseIndex : Children[StartSparseIndex])
			{
				TArray<int32> EdgeSparseIndices;
				EdgeSparseIndices.Add(StartSparseIndex);
				int32 CurrentSparseIndex = FirstChildSparseIndex;
				while (CurrentSparseIndex != INDEX_NONE)
				{
					EdgeSparseIndices.Add(CurrentSparseIndex);
					if (NodeIDBySparseIndex[CurrentSparseIndex] != INDEX_NONE)
					{
						break;
					}
					CurrentSparseIndex = Children[CurrentSparseIndex].Num() == 1
						? Children[CurrentSparseIndex][0]
						: INDEX_NONE;
				}
				if (CurrentSparseIndex == INDEX_NONE
					|| NodeIDBySparseIndex[CurrentSparseIndex] == INDEX_NONE)
				{
					continue;
				}
				FFoliageBakerTreeSkeletonEdge& Edge = Result.Edges.AddDefaulted_GetRef();
				Edge.EdgeID = Result.Edges.Num() - 1;
				Edge.StartNodeID = StartNodeID;
				Edge.EndNodeID = NodeIDBySparseIndex[CurrentSparseIndex];
				TArray<double> Radii;
				for (const int32 EdgeSparseIndex : EdgeSparseIndices)
				{
					const FVector Position = EdgeSparseIndex == RootSparseIndex
						? Pivot
						: GridPosition(
							VoxelIndices[EdgeSparseIndex],
							Dimensions,
							GridOrigin,
							GpuResult.CellSize);
					Edge.Polyline.Add(Position);
					Radii.Add(RadiusBySparseIndex[EdgeSparseIndex]);
				}
				SmoothAndSimplifyPolyline(Edge.Polyline, GpuResult.CellSize);
				Radii.Sort();
				Edge.Radius = Radii.IsEmpty()
					? GpuResult.CellSize
					: Radii[Radii.Num() / 2];
				Result.Nodes[Edge.EndNodeID].ParentNodeID = StartNodeID;
			}
		}

		Result.OccupiedVoxelCount = GpuResult.OccupiedVoxelCount;
		Result.OccupiedComponentCount = 1;
		Result.ConnectedComponentCount = 1;
		Result.ConnectedInteriorVoxelCount = ReachableVoxelCount;
		Result.CellSize = GpuResult.CellSize;
		return !Result.Nodes.IsEmpty() && !Result.Edges.IsEmpty();
	}
}

FFoliageBakerTreeSkeletonResult FFoliageBakerTreeSkeleton::Build(
	const TArray<FFoliageBakerTreeSkeletonTriangle>& Triangles,
	const FVector& Pivot)
{
	FFoliageBakerTreeSkeletonResult Result;
	if (Triangles.IsEmpty())
	{
		Result.Report = TEXT("No wood triangles were supplied to the skeleton builder.");
		return Result;
	}

	FDynamicMesh3 SourceMesh;
	struct FCoverageDiagnostic
	{
		int32 TriangleCount = 0;
		double MaximumRatio = 0.0;
		FVector WorstPosition = FVector::ZeroVector;
	};
	TMap<int32, FCoverageDiagnostic> CoverageByComponent;
	for (const FFoliageBakerTreeSkeletonTriangle& Triangle : Triangles)
	{
		if (Triangle.A.ContainsNaN()
			|| Triangle.B.ContainsNaN()
			|| Triangle.C.ContainsNaN())
		{
			continue;
		}
		const int32 A = SourceMesh.AppendVertex(FVector3d(Triangle.A));
		const int32 B = SourceMesh.AppendVertex(FVector3d(Triangle.B));
		const int32 C = SourceMesh.AppendVertex(FVector3d(Triangle.C));
		SourceMesh.AppendTriangle(A, B, C);
	}
	if (SourceMesh.TriangleCount() == 0)
	{
		Result.Report = TEXT("Wood triangles contain no finite geometry.");
		return Result;
	}

	const FAxisAlignedBox3d SourceBounds = SourceMesh.GetBounds();
	const double CellSize = SourceBounds.MaxDim() / CpuFallbackVoxelResolution;
	if (!FMath::IsFinite(CellSize) || CellSize <= UE_DOUBLE_SMALL_NUMBER)
	{
		Result.Report = TEXT("Wood bounds cannot define a finite voxel size.");
		return Result;
	}
	Result.CellSize = CellSize;

	TMeshAABBTree3<FDynamicMesh3> SourceSpatial(&SourceMesh);
	TFastWindingTree<FDynamicMesh3> SourceWinding(&SourceSpatial);
	TImplicitSolidify<FDynamicMesh3> Solidify(
		&SourceMesh,
		&SourceSpatial,
		&SourceWinding);
	Solidify.ExtendBounds = CellSize * 2.0;
	Solidify.MeshCellSize = CellSize;
	Solidify.WindingThreshold = 0.5;
	Solidify.SurfaceSearchSteps = 3;
	Solidify.bSolidAtBoundaries = true;
	const FDynamicMesh3 SolidMesh(&Solidify.Generate());
	if (SolidMesh.TriangleCount() == 0)
	{
		Result.Report = TEXT("Voxel solidification produced no wood volume.");
		return Result;
	}

	TArray<FFoliageBakerTreeSkeletonTriangle> SolidTriangles;
	SolidTriangles.Reserve(SolidMesh.TriangleCount());
	for (const int32 TriangleID : SolidMesh.TriangleIndicesItr())
	{
		const UE::Geometry::FIndex3i Triangle = SolidMesh.GetTriangle(TriangleID);
		FFoliageBakerTreeSkeletonTriangle& SolidTriangle =
			SolidTriangles.AddDefaulted_GetRef();
		SolidTriangle.A = FVector(SolidMesh.GetVertex(Triangle.A));
		SolidTriangle.B = FVector(SolidMesh.GetVertex(Triangle.B));
		SolidTriangle.C = FVector(SolidMesh.GetVertex(Triangle.C));
	}

	const FFoliageBakerGpuTreeSkeletonResult GpuSkeleton =
		FFoliageBakerTreeSkeletonGpu::Build(
			SolidTriangles,
			FBox(FVector(SourceBounds.Min), FVector(SourceBounds.Max)),
			Pivot,
			TargetVoxelResolution);
	if (GpuSkeleton.bSucceeded
		&& BuildSparseGpuSkeletonGraph(
			GpuSkeleton,
			Pivot,
			SourceSpatial,
			Result))
	{
		for (const FFoliageBakerTreeSkeletonTriangle& Triangle : Triangles)
		{
			if (Triangle.A.ContainsNaN()
				|| Triangle.B.ContainsNaN()
				|| Triangle.C.ContainsNaN())
			{
				continue;
			}
			const TStaticArray<FVector, 4> Samples{
				Triangle.A,
				Triangle.B,
				Triangle.C,
				(Triangle.A + Triangle.B + Triangle.C) / 3.0};
			double TriangleMaximumRatio = 0.0;
			for (const FVector& Sample : Samples)
			{
				const FSkeletonEdgeLocation Location =
					FindClosestSkeletonEdgeLocation(Sample, Result.Edges);
				if (!Result.Edges.IsValidIndex(Location.EdgeIndex))
				{
					continue;
				}
				const double CoverageRadius = FMath::Max(
					Result.Edges[Location.EdgeIndex].Radius + Result.CellSize * 0.75,
					Result.CellSize);
				TriangleMaximumRatio = FMath::Max(
					TriangleMaximumRatio,
					FMath::Sqrt(Location.DistanceSquared) / CoverageRadius);
			}
			Result.MaximumWoodCoverageRatio = FMath::Max(
				Result.MaximumWoodCoverageRatio,
				TriangleMaximumRatio);
			if (TriangleMaximumRatio > 1.75)
			{
				++Result.UncoveredWoodTriangleCount;
			}
		}

		const int32 BranchCount = ApplyTreeSemantics(Pivot, Result);
		Result.bSucceeded = !Result.Nodes.IsEmpty() && !Result.Edges.IsEmpty();
		Result.Report = FString::Printf(
			TEXT("%s %d node(s), %d edge(s), %d branch group(s), %d uncovered wood triangle(s), %.2f maximum wood coverage ratio."),
			*GpuSkeleton.Report,
			Result.Nodes.Num(),
			Result.Edges.Num(),
			BranchCount,
			Result.UncoveredWoodTriangleCount,
			Result.MaximumWoodCoverageRatio);
		return Result;
	}

	TMeshAABBTree3<FDynamicMesh3> SolidSpatial(&SolidMesh);
	TSweepingMeshSDF<FDynamicMesh3> SDF;
	SDF.Mesh = &SolidMesh;
	SDF.Spatial = &SolidSpatial;
	SDF.ComputeMode = TSweepingMeshSDF<FDynamicMesh3>::EComputeModes::FullGrid;
	SDF.CellSize = static_cast<float>(CellSize);
	SDF.ExactBandWidth = 2;
	SDF.ExpandBounds = FVector3d(CellSize * 2.0);
	if (!SDF.Compute(SourceBounds))
	{
		Result.Report = TEXT("Signed-distance voxelization failed.");
		return Result;
	}

	const FVector3i Dimensions = SDF.Dimensions();
	const int32 GridCellCount = SDF.Grid.Size();
	TBitArray<> Occupied(false, GridCellCount);
	TBitArray<> Interior(false, GridCellCount);
	int32 RootIndex = INDEX_NONE;
	double RootDistanceSquared = TNumericLimits<double>::Max();
	for (int32 Index = 0; Index < GridCellCount; ++Index)
	{
		if (SDF.Grid[Index]
			> static_cast<float>(CellSize * OccupancyExpansionCellScale))
		{
			continue;
		}
		Occupied[Index] = true;
		++Result.OccupiedVoxelCount;
		if (SDF.Grid[Index] > 0.0f)
		{
			continue;
		}
		Interior[Index] = true;
		const double DistanceSquared = FVector::DistSquared(
			GridPosition(Index, Dimensions, SDF.GridOrigin, CellSize),
			Pivot);
		if (DistanceSquared < RootDistanceSquared)
		{
			RootDistanceSquared = DistanceSquared;
			RootIndex = Index;
		}
	}
	if (RootIndex == INDEX_NONE)
	{
		Result.Report = TEXT("Signed-distance voxelization contains no interior wood cells.");
		return Result;
	}

	TArray<int32> ComponentIDs;
	ComponentIDs.Init(INDEX_NONE, GridCellCount);
	TArray<FVolumeComponent> Components;
	TArray<int32> FloodQueue;
	for (int32 SeedIndex = 0; SeedIndex < GridCellCount; ++SeedIndex)
	{
		if (!Occupied[SeedIndex] || ComponentIDs[SeedIndex] != INDEX_NONE)
		{
			continue;
		}
		const int32 ComponentID = Components.AddDefaulted();
		FloodQueue.Reset();
		FloodQueue.Add(SeedIndex);
		ComponentIDs[SeedIndex] = ComponentID;
		for (int32 QueueIndex = 0; QueueIndex < FloodQueue.Num(); ++QueueIndex)
		{
			const int32 CurrentIndex = FloodQueue[QueueIndex];
			++Components[ComponentID].OccupiedCellCount;
			const FVector3i Current = GridCoordinate(CurrentIndex, Dimensions);
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				for (int32 Direction = -1; Direction <= 1; Direction += 2)
				{
					FVector3i Neighbor = Current;
					Neighbor[Axis] += Direction;
					if (!IsInsideGrid(Neighbor, Dimensions))
					{
						continue;
					}
					const int32 NeighborIndex = GridIndex(Neighbor, Dimensions);
					if (!Occupied[NeighborIndex]
						|| ComponentIDs[NeighborIndex] != INDEX_NONE)
					{
						continue;
					}
					ComponentIDs[NeighborIndex] = ComponentID;
					FloodQueue.Add(NeighborIndex);
				}
			}
		}
	}
	Result.OccupiedComponentCount = Components.Num();
	const int32 RootComponentID = ComponentIDs[RootIndex];
	for (int32 Index = 0; Index < GridCellCount; ++Index)
	{
		if (!Interior[Index])
		{
			continue;
		}
		const int32 ComponentID = ComponentIDs[Index];
		if (!Components.IsValidIndex(ComponentID))
		{
			continue;
		}
		FVolumeComponent& Component = Components[ComponentID];
		++Component.InteriorCellCount;
		Component.InteriorCellIndices.Add(Index);
		Component.InteriorPositionSum += GridPosition(
			Index,
			Dimensions,
			SDF.GridOrigin,
			CellSize);
	}
	for (FVolumeComponent& Component : Components)
	{
		if (Component.InteriorCellIndices.IsEmpty())
		{
			continue;
		}
		const FVector Centroid = Component.InteriorPositionSum
			/ static_cast<double>(Component.InteriorCellCount);
		double MaximumDistanceSquared = -1.0;
		for (const int32 CellIndex : Component.InteriorCellIndices)
		{
			const double DistanceSquared = FVector::DistSquared(
				GridPosition(CellIndex, Dimensions, SDF.GridOrigin, CellSize),
				Centroid);
			if (DistanceSquared > MaximumDistanceSquared)
			{
				MaximumDistanceSquared = DistanceSquared;
				Component.EndpointA = CellIndex;
			}
		}
		MaximumDistanceSquared = -1.0;
		const FVector EndpointAPosition = GridPosition(
			Component.EndpointA,
			Dimensions,
			SDF.GridOrigin,
			CellSize);
		for (const int32 CellIndex : Component.InteriorCellIndices)
		{
			const double DistanceSquared = FVector::DistSquared(
				GridPosition(CellIndex, Dimensions, SDF.GridOrigin, CellSize),
				EndpointAPosition);
			if (DistanceSquared > MaximumDistanceSquared)
			{
				MaximumDistanceSquared = DistanceSquared;
				Component.EndpointB = CellIndex;
			}
		}
		Component.EndpointSpan = MaximumDistanceSquared > 0.0
			? FMath::Sqrt(MaximumDistanceSquared)
			: 0.0;
	}

	TMap<uint64, FComponentContact> ContactByComponentPair;
	const int32 MaximumBridgeOffset = FMath::CeilToInt(
		MaximumComponentBridgeCellDistance);
	const double MaximumBridgeDistanceSquared = FMath::Square(
		CellSize * MaximumComponentBridgeCellDistance);
	for (int32 Index = 0; Index < GridCellCount; ++Index)
	{
		if (!Interior[Index])
		{
			continue;
		}
		const int32 ComponentID = ComponentIDs[Index];
		const FVector3i Coordinate = GridCoordinate(Index, Dimensions);
		for (int32 OffsetZ = -MaximumBridgeOffset;
			OffsetZ <= MaximumBridgeOffset;
			++OffsetZ)
		{
			for (int32 OffsetY = -MaximumBridgeOffset;
				OffsetY <= MaximumBridgeOffset;
				++OffsetY)
			{
				for (int32 OffsetX = -MaximumBridgeOffset;
					OffsetX <= MaximumBridgeOffset;
					++OffsetX)
				{
					const FVector3i NeighborCoordinate = Coordinate
						+ FVector3i(OffsetX, OffsetY, OffsetZ);
					if (!IsInsideGrid(NeighborCoordinate, Dimensions))
					{
						continue;
					}
					const int32 NeighborIndex = GridIndex(
						NeighborCoordinate,
						Dimensions);
					if (NeighborIndex <= Index || !Interior[NeighborIndex])
					{
						continue;
					}
					const int32 NeighborComponentID = ComponentIDs[NeighborIndex];
					if (NeighborComponentID == ComponentID)
					{
						continue;
					}
					const double GapSquared = FVector::DistSquared(
						GridPosition(Index, Dimensions, SDF.GridOrigin, CellSize),
						GridPosition(NeighborIndex, Dimensions, SDF.GridOrigin, CellSize));
					if (GapSquared > MaximumBridgeDistanceSquared)
					{
						continue;
					}
					const int32 ComponentA = FMath::Min(ComponentID, NeighborComponentID);
					const int32 ComponentB = FMath::Max(ComponentID, NeighborComponentID);
					FComponentContact& Contact = ContactByComponentPair.FindOrAdd(
						ComponentPairKey(ComponentA, ComponentB));
					Contact.ComponentA = ComponentA;
					Contact.ComponentB = ComponentB;
					++Contact.SupportSampleCount;
					if (GapSquared < Contact.GapSquared)
					{
						Contact.GapSquared = GapSquared;
						Contact.CellA = ComponentID == ComponentA ? Index : NeighborIndex;
						Contact.CellB = ComponentID == ComponentA ? NeighborIndex : Index;
					}
				}
			}
		}
	}
	TArray<FComponentContact> ComponentContacts;
	ContactByComponentPair.GenerateValueArray(ComponentContacts);
	ComponentContacts.Sort(
		[](const FComponentContact& First, const FComponentContact& Second)
		{
			if (First.ComponentA != Second.ComponentA)
			{
				return First.ComponentA < Second.ComponentA;
			}
			return First.ComponentB < Second.ComponentB;
		});

	TBitArray<> ConnectedComponents(false, Components.Num());
	if (RootComponentID >= 0 && RootComponentID < ConnectedComponents.Num())
	{
		ConnectedComponents[RootComponentID] = true;
	}
	TArray<FSelectedComponentBridge> SelectedBridges;
	for (;;)
	{
		FSelectedComponentBridge GlobalChoice;
		for (int32 ChildComponentID = 0;
			ChildComponentID < Components.Num();
			++ChildComponentID)
		{
			if (ConnectedComponents[ChildComponentID])
			{
				continue;
			}
			const FVolumeComponent& ChildComponent = Components[ChildComponentID];
			if (ChildComponent.InteriorCellCount < MinimumComponentInteriorCellCount
				|| ChildComponent.EndpointSpan < CellSize * 2.0)
			{
				continue;
			}

			FSelectedComponentBridge BestChoice;
			double SecondBestScore = TNumericLimits<double>::Max();
			int32 SecondBestParentComponentID = INDEX_NONE;
			for (const FComponentContact& Contact : ComponentContacts)
			{
				int32 ParentComponentID = INDEX_NONE;
				int32 ParentCellIndex = INDEX_NONE;
				int32 ChildCellIndex = INDEX_NONE;
				if (Contact.ComponentA == ChildComponentID
					&& Contact.ComponentB >= 0
					&& Contact.ComponentB < ConnectedComponents.Num()
					&& ConnectedComponents[Contact.ComponentB])
				{
					ParentComponentID = Contact.ComponentB;
					ParentCellIndex = Contact.CellB;
					ChildCellIndex = Contact.CellA;
				}
				else if (Contact.ComponentB == ChildComponentID
					&& Contact.ComponentA >= 0
					&& Contact.ComponentA < ConnectedComponents.Num()
					&& ConnectedComponents[Contact.ComponentA])
				{
					ParentComponentID = Contact.ComponentA;
					ParentCellIndex = Contact.CellA;
					ChildCellIndex = Contact.CellB;
				}
				if (ParentComponentID == INDEX_NONE
					|| Contact.SupportSampleCount < MinimumBridgeSupportSampleCount)
				{
					continue;
				}
				const double EndpointRatio = ComponentEndpointRatio(
					ChildCellIndex,
					ChildComponent,
					Dimensions,
					SDF.GridOrigin,
					CellSize);
				if (EndpointRatio > MaximumChildEndpointRatio)
				{
					continue;
				}
				const double Gap = FMath::Sqrt(Contact.GapSquared);
				const double SupportBonus = FMath::Min(
					0.5,
					FMath::Log2(static_cast<double>(Contact.SupportSampleCount) + 1.0)
						* 0.1);
				const double Score = Gap / CellSize
					+ EndpointRatio * 3.0
					- SupportBonus;
				if (Score < BestChoice.Score)
				{
					SecondBestScore = BestChoice.Score;
					SecondBestParentComponentID = BestChoice.ParentComponentID;
					BestChoice.ParentComponentID = ParentComponentID;
					BestChoice.ChildComponentID = ChildComponentID;
					BestChoice.ParentCellIndex = ParentCellIndex;
					BestChoice.ChildCellIndex = ChildCellIndex;
					BestChoice.Score = Score;
				}
				else if (Score < SecondBestScore)
				{
					SecondBestScore = Score;
					SecondBestParentComponentID = ParentComponentID;
				}
			}
			if (BestChoice.ParentComponentID == INDEX_NONE)
			{
				continue;
			}
			const bool bAmbiguousDifferentParent =
				SecondBestParentComponentID != INDEX_NONE
				&& SecondBestParentComponentID != BestChoice.ParentComponentID
				&& SecondBestScore - BestChoice.Score
					< ComponentBridgeAmbiguityMargin;
			if (!bAmbiguousDifferentParent && BestChoice.Score < GlobalChoice.Score)
			{
				GlobalChoice = BestChoice;
			}
		}
		if (GlobalChoice.ChildComponentID == INDEX_NONE)
		{
			break;
		}
		ConnectedComponents[GlobalChoice.ChildComponentID] = true;
		SelectedBridges.Add(GlobalChoice);
	}
	TMultiMap<int32, FBridgeNeighbor> BridgeNeighbors;
	for (const FSelectedComponentBridge& Bridge : SelectedBridges)
	{
		const double Length = FVector::Distance(
			GridPosition(Bridge.ParentCellIndex, Dimensions, SDF.GridOrigin, CellSize),
			GridPosition(Bridge.ChildCellIndex, Dimensions, SDF.GridOrigin, CellSize));
		BridgeNeighbors.Add(
			Bridge.ParentCellIndex,
			FBridgeNeighbor{Bridge.ChildCellIndex, Length});
		BridgeNeighbors.Add(
			Bridge.ChildCellIndex,
			FBridgeNeighbor{Bridge.ParentCellIndex, Length});
	}
	for (int32 ComponentID = 0; ComponentID < Components.Num(); ++ComponentID)
	{
		if (!ConnectedComponents[ComponentID])
		{
			continue;
		}
		++Result.ConnectedComponentCount;
		Result.ConnectedInteriorVoxelCount += Components[ComponentID].InteriorCellCount;
	}

	TBitArray<> Anchors(false, GridCellCount);
	Anchors[RootIndex] = true;
	for (const FSelectedComponentBridge& Bridge : SelectedBridges)
	{
		Anchors[Bridge.ParentCellIndex] = true;
		Anchors[Bridge.ChildCellIndex] = true;
	}
	TBitArray<> Skeleton(false, GridCellCount);
	for (int32 Index = 0; Index < GridCellCount; ++Index)
	{
		const int32 ComponentID = ComponentIDs[Index];
		Skeleton[Index] = Interior[Index]
			&& ComponentID >= 0
			&& ComponentID < ConnectedComponents.Num()
			&& ConnectedComponents[ComponentID];
	}
	ThinWoodVolume(Dimensions, SDF, Anchors, Skeleton);

	TArray<double> Distances;
	TArray<int32> Predecessors;
	BuildRootedSkeletonTree(
		RootIndex,
		Dimensions,
		SDF.GridOrigin,
		CellSize,
		Skeleton,
		ComponentIDs,
		BridgeNeighbors,
		Distances,
		Predecessors);
	for (int32 Index = 0; Index < GridCellCount; ++Index)
	{
		if (Skeleton[Index]
			&& Distances[Index] == TNumericLimits<double>::Max())
		{
			Skeleton[Index] = false;
		}
	}
	for (;;)
	{
		TArray<int32> PruneChildCounts;
		PruneChildCounts.Init(0, GridCellCount);
		for (int32 Index = 0; Index < GridCellCount; ++Index)
		{
			if (Skeleton[Index]
				&& Predecessors[Index] != INDEX_NONE
				&& Skeleton[Predecessors[Index]])
			{
				++PruneChildCounts[Predecessors[Index]];
			}
		}
		bool bRemovedPath = false;
		for (int32 TipIndex = 0; TipIndex < GridCellCount; ++TipIndex)
		{
			if (!Skeleton[TipIndex]
				|| TipIndex == RootIndex
				|| PruneChildCounts[TipIndex] != 0)
			{
				continue;
			}
			TArray<int32> TerminalPath;
			TArray<double> TerminalRadii;
			double TerminalLength = 0.0;
			bool bContainsAnchor = false;
			int32 CurrentIndex = TipIndex;
			while (CurrentIndex != INDEX_NONE && CurrentIndex != RootIndex)
			{
				TerminalPath.Add(CurrentIndex);
				TerminalRadii.Add(FMath::Max(
					CellSize,
					-static_cast<double>(SDF.Grid[CurrentIndex])));
				bContainsAnchor = bContainsAnchor || Anchors[CurrentIndex];
				const int32 ParentIndex = Predecessors[CurrentIndex];
				if (ParentIndex == INDEX_NONE)
				{
					break;
				}
				TerminalLength += FVector::Distance(
					GridPosition(CurrentIndex, Dimensions, SDF.GridOrigin, CellSize),
					GridPosition(ParentIndex, Dimensions, SDF.GridOrigin, CellSize));
				if (ParentIndex == RootIndex || PruneChildCounts[ParentIndex] > 1)
				{
					break;
				}
				CurrentIndex = ParentIndex;
			}
			if (TerminalRadii.IsEmpty() || bContainsAnchor)
			{
				continue;
			}
			TerminalRadii.Sort();
			const double MedianRadius = TerminalRadii[TerminalRadii.Num() / 2];
			const double MinimumLength = FMath::Max(
				CellSize * MinimumExtractedPathCellCount,
				MedianRadius * MinimumExtractedPathRadiusScale);
			if (TerminalLength >= MinimumLength)
			{
				continue;
			}
			for (const int32 PathCellIndex : TerminalPath)
			{
				Skeleton[PathCellIndex] = false;
			}
			bRemovedPath = true;
		}
		if (!bRemovedPath)
		{
			break;
		}
		BuildRootedSkeletonTree(
			RootIndex,
			Dimensions,
			SDF.GridOrigin,
			CellSize,
			Skeleton,
			ComponentIDs,
			BridgeNeighbors,
			Distances,
			Predecessors);
	}
	for (int32 Index = 0; Index < GridCellCount; ++Index)
	{
		if (Skeleton[Index]
			&& Distances[Index] == TNumericLimits<double>::Max())
		{
			Skeleton[Index] = false;
		}
	}

	TArray<int32> FirstChild;
	FirstChild.Init(INDEX_NONE, GridCellCount);
	TArray<int32> NextSibling;
	NextSibling.Init(INDEX_NONE, GridCellCount);
	TArray<int32> ChildCounts;
	ChildCounts.Init(0, GridCellCount);
	for (int32 Index = 0; Index < GridCellCount; ++Index)
	{
		if (!Skeleton[Index])
		{
			continue;
		}
		const int32 ParentIndex = Predecessors[Index];
		if (ParentIndex == INDEX_NONE || !Skeleton[ParentIndex])
		{
			continue;
		}
		NextSibling[Index] = FirstChild[ParentIndex];
		FirstChild[ParentIndex] = Index;
		++ChildCounts[ParentIndex];
	}

	TArray<int32> NodeIDByGridIndex;
	NodeIDByGridIndex.Init(INDEX_NONE, GridCellCount);
	for (int32 Index = 0; Index < GridCellCount; ++Index)
	{
		if (!Skeleton[Index]
			|| (Index != RootIndex && ChildCounts[Index] == 1))
		{
			continue;
		}
		FFoliageBakerTreeSkeletonNode& Node = Result.Nodes.AddDefaulted_GetRef();
		Node.NodeID = Result.Nodes.Num() - 1;
		Node.Position = Index == RootIndex
			? Pivot
			: GridPosition(Index, Dimensions, SDF.GridOrigin, CellSize);
		Node.Radius = FMath::Max(
			CellSize,
			-static_cast<double>(SDF.Grid[Index]));
		Node.Kind = Index == RootIndex
			? EFoliageBakerTreeSkeletonNodeKind::Root
			: ChildCounts[Index] > 1
				? EFoliageBakerTreeSkeletonNodeKind::Fork
				: EFoliageBakerTreeSkeletonNodeKind::Tip;
		NodeIDByGridIndex[Index] = Node.NodeID;
	}
	Result.RootNodeID = NodeIDByGridIndex[RootIndex];

	for (int32 StartGridIndex = 0;
		StartGridIndex < GridCellCount;
		++StartGridIndex)
	{
		const int32 StartNodeID = NodeIDByGridIndex[StartGridIndex];
		if (StartNodeID == INDEX_NONE)
		{
			continue;
		}
		for (int32 ChildIndex = FirstChild[StartGridIndex];
			ChildIndex != INDEX_NONE;
			ChildIndex = NextSibling[ChildIndex])
		{
			TArray<int32> EdgeCells;
			EdgeCells.Add(StartGridIndex);
			int32 CurrentIndex = ChildIndex;
			while (CurrentIndex != INDEX_NONE)
			{
				EdgeCells.Add(CurrentIndex);
				if (NodeIDByGridIndex[CurrentIndex] != INDEX_NONE)
				{
					break;
				}
				CurrentIndex = FirstChild[CurrentIndex];
			}
			if (CurrentIndex == INDEX_NONE
				|| NodeIDByGridIndex[CurrentIndex] == INDEX_NONE)
			{
				continue;
			}

			FFoliageBakerTreeSkeletonEdge& Edge = Result.Edges.AddDefaulted_GetRef();
			Edge.EdgeID = Result.Edges.Num() - 1;
			Edge.StartNodeID = StartNodeID;
			Edge.EndNodeID = NodeIDByGridIndex[CurrentIndex];
			TArray<double> Radii;
			for (const int32 EdgeCellIndex : EdgeCells)
			{
				Edge.Polyline.Add(EdgeCellIndex == RootIndex
					? Pivot
					: GridPosition(
						EdgeCellIndex,
						Dimensions,
						SDF.GridOrigin,
						CellSize));
				Radii.Add(FMath::Max(
					CellSize,
					-static_cast<double>(SDF.Grid[EdgeCellIndex])));
			}
			SmoothAndSimplifyPolyline(Edge.Polyline, CellSize);
			Radii.Sort();
			Edge.Radius = Radii.IsEmpty()
				? CellSize
				: Radii[Radii.Num() / 2];
			Result.Nodes[Edge.EndNodeID].ParentNodeID = StartNodeID;
		}
	}
	const int32 BranchCount = ApplyTreeSemantics(Pivot, Result);

	for (const FFoliageBakerTreeSkeletonTriangle& Triangle : Triangles)
	{
		if (Triangle.A.ContainsNaN()
			|| Triangle.B.ContainsNaN()
			|| Triangle.C.ContainsNaN())
		{
			continue;
		}
		const TStaticArray<FVector, 4> Samples{
			Triangle.A,
			Triangle.B,
			Triangle.C,
			(Triangle.A + Triangle.B + Triangle.C) / 3.0};
		double TriangleMaximumRatio = 0.0;
		for (const FVector& Sample : Samples)
		{
			const FSkeletonEdgeLocation Location =
				FindClosestSkeletonEdgeLocation(Sample, Result.Edges);
			if (!Result.Edges.IsValidIndex(Location.EdgeIndex))
			{
				continue;
			}
			const double CoverageRadius = FMath::Max(
				Result.Edges[Location.EdgeIndex].Radius + CellSize * 0.75,
				CellSize);
			const double CoverageRatio =
				FMath::Sqrt(Location.DistanceSquared) / CoverageRadius;
			if (CoverageRatio > TriangleMaximumRatio)
			{
				TriangleMaximumRatio = CoverageRatio;
			}
		}
		Result.MaximumWoodCoverageRatio = FMath::Max(
			Result.MaximumWoodCoverageRatio,
			TriangleMaximumRatio);
		if (TriangleMaximumRatio > 1.75)
		{
			++Result.UncoveredWoodTriangleCount;
			FCoverageDiagnostic& Diagnostic =
				CoverageByComponent.FindOrAdd(Triangle.SourceComponentID);
			++Diagnostic.TriangleCount;
			if (TriangleMaximumRatio > Diagnostic.MaximumRatio)
			{
				Diagnostic.MaximumRatio = TriangleMaximumRatio;
				Diagnostic.WorstPosition =
					(Triangle.A + Triangle.B + Triangle.C) / 3.0;
			}
		}
	}
	TArray<int32> CoverageComponentIDs;
	CoverageByComponent.GetKeys(CoverageComponentIDs);
	CoverageComponentIDs.Sort(
		[&CoverageByComponent](const int32 First, const int32 Second)
		{
			const FCoverageDiagnostic FirstDiagnostic =
				CoverageByComponent.FindRef(First);
			const FCoverageDiagnostic SecondDiagnostic =
				CoverageByComponent.FindRef(Second);
			if (FirstDiagnostic.TriangleCount != SecondDiagnostic.TriangleCount)
			{
				return FirstDiagnostic.TriangleCount > SecondDiagnostic.TriangleCount;
			}
			return First < Second;
		});
	TArray<FString> CoverageDiagnostics;
	for (int32 Index = 0;
		Index < FMath::Min(12, CoverageComponentIDs.Num());
		++Index)
	{
		const int32 ComponentID = CoverageComponentIDs[Index];
		const FCoverageDiagnostic Diagnostic =
			CoverageByComponent.FindRef(ComponentID);
		CoverageDiagnostics.Add(FString::Printf(
			TEXT("%d:%d/%.2f@%s"),
			ComponentID,
			Diagnostic.TriangleCount,
			Diagnostic.MaximumRatio,
			*Diagnostic.WorstPosition.ToCompactString()));
	}

	Result.bSucceeded = !Result.Nodes.IsEmpty() && !Result.Edges.IsEmpty();
	Result.Report = FString::Printf(
		TEXT("fallback voxel skeleton: %d occupied cell(s), %d volume component(s), %d connected component(s), %d connected interior cell(s), %d uncovered wood triangle(s) [%s], %.2f maximum wood coverage ratio, %d node(s), %d edge(s), %d branch group(s), %.3f cm global cell size"),
		Result.OccupiedVoxelCount,
		Result.OccupiedComponentCount,
		Result.ConnectedComponentCount,
		Result.ConnectedInteriorVoxelCount,
		Result.UncoveredWoodTriangleCount,
		*FString::Join(CoverageDiagnostics, TEXT(",")),
		Result.MaximumWoodCoverageRatio,
		Result.Nodes.Num(),
		Result.Edges.Num(),
		BranchCount,
		Result.CellSize);
	return Result;
}
