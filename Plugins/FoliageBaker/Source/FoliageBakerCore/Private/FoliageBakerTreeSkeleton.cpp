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

	constexpr int32 TargetVoxelResolution = 400;
	constexpr int32 CpuFallbackVoxelResolution = 400;
	constexpr bool bUseGpuSkeleton = true;
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
	constexpr double RootBandBaseDiameterScale = 2.0;
	constexpr double MinimumRootBandLengthFraction = 0.02;
	constexpr double MaximumRootBandLengthFraction = 0.08;
	constexpr double RootMaximumRisePersistenceFraction = 0.25;

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

	void MarkPathNeighborhoodCovered(
		const TArray<int32>& Path,
		const FVector3i& Dimensions,
		const TArray<int32>& ComponentIDs,
		const TBitArray<>& ConnectedComponents,
		const TBitArray<>& Interior,
		const TSweepingMeshSDF<FDynamicMesh3>& SDF,
		const double CellSize,
		TBitArray<>& Covered)
	{
		for (const int32 PathCellIndex : Path)
		{
			const double Radius = FMath::Max(
				CellSize,
				-static_cast<double>(SDF.Grid[PathCellIndex]));
			const int32 CoverageCells = FMath::Clamp(
				FMath::CeilToInt(
					Radius / CellSize * CoverageRadiusScale),
				1,
				24);
			const FVector3i Center = GridCoordinate(
				PathCellIndex,
				Dimensions);
			for (int32 OffsetZ = -CoverageCells;
				OffsetZ <= CoverageCells;
				++OffsetZ)
			{
				for (int32 OffsetY = -CoverageCells;
					OffsetY <= CoverageCells;
					++OffsetY)
				{
					for (int32 OffsetX = -CoverageCells;
						OffsetX <= CoverageCells;
						++OffsetX)
					{
						if (OffsetX * OffsetX
							+ OffsetY * OffsetY
							+ OffsetZ * OffsetZ
							> CoverageCells * CoverageCells)
						{
							continue;
						}
						const FVector3i CoveredCoordinate = Center
							+ FVector3i(OffsetX, OffsetY, OffsetZ);
						if (!IsInsideGrid(CoveredCoordinate, Dimensions))
						{
							continue;
						}
						const int32 CoveredIndex = GridIndex(
							CoveredCoordinate,
							Dimensions);
						const int32 ComponentID = ComponentIDs[CoveredIndex];
						if (ComponentID >= 0
							&& ComponentID < ConnectedComponents.Num()
							&& ConnectedComponents[ComponentID]
							&& Interior[CoveredIndex])
						{
							Covered[CoveredIndex] = true;
						}
					}
				}
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

	struct FSurfaceVertex
	{
		FVector Position = FVector::ZeroVector;
		TArray<int32> Neighbors;
		TArray<int32> IncidentTriangles;
		TArray<int32> BoundaryNeighbors;
	};

	struct FSurfaceTriangle
	{
		TStaticArray<int32, 3> VertexIndices;
		int32 SourceTriangleID = INDEX_NONE;
		int32 BandIndex = INDEX_NONE;
		int32 ClusterIndex = INDEX_NONE;
	};

	struct FSurfaceQueueEntry
	{
		int32 Index = INDEX_NONE;
		double Distance = TNumericLimits<double>::Max();
	};

	struct FSurfaceQueueEntryLess
	{
		bool operator()(
			const FSurfaceQueueEntry& First,
			const FSurfaceQueueEntry& Second) const
		{
			return First.Distance < Second.Distance;
		}
	};

	struct FSurfaceCluster
	{
		TArray<int32> TriangleIndices;
		TArray<int32> Neighbors;
		FVector Center = FVector::ZeroVector;
		double Radius = 0.0;
		int32 MinimumSourceTriangleID = MAX_int32;
		int32 ParentIndex = INDEX_NONE;
		TArray<int32> ChildIndices;
		double RootDistance = TNumericLimits<double>::Max();
	};

	class FSurfaceDisjointSet final
	{
	public:
		explicit FSurfaceDisjointSet(const int32 Count)
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

	uint64 SurfaceEdgeKey(const int32 First, const int32 Second)
	{
		const uint32 MinimumIndex = static_cast<uint32>(FMath::Min(First, Second));
		const uint32 MaximumIndex = static_cast<uint32>(FMath::Max(First, Second));
		return (static_cast<uint64>(MinimumIndex) << 32)
			| static_cast<uint64>(MaximumIndex);
	}

	uint64 SurfaceClusterPairKey(const int32 First, const int32 Second)
	{
		return SurfaceEdgeKey(First, Second);
	}

	int32 AddSurfaceVertex(
		const FVector& Position,
		TMap<FVector3f, int32>& VertexIndexByPosition,
		TArray<FSurfaceVertex>& Vertices)
	{
		const FVector3f PositionKey(Position);
		const int32 ExistingIndexPlusOne = VertexIndexByPosition.FindRef(PositionKey);
		if (ExistingIndexPlusOne > 0)
		{
			return ExistingIndexPlusOne - 1;
		}
		const int32 NewIndex = Vertices.Num();
		FSurfaceVertex& Vertex = Vertices.AddDefaulted_GetRef();
		Vertex.Position = Position;
		VertexIndexByPosition.Add(PositionKey, NewIndex + 1);
		return NewIndex;
	}

	void AddSurfaceNeighbor(
		const int32 First,
		const int32 Second,
		TArray<FSurfaceVertex>& Vertices)
	{
		if (First == Second
			|| !Vertices.IsValidIndex(First)
			|| !Vertices.IsValidIndex(Second))
		{
			return;
		}
		Vertices[First].Neighbors.AddUnique(Second);
		Vertices[Second].Neighbors.AddUnique(First);
	}

	TArray<int32> SelectSurfaceRootVertices(
		const TArray<FSurfaceVertex>& Vertices,
		const TMap<uint64, int32>& EdgeUseCounts,
		const FVector& Pivot,
		const double GeometryScale)
	{
		TArray<int32> RootVertices;
		TBitArray<> BoundaryVertices(false, Vertices.Num());
		for (const TPair<uint64, int32>& EdgeUseCount : EdgeUseCounts)
		{
			if (EdgeUseCount.Value != 1)
			{
				continue;
			}
			const int32 First = static_cast<int32>(EdgeUseCount.Key >> 32);
			const int32 Second = static_cast<int32>(EdgeUseCount.Key & 0xffffffffu);
			if (BoundaryVertices.IsValidIndex(First)
				&& BoundaryVertices.IsValidIndex(Second))
			{
				BoundaryVertices[First] = true;
				BoundaryVertices[Second] = true;
			}
		}

		struct FBoundaryLoop
		{
			TArray<int32> VertexIndices;
			double Perimeter = 0.0;
			FVector Center = FVector::ZeroVector;
		};
		TArray<FBoundaryLoop> BoundaryLoops;
		TBitArray<> Visited(false, Vertices.Num());
		for (int32 SeedIndex = 0; SeedIndex < Vertices.Num(); ++SeedIndex)
		{
			if (!BoundaryVertices[SeedIndex] || Visited[SeedIndex])
			{
				continue;
			}
			FBoundaryLoop& Loop = BoundaryLoops.AddDefaulted_GetRef();
			TArray<int32> Queue;
			Queue.Add(SeedIndex);
			Visited[SeedIndex] = true;
			for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
			{
				const int32 VertexIndex = Queue[QueueIndex];
				Loop.VertexIndices.Add(VertexIndex);
				Loop.Center += Vertices[VertexIndex].Position;
				for (const int32 NeighborIndex : Vertices[VertexIndex].BoundaryNeighbors)
				{
					if (VertexIndex < NeighborIndex)
					{
						Loop.Perimeter += FVector::Distance(
							Vertices[VertexIndex].Position,
							Vertices[NeighborIndex].Position);
					}
					if (!Visited[NeighborIndex])
					{
						Visited[NeighborIndex] = true;
						Queue.Add(NeighborIndex);
					}
				}
			}
			if (!Loop.VertexIndices.IsEmpty())
			{
				Loop.Center /= static_cast<double>(Loop.VertexIndices.Num());
			}
		}

		double MaximumPerimeter = 0.0;
		for (const FBoundaryLoop& Loop : BoundaryLoops)
		{
			if (Loop.VertexIndices.Num() >= 3)
			{
				MaximumPerimeter = FMath::Max(MaximumPerimeter, Loop.Perimeter);
			}
		}
		double BestDistanceSquared = TNumericLimits<double>::Max();
		int32 BestLoopIndex = INDEX_NONE;
		for (int32 LoopIndex = 0; LoopIndex < BoundaryLoops.Num(); ++LoopIndex)
		{
			const FBoundaryLoop& Loop = BoundaryLoops[LoopIndex];
			if (Loop.VertexIndices.Num() < 3
				|| Loop.Perimeter < MaximumPerimeter * 0.55)
			{
				continue;
			}
			const double DistanceSquared = FVector::DistSquared(Loop.Center, Pivot);
			if (DistanceSquared < BestDistanceSquared)
			{
				BestDistanceSquared = DistanceSquared;
				BestLoopIndex = LoopIndex;
			}
		}
		if (BoundaryLoops.IsValidIndex(BestLoopIndex))
		{
			return BoundaryLoops[BestLoopIndex].VertexIndices;
		}

		int32 NearestVertexIndex = INDEX_NONE;
		for (int32 VertexIndex = 0; VertexIndex < Vertices.Num(); ++VertexIndex)
		{
			const double DistanceSquared = FVector::DistSquared(
				Vertices[VertexIndex].Position,
				Pivot);
			if (DistanceSquared < BestDistanceSquared)
			{
				BestDistanceSquared = DistanceSquared;
				NearestVertexIndex = VertexIndex;
			}
		}
		if (Vertices.IsValidIndex(NearestVertexIndex))
		{
			const double PatchRadiusSquared = FMath::Square(
				FMath::Max(GeometryScale * 2.5, UE_DOUBLE_SMALL_NUMBER));
			const FVector RootPosition = Vertices[NearestVertexIndex].Position;
			for (int32 VertexIndex = 0; VertexIndex < Vertices.Num(); ++VertexIndex)
			{
				if (FVector::DistSquared(Vertices[VertexIndex].Position, RootPosition)
					<= PatchRadiusSquared)
				{
					RootVertices.Add(VertexIndex);
				}
			}
			if (RootVertices.IsEmpty())
			{
				RootVertices.Add(NearestVertexIndex);
			}
		}
		return RootVertices;
	}

	void MeasureSurfaceRootDistances(
		const TArray<FSurfaceVertex>& Vertices,
		const TArray<int32>& RootVertices,
		TArray<double>& OutDistances)
	{
		OutDistances.Init(TNumericLimits<double>::Max(), Vertices.Num());
		TArray<FSurfaceQueueEntry> Queue;
		const FSurfaceQueueEntryLess QueueEntryLess;
		for (const int32 RootVertexIndex : RootVertices)
		{
			if (!Vertices.IsValidIndex(RootVertexIndex))
			{
				continue;
			}
			OutDistances[RootVertexIndex] = 0.0;
			Queue.HeapPush(
				FSurfaceQueueEntry{RootVertexIndex, 0.0},
				QueueEntryLess);
		}
		while (!Queue.IsEmpty())
		{
			FSurfaceQueueEntry Entry;
			Queue.HeapPop(Entry, QueueEntryLess, EAllowShrinking::No);
			if (!OutDistances.IsValidIndex(Entry.Index)
				|| Entry.Distance > OutDistances[Entry.Index])
			{
				continue;
			}
			for (const int32 NeighborIndex : Vertices[Entry.Index].Neighbors)
			{
				const double EdgeLength = FMath::Max(
					FVector::Distance(
						Vertices[Entry.Index].Position,
						Vertices[NeighborIndex].Position),
					UE_DOUBLE_SMALL_NUMBER);
				const double CandidateDistance = Entry.Distance + EdgeLength;
				if (CandidateDistance >= OutDistances[NeighborIndex])
				{
					continue;
				}
				OutDistances[NeighborIndex] = CandidateDistance;
				Queue.HeapPush(
					FSurfaceQueueEntry{NeighborIndex, CandidateDistance},
					QueueEntryLess);
			}
		}
	}

	void BuildSurfaceClusterGeometry(
		const TArray<FSurfaceTriangle>& Triangles,
		const TArray<FSurfaceVertex>& Vertices,
		FSurfaceCluster& Cluster)
	{
		TSet<int32> UniqueVertexIndices;
		for (const int32 TriangleIndex : Cluster.TriangleIndices)
		{
			if (!Triangles.IsValidIndex(TriangleIndex))
			{
				continue;
			}
			const FSurfaceTriangle& Triangle = Triangles[TriangleIndex];
			Cluster.MinimumSourceTriangleID = FMath::Min(
				Cluster.MinimumSourceTriangleID,
				Triangle.SourceTriangleID);
			for (const int32 VertexIndex : Triangle.VertexIndices)
			{
				UniqueVertexIndices.Add(VertexIndex);
			}
		}
		if (UniqueVertexIndices.IsEmpty())
		{
			return;
		}
		for (const int32 VertexIndex : UniqueVertexIndices)
		{
			Cluster.Center += Vertices[VertexIndex].Position;
		}
		Cluster.Center /= static_cast<double>(UniqueVertexIndices.Num());
		TArray<double> RadialDistances;
		RadialDistances.Reserve(UniqueVertexIndices.Num());
		for (const int32 VertexIndex : UniqueVertexIndices)
		{
			RadialDistances.Add(FVector::Distance(
				Vertices[VertexIndex].Position,
				Cluster.Center));
		}
		RadialDistances.Sort();
		Cluster.Radius = RadialDistances[RadialDistances.Num() / 4];
	}

	FVector FindSurfaceTerminalCenter(
		const int32 ClusterIndex,
		const TArray<FSurfaceCluster>& Clusters,
		const TArray<FSurfaceTriangle>& Triangles,
		const TArray<FSurfaceVertex>& Vertices,
		const double GeometryScale)
	{
		if (!Clusters.IsValidIndex(ClusterIndex))
		{
			return FVector::ZeroVector;
		}
		const FSurfaceCluster& Cluster = Clusters[ClusterIndex];
		if (!Clusters.IsValidIndex(Cluster.ParentIndex))
		{
			return Cluster.Center;
		}
		const FVector Direction = (
			Cluster.Center - Clusters[Cluster.ParentIndex].Center).GetSafeNormal();
		if (Direction.IsNearlyZero())
		{
			return Cluster.Center;
		}

		TSet<int32> UniqueVertexIndices;
		double MaximumProjection = TNumericLimits<double>::Lowest();
		for (const int32 TriangleIndex : Cluster.TriangleIndices)
		{
			if (!Triangles.IsValidIndex(TriangleIndex))
			{
				continue;
			}
			for (const int32 VertexIndex : Triangles[TriangleIndex].VertexIndices)
			{
				if (!Vertices.IsValidIndex(VertexIndex))
				{
					continue;
				}
				UniqueVertexIndices.Add(VertexIndex);
				MaximumProjection = FMath::Max(
					MaximumProjection,
					FVector::DotProduct(Vertices[VertexIndex].Position, Direction));
			}
		}
		if (UniqueVertexIndices.IsEmpty()
			|| !FMath::IsFinite(MaximumProjection))
		{
			return Cluster.Center;
		}

		const double ProjectionTolerance = FMath::Max3(
			Cluster.Radius * 0.35,
			GeometryScale * 0.05,
			UE_DOUBLE_SMALL_NUMBER);
		FVector TerminalCenter = FVector::ZeroVector;
		int32 TerminalVertexCount = 0;
		for (const int32 VertexIndex : UniqueVertexIndices)
		{
			const FVector& Position = Vertices[VertexIndex].Position;
			const double Projection = FVector::DotProduct(Position, Direction);
			if (Projection < MaximumProjection - ProjectionTolerance)
			{
				continue;
			}
			TerminalCenter += Position;
			++TerminalVertexCount;
		}
		return TerminalVertexCount > 0
			? TerminalCenter / static_cast<double>(TerminalVertexCount)
			: Cluster.Center;
	}

	void AddDistinctSurfaceGuidePoint(
		TArray<FVector>& Polyline,
		const FVector& Point,
		const double GeometryScale)
	{
		if (Polyline.IsEmpty()
			|| FVector::DistSquared(Polyline.Last(), Point)
				> FMath::Square(FMath::Max(GeometryScale * 0.1, UE_DOUBLE_SMALL_NUMBER)))
		{
			Polyline.Add(Point);
		}
	}

	struct FSkeletonEdgeLocation
	{
		int32 EdgeIndex = INDEX_NONE;
		int32 SegmentIndex = INDEX_NONE;
		FVector Position = FVector::ZeroVector;
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
					Result.SegmentIndex = SegmentIndex;
					Result.Position = Candidate;
					Result.DistanceSquared = DistanceSquared;
				}
			}
		}
		return Result;
	}

	double GuideCoverageDistance(
		const double GuideRadius,
		const double CellSize)
	{
		return FMath::Max(
			CellSize * 0.75,
			FMath::Min(GuideRadius * 0.5, CellSize * 1.25));
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
			BaseTrunkDiameter * RootBandBaseDiameterScale,
			TrunkLength * MinimumRootBandLengthFraction,
			TrunkLength * MaximumRootBandLengthFraction);
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
					PersistenceLength * RootMaximumRisePersistenceFraction,
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

	TBitArray<> ThinSparseSkeletonVolume(
		const int32 RootSparseIndex,
		const FVector3i& Dimensions,
		const TArray<int32>& VoxelIndices,
		const TMap<int32, int32>& SparseIndexByVoxel,
		const TArray<double>& Radii)
	{
		const int32 GridCellCount = Dimensions.X * Dimensions.Y * Dimensions.Z;
		TBitArray<> Volume(false, GridCellCount);
		for (const int32 VoxelIndex : VoxelIndices)
		{
			Volume[VoxelIndex] = true;
		}
		const int32 RootVoxelIndex = VoxelIndices[RootSparseIndex];
		TBitArray<> Queued(false, GridCellCount);
		TArray<FQueueEntry> Queue;
		const FQueueEntryLess QueueEntryLess;
		for (int32 SparseIndex = 0; SparseIndex < VoxelIndices.Num(); ++SparseIndex)
		{
			const int32 VoxelIndex = VoxelIndices[SparseIndex];
			if (VoxelIndex == RootVoxelIndex
				|| !IsVolumeBoundaryCell(VoxelIndex, Dimensions, Volume))
			{
				continue;
			}
			Queue.HeapPush(
				FQueueEntry{VoxelIndex, Radii[SparseIndex]},
				QueueEntryLess);
			Queued[VoxelIndex] = true;
		}

		while (!Queue.IsEmpty())
		{
			FQueueEntry Entry;
			Queue.HeapPop(Entry, QueueEntryLess, EAllowShrinking::No);
			Queued[Entry.Index] = false;
			if (!Volume[Entry.Index]
				|| Entry.Index == RootVoxelIndex
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
						const FVector3i NeighborCoordinate = Coordinate
							+ FVector3i(OffsetX, OffsetY, OffsetZ);
						if (!IsInsideGrid(NeighborCoordinate, Dimensions))
						{
							continue;
						}
						const int32 NeighborVoxelIndex = GridIndex(
							NeighborCoordinate,
							Dimensions);
						if (!Volume[NeighborVoxelIndex]
							|| NeighborVoxelIndex == RootVoxelIndex
							|| Queued[NeighborVoxelIndex]
							|| !IsVolumeBoundaryCell(
								NeighborVoxelIndex,
								Dimensions,
								Volume))
						{
							continue;
						}
						const int32 NeighborSparseIndex = SparseIndexByVoxel.FindRef(
							NeighborVoxelIndex);
						Queue.HeapPush(
							FQueueEntry{
								NeighborVoxelIndex,
								Radii[NeighborSparseIndex]},
							QueueEntryLess);
						Queued[NeighborVoxelIndex] = true;
					}
				}
			}
		}

		TBitArray<> Retained(false, VoxelIndices.Num());
		for (int32 SparseIndex = 0; SparseIndex < VoxelIndices.Num(); ++SparseIndex)
		{
			Retained[SparseIndex] = Volume[VoxelIndices[SparseIndex]];
		}
		return Retained;
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

TArray<FFoliageBakerTreeSkeletonGuide>
FFoliageBakerTreeSkeleton::BuildSurfaceGuides(
	const TArray<FFoliageBakerTreeSkeletonTriangle>& Triangles,
	const int32 SourceComponentID,
	const double GeometryScale,
	const FVector& Pivot)
{
	TArray<FFoliageBakerTreeSkeletonGuide> Result;
	if (Triangles.IsEmpty() || GeometryScale <= UE_DOUBLE_SMALL_NUMBER)
	{
		return Result;
	}

	TArray<int32> TriangleOrder;
	TriangleOrder.Reserve(Triangles.Num());
	for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
	{
		if (!Triangles[TriangleIndex].A.ContainsNaN()
			&& !Triangles[TriangleIndex].B.ContainsNaN()
			&& !Triangles[TriangleIndex].C.ContainsNaN())
		{
			TriangleOrder.Add(TriangleIndex);
		}
	}
	TriangleOrder.Sort(
		[&Triangles](const int32 First, const int32 Second)
		{
			return Triangles[First].SourceTriangleID
				< Triangles[Second].SourceTriangleID;
		});
	if (TriangleOrder.IsEmpty())
	{
		return Result;
	}

	TMap<FVector3f, int32> VertexIndexByPosition;
	TArray<FSurfaceVertex> Vertices;
	TArray<FSurfaceTriangle> SurfaceTriangles;
	SurfaceTriangles.Reserve(TriangleOrder.Num());
	TMap<uint64, int32> EdgeUseCounts;
	TMap<uint64, TArray<int32>> TriangleIndicesByEdge;
	for (const int32 SourceTriangleIndex : TriangleOrder)
	{
		const FFoliageBakerTreeSkeletonTriangle& SourceTriangle =
			Triangles[SourceTriangleIndex];
		FSurfaceTriangle& Triangle = SurfaceTriangles.AddDefaulted_GetRef();
		Triangle.SourceTriangleID = SourceTriangle.SourceTriangleID;
		Triangle.VertexIndices[0] = AddSurfaceVertex(
			SourceTriangle.A,
			VertexIndexByPosition,
			Vertices);
		Triangle.VertexIndices[1] = AddSurfaceVertex(
			SourceTriangle.B,
			VertexIndexByPosition,
			Vertices);
		Triangle.VertexIndices[2] = AddSurfaceVertex(
			SourceTriangle.C,
			VertexIndexByPosition,
			Vertices);
		const int32 LocalTriangleIndex = SurfaceTriangles.Num() - 1;
		for (const int32 VertexIndex : Triangle.VertexIndices)
		{
			Vertices[VertexIndex].IncidentTriangles.Add(LocalTriangleIndex);
		}
		for (int32 EdgeIndex = 0; EdgeIndex < 3; ++EdgeIndex)
		{
			const int32 First = Triangle.VertexIndices[EdgeIndex];
			const int32 Second = Triangle.VertexIndices[(EdgeIndex + 1) % 3];
			if (First == Second)
			{
				continue;
			}
			AddSurfaceNeighbor(First, Second, Vertices);
			const uint64 EdgeKey = SurfaceEdgeKey(First, Second);
			EdgeUseCounts.FindOrAdd(EdgeKey) += 1;
			TriangleIndicesByEdge.FindOrAdd(EdgeKey).Add(LocalTriangleIndex);
		}
	}
	if (Vertices.Num() < 2 || SurfaceTriangles.IsEmpty())
	{
		return Result;
	}
	for (const TPair<uint64, int32>& EdgeUseCount : EdgeUseCounts)
	{
		if (EdgeUseCount.Value != 1)
		{
			continue;
		}
		const int32 First = static_cast<int32>(EdgeUseCount.Key >> 32);
		const int32 Second = static_cast<int32>(EdgeUseCount.Key & 0xffffffffu);
		if (Vertices.IsValidIndex(First) && Vertices.IsValidIndex(Second))
		{
			Vertices[First].BoundaryNeighbors.AddUnique(Second);
			Vertices[Second].BoundaryNeighbors.AddUnique(First);
		}
	}

	const TArray<int32> RootVertices = SelectSurfaceRootVertices(
		Vertices,
		EdgeUseCounts,
		Pivot,
		GeometryScale);
	if (RootVertices.IsEmpty())
	{
		return Result;
	}
	TArray<double> VertexRootDistances;
	MeasureSurfaceRootDistances(Vertices, RootVertices, VertexRootDistances);
	double MaximumRootDistance = 0.0;
	for (const double Distance : VertexRootDistances)
	{
		if (FMath::IsFinite(Distance))
		{
			MaximumRootDistance = FMath::Max(MaximumRootDistance, Distance);
		}
	}
	if (MaximumRootDistance <= UE_DOUBLE_SMALL_NUMBER)
	{
		return Result;
	}
	const double BandSize = FMath::Max(
		GeometryScale * 2.0,
		MaximumRootDistance / 160.0);
	for (FSurfaceTriangle& Triangle : SurfaceTriangles)
	{
		TStaticArray<double, 3> Distances = {
			VertexRootDistances[Triangle.VertexIndices[0]],
			VertexRootDistances[Triangle.VertexIndices[1]],
			VertexRootDistances[Triangle.VertexIndices[2]]};
		if (Distances[0] > Distances[1])
		{
			Swap(Distances[0], Distances[1]);
		}
		if (Distances[1] > Distances[2])
		{
			Swap(Distances[1], Distances[2]);
		}
		if (Distances[0] > Distances[1])
		{
			Swap(Distances[0], Distances[1]);
		}
		Triangle.BandIndex = FMath::Max(
			0,
			FMath::FloorToInt(Distances[1] / BandSize));
	}

	FSurfaceDisjointSet TriangleSets(SurfaceTriangles.Num());
	for (const FSurfaceVertex& Vertex : Vertices)
	{
		for (int32 FirstIndex = 0;
			FirstIndex < Vertex.IncidentTriangles.Num();
			++FirstIndex)
		{
			const int32 FirstTriangleIndex =
				Vertex.IncidentTriangles[FirstIndex];
			for (int32 SecondIndex = FirstIndex + 1;
				SecondIndex < Vertex.IncidentTriangles.Num();
				++SecondIndex)
			{
				const int32 SecondTriangleIndex =
					Vertex.IncidentTriangles[SecondIndex];
				if (SurfaceTriangles[FirstTriangleIndex].BandIndex
					== SurfaceTriangles[SecondTriangleIndex].BandIndex)
				{
					TriangleSets.Union(FirstTriangleIndex, SecondTriangleIndex);
				}
			}
		}
	}

	TMap<int32, int32> ClusterIndexBySetRoot;
	TArray<FSurfaceCluster> Clusters;
	for (int32 TriangleIndex = 0;
		TriangleIndex < SurfaceTriangles.Num();
		++TriangleIndex)
	{
		const int32 SetRoot = TriangleSets.Find(TriangleIndex);
		int32 ClusterIndexPlusOne = ClusterIndexBySetRoot.FindRef(SetRoot);
		if (ClusterIndexPlusOne == 0)
		{
			ClusterIndexPlusOne = Clusters.Num() + 1;
			Clusters.AddDefaulted();
			ClusterIndexBySetRoot.Add(SetRoot, ClusterIndexPlusOne);
		}
		const int32 ClusterIndex = ClusterIndexPlusOne - 1;
		SurfaceTriangles[TriangleIndex].ClusterIndex = ClusterIndex;
		Clusters[ClusterIndex].TriangleIndices.Add(TriangleIndex);
	}
	for (FSurfaceCluster& Cluster : Clusters)
	{
		BuildSurfaceClusterGeometry(SurfaceTriangles, Vertices, Cluster);
	}

	TMap<uint64, int32> ClusterPairSupport;
	for (const FSurfaceVertex& Vertex : Vertices)
	{
		TArray<int32> IncidentClusters;
		for (const int32 TriangleIndex : Vertex.IncidentTriangles)
		{
			IncidentClusters.AddUnique(
				SurfaceTriangles[TriangleIndex].ClusterIndex);
		}
		IncidentClusters.Sort();
		for (int32 FirstIndex = 0;
			FirstIndex < IncidentClusters.Num();
			++FirstIndex)
		{
			for (int32 SecondIndex = FirstIndex + 1;
				SecondIndex < IncidentClusters.Num();
				++SecondIndex)
			{
				const int32 FirstClusterIndex = IncidentClusters[FirstIndex];
				const int32 SecondClusterIndex = IncidentClusters[SecondIndex];
				if (FirstClusterIndex == SecondClusterIndex)
				{
					continue;
				}
				const uint64 PairKey = SurfaceClusterPairKey(
					FirstClusterIndex,
					SecondClusterIndex);
				ClusterPairSupport.FindOrAdd(PairKey) += 1;
			}
		}
	}
	for (const TPair<uint64, int32>& PairSupport : ClusterPairSupport)
	{
		const int32 First = static_cast<int32>(PairSupport.Key >> 32);
		const int32 Second = static_cast<int32>(PairSupport.Key & 0xffffffffu);
		if (Clusters.IsValidIndex(First) && Clusters.IsValidIndex(Second))
		{
			Clusters[First].Neighbors.AddUnique(Second);
			Clusters[Second].Neighbors.AddUnique(First);
		}
	}

	int32 RootClusterIndex = INDEX_NONE;
	int32 RootClusterTriangleID = MAX_int32;
	for (const int32 RootVertexIndex : RootVertices)
	{
		for (const int32 TriangleIndex : Vertices[RootVertexIndex].IncidentTriangles)
		{
			const int32 ClusterIndex = SurfaceTriangles[TriangleIndex].ClusterIndex;
			if (Clusters[ClusterIndex].MinimumSourceTriangleID < RootClusterTriangleID)
			{
				RootClusterTriangleID = Clusters[ClusterIndex].MinimumSourceTriangleID;
				RootClusterIndex = ClusterIndex;
			}
		}
	}
	if (!Clusters.IsValidIndex(RootClusterIndex))
	{
		return Result;
	}

	TArray<FSurfaceQueueEntry> ClusterQueue;
	const FSurfaceQueueEntryLess QueueEntryLess;
	Clusters[RootClusterIndex].RootDistance = 0.0;
	ClusterQueue.HeapPush(
		FSurfaceQueueEntry{RootClusterIndex, 0.0},
		QueueEntryLess);
	while (!ClusterQueue.IsEmpty())
	{
		FSurfaceQueueEntry Entry;
		ClusterQueue.HeapPop(Entry, QueueEntryLess, EAllowShrinking::No);
		if (Entry.Distance > Clusters[Entry.Index].RootDistance)
		{
			continue;
		}
		for (const int32 NeighborIndex : Clusters[Entry.Index].Neighbors)
		{
			const double StepLength = FMath::Max(
				FVector::Distance(
					Clusters[Entry.Index].Center,
					Clusters[NeighborIndex].Center),
				GeometryScale * 0.1);
			const double CandidateDistance = Entry.Distance + StepLength;
			const bool bShorter = CandidateDistance
				< Clusters[NeighborIndex].RootDistance - UE_DOUBLE_SMALL_NUMBER;
			const bool bStableTie = FMath::IsNearlyEqual(
				CandidateDistance,
				Clusters[NeighborIndex].RootDistance,
				UE_DOUBLE_SMALL_NUMBER)
				&& (Clusters[NeighborIndex].ParentIndex == INDEX_NONE
					|| Clusters[Entry.Index].MinimumSourceTriangleID
						< Clusters[Clusters[NeighborIndex].ParentIndex]
							.MinimumSourceTriangleID);
			if (!bShorter && !bStableTie)
			{
				continue;
			}
			Clusters[NeighborIndex].RootDistance = CandidateDistance;
			Clusters[NeighborIndex].ParentIndex = Entry.Index;
			ClusterQueue.HeapPush(
				FSurfaceQueueEntry{NeighborIndex, CandidateDistance},
				QueueEntryLess);
		}
	}
	for (int32 ClusterIndex = 0; ClusterIndex < Clusters.Num(); ++ClusterIndex)
	{
		const int32 ParentIndex = Clusters[ClusterIndex].ParentIndex;
		if (Clusters.IsValidIndex(ParentIndex))
		{
			Clusters[ParentIndex].ChildIndices.Add(ClusterIndex);
		}
	}
	for (FSurfaceCluster& Cluster : Clusters)
	{
		Cluster.ChildIndices.Sort(
			[&Clusters](const int32 First, const int32 Second)
			{
				return Clusters[First].MinimumSourceTriangleID
					< Clusters[Second].MinimumSourceTriangleID;
			});
	}

	TArray<int32> PendingForkIndices;
	PendingForkIndices.Add(RootClusterIndex);
	for (int32 PendingIndex = 0;
		PendingIndex < PendingForkIndices.Num();
		++PendingIndex)
	{
		const int32 ForkClusterIndex = PendingForkIndices[PendingIndex];
		for (const int32 FirstChildIndex :
			Clusters[ForkClusterIndex].ChildIndices)
		{
			TArray<FVector> Polyline;
			TArray<double> Radii;
			AddDistinctSurfaceGuidePoint(
				Polyline,
				Clusters[ForkClusterIndex].Center,
				GeometryScale);
			Radii.Add(Clusters[ForkClusterIndex].Radius);
			int32 CurrentClusterIndex = FirstChildIndex;
			while (Clusters.IsValidIndex(CurrentClusterIndex))
			{
				const FVector GuidePoint =
					Clusters[CurrentClusterIndex].ChildIndices.IsEmpty()
						? FindSurfaceTerminalCenter(
							CurrentClusterIndex,
							Clusters,
							SurfaceTriangles,
							Vertices,
							GeometryScale)
						: Clusters[CurrentClusterIndex].Center;
				AddDistinctSurfaceGuidePoint(
					Polyline,
					GuidePoint,
					GeometryScale);
				Radii.Add(Clusters[CurrentClusterIndex].Radius);
				if (Clusters[CurrentClusterIndex].ChildIndices.Num() != 1)
				{
					break;
				}
				CurrentClusterIndex =
					Clusters[CurrentClusterIndex].ChildIndices[0];
			}
			if (Clusters.IsValidIndex(CurrentClusterIndex)
				&& !Clusters[CurrentClusterIndex].ChildIndices.IsEmpty())
			{
				PendingForkIndices.Add(CurrentClusterIndex);
			}
			if (Polyline.Num() < 2)
			{
				continue;
			}
			SmoothAndSimplifyPolyline(Polyline, GeometryScale);
			const double Length = PolylineLength(Polyline);
			if (!FMath::IsFinite(Length)
				|| Length < GeometryScale * 0.5)
			{
				continue;
			}
			const double RootRadius = Radii[0];
			const double TipRadius = Radii.Last();
			Radii.Sort();
			FFoliageBakerTreeSkeletonGuide& Guide =
				Result.AddDefaulted_GetRef();
			Guide.SourceComponentID = SourceComponentID;
			Guide.bRooted = true;
			Guide.EndpointRadii[0] = FMath::Max(
				GeometryScale,
				RootRadius);
			Guide.EndpointRadii[1] = FMath::Max(
				GeometryScale,
				TipRadius);
			Guide.Radius = FMath::Max(
				GeometryScale,
				Radii[Radii.Num() / 2]);
			Guide.Polyline = MoveTemp(Polyline);
		}
	}
	return Result;
}

FFoliageBakerTreeSkeletonResult FFoliageBakerTreeSkeleton::Build(
	const TArray<FFoliageBakerTreeSkeletonTriangle>& Triangles,
	const TArray<FFoliageBakerTreeSkeletonGuide>& Guides,
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
	TMap<int32, int32> GuideCountByComponent;
	for (const FFoliageBakerTreeSkeletonGuide& Guide : Guides)
	{
		GuideCountByComponent.FindOrAdd(Guide.SourceComponentID) += 1;
	}
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

	FFoliageBakerGpuTreeSkeletonResult GpuSkeleton;
	if (bUseGpuSkeleton)
	{
		GpuSkeleton = FFoliageBakerTreeSkeletonGpu::Build(
			SolidTriangles,
			FBox(FVector(SourceBounds.Min), FVector(SourceBounds.Max)),
			Pivot,
			TargetVoxelResolution);
	}
	if (bUseGpuSkeleton
		&& GpuSkeleton.bSucceeded
		&& BuildSparseGpuSkeletonGraph(
			GpuSkeleton,
			Pivot,
			SourceSpatial,
			Result))
	{
		for (const FFoliageBakerTreeSkeletonGuide& Guide : Guides)
		{
			if (Guide.Polyline.IsEmpty())
			{
				continue;
			}
			const double CoverageDistance = GuideCoverageDistance(
				FMath::Max(Guide.Radius, Result.CellSize * 0.1),
				Result.CellSize);
			const TStaticArray<FVector, 2> Endpoints{
				Guide.Polyline[0],
				Guide.Polyline.Last()};
			const FSkeletonEdgeLocation TerminalLocation =
				FindClosestSkeletonEdgeLocation(Guide.Polyline.Last(), Result.Edges);
			Result.UncoveredGuideTerminalCount +=
				TerminalLocation.DistanceSquared > FMath::Square(CoverageDistance)
					? 1
					: 0;
			for (const FVector& Endpoint : Endpoints)
			{
				const FSkeletonEdgeLocation Location =
					FindClosestSkeletonEdgeLocation(Endpoint, Result.Edges);
				Result.UncoveredGuideEndpointCount +=
					Location.DistanceSquared > FMath::Square(CoverageDistance)
						? 1
						: 0;
			}
		}

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
				Result.UncoveredWoodSourceTriangleIDs.Add(Triangle.SourceTriangleID);
			}
		}

		const int32 BranchCount = ApplyTreeSemantics(Pivot, Result);
		Result.bSucceeded = !Result.Nodes.IsEmpty() && !Result.Edges.IsEmpty();
		Result.Report = FString::Printf(
			TEXT("%s %d node(s), %d edge(s), %d branch group(s), %d uncovered guide terminal(s), %d uncovered wood triangle(s), %.2f maximum wood coverage ratio."),
			*GpuSkeleton.Report,
			Result.Nodes.Num(),
			Result.Edges.Num(),
			BranchCount,
			Result.UncoveredGuideTerminalCount,
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
	// Surface guides are diagnostics for this high-resolution voxel pass. They must
	// not create topology by connecting to the nearest existing skeleton edge.
	for (const FFoliageBakerTreeSkeletonGuide& Guide : Guides)
	{
		const double CoverageDistance = GuideCoverageDistance(
			FMath::Max(Guide.Radius, CellSize * 0.1),
			CellSize);
		if (Guide.Polyline.IsEmpty())
		{
			continue;
		}
		const TStaticArray<FVector, 2> Endpoints{
			Guide.Polyline[0],
			Guide.Polyline.Last()};
		const FSkeletonEdgeLocation TerminalLocation =
			FindClosestSkeletonEdgeLocation(Guide.Polyline.Last(), Result.Edges);
		Result.UncoveredGuideTerminalCount +=
			TerminalLocation.DistanceSquared > FMath::Square(CoverageDistance)
				? 1
				: 0;
		for (const FVector& Endpoint : Endpoints)
		{
			const FSkeletonEdgeLocation Location =
				FindClosestSkeletonEdgeLocation(Endpoint, Result.Edges);
			Result.UncoveredGuideEndpointCount +=
				Location.DistanceSquared > FMath::Square(CoverageDistance)
					? 1
					: 0;
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
			Result.UncoveredWoodSourceTriangleIDs.Add(
				Triangle.SourceTriangleID);
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
			TEXT("%d:%d/%.2f/g%d@%s"),
			ComponentID,
			Diagnostic.TriangleCount,
			Diagnostic.MaximumRatio,
			GuideCountByComponent.FindRef(ComponentID),
			*Diagnostic.WorstPosition.ToCompactString()));
	}

	Result.bSucceeded = !Result.Nodes.IsEmpty() && !Result.Edges.IsEmpty();
	Result.Report = FString::Printf(
		TEXT("hybrid skeleton: %d occupied cell(s), %d volume component(s), %d connected component(s), %d connected interior cell(s), %d recovered local guide(s), %d unresolved local guide(s) [%s], %d uncovered local guide terminal(s), %d uncovered local guide endpoint(s), %d uncovered wood triangle(s) [%s], %.2f maximum wood coverage ratio, %d node(s), %d edge(s), %d branch group(s), %.3f cm global cell size"),
		Result.OccupiedVoxelCount,
		Result.OccupiedComponentCount,
		Result.ConnectedComponentCount,
		Result.ConnectedInteriorVoxelCount,
		Result.RecoveredGuideCount,
		Result.UnresolvedGuideCount,
		*FString::Join(Result.UnresolvedGuideDiagnostics, TEXT(",")),
		Result.UncoveredGuideTerminalCount,
		Result.UncoveredGuideEndpointCount,
		Result.UncoveredWoodTriangleCount,
		*FString::Join(CoverageDiagnostics, TEXT(",")),
		Result.MaximumWoodCoverageRatio,
		Result.Nodes.Num(),
		Result.Edges.Num(),
		BranchCount,
		Result.CellSize);
	return Result;
}
