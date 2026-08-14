#include "FoliageBakerTreeHierarchyColorBaker.h"
#include "FoliageBakerTreeSkeleton.h"

#include "Algo/Reverse.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoliageBakerTreeHierarchy, Log, All);

namespace
{
	constexpr double MinimumGeometryScale = UE_DOUBLE_SMALL_NUMBER;
	constexpr double ComponentContactScale = 2.5;
	constexpr double MaximumContactTreeFraction = 0.025;
	constexpr double DirectionChangeWeight = 0.5;
	constexpr double BranchDistanceComparisonTolerance = 1.0e-9;
	constexpr double BranchPreviewRadiusTreeFraction = 0.002;
	constexpr double MinimumBranchPreviewRadius = 0.5;
	constexpr double TrunkPreviewRadiusScale = 2.5;
	constexpr double JointPreviewRadiusScale = 1.5;
	constexpr double PreviewSliceLengthScale = 0.75;
	constexpr int32 MaximumPreviewSliceCount = 128;
	constexpr double PreviewCenterlineSimplificationScale = 0.75;
	constexpr double PreviewCenterlineSimplificationLengthFraction = 0.001;
	constexpr int32 PreviewCenterlineSmoothingPassCount = 3;
	constexpr double PreviewJointMinimumDirectionDot = 0.984807753012208;
	constexpr double LocalTrunkDiameterSupportLengthFraction = 0.02;
	constexpr double LocalTrunkDiameterGeometryScale = 4.0;
	constexpr double RootBandBaseDiameterScale = 2.0;
	constexpr double MinimumRootBandLengthFraction = 0.02;
	constexpr double MaximumRootBandLengthFraction = 0.08;
	constexpr double RootMaximumRisePersistenceFraction = 0.25;

	struct FTreeTriangle
	{
		FTriangleID TriangleID;
		int32 MaterialIndex = INDEX_NONE;
		TStaticArray<FVertexID, 3> VertexIDs;
	};

	struct FTriangleComponent
	{
		TArray<int32> TriangleIndices;
	};

	struct FWoodComponent
	{
		TArray<int32> TriangleIndices;
		TArray<FVector> Positions;
		TStaticArray<FVector, 2> Endpoints;
		FBox Bounds = FBox(EForceInit::ForceInit);
		FVector PrincipalAxis = FVector::UpVector;
		double GeometryScale = 0.0;
		int32 MinimumTriangleID = MAX_int32;
	};

	struct FWoodContact
	{
		int32 NeighborIndex = INDEX_NONE;
		double InheritanceCost = 0.0;
	};

	struct FClosestCenterlineLocation
	{
		FVector Position = FVector::ZeroVector;
		double DistanceAlong = 0.0;
		double DistanceSquared = TNumericLimits<double>::Max();
	};

	struct FBranchAssignment
	{
		TArray<int32> BranchIDs;
		int32 BranchCount = 0;
		int32 RootSubtreeCount = 0;
	};

	struct FCenterlineRadialSample
	{
		double DistanceAlong = 0.0;
		double Radius = 0.0;
	};

	struct FVertexEdgeKey
	{
		int32 First = INDEX_NONE;
		int32 Second = INDEX_NONE;

		friend bool operator==(
			const FVertexEdgeKey& Left,
			const FVertexEdgeKey& Right)
		{
			return Left.First == Right.First && Left.Second == Right.Second;
		}

		friend uint32 GetTypeHash(const FVertexEdgeKey& Key)
		{
			return HashCombineFast(
				::GetTypeHash(Key.First),
				::GetTypeHash(Key.Second));
		}
	};

	struct FPositionKey
	{
		uint32 X = 0;
		uint32 Y = 0;
		uint32 Z = 0;

		friend bool operator==(
			const FPositionKey& Left,
			const FPositionKey& Right)
		{
			return Left.X == Right.X
				&& Left.Y == Right.Y
				&& Left.Z == Right.Z;
		}

		friend uint32 GetTypeHash(const FPositionKey& Key)
		{
			return HashCombineFast(
				HashCombineFast(
					::GetTypeHash(Key.X),
					::GetTypeHash(Key.Y)),
				::GetTypeHash(Key.Z));
		}
	};

	struct FWoodCapIsland
	{
		TArray<int32> TriangleIndices;
		int32 OwnerComponentIndex = INDEX_NONE;
	};

	struct FLeafBranchAssignment
	{
		TArray<int32> TriangleIndices;
		TArray<FVector3f> TrianglePositions;
		int32 ParentComponentIndex = INDEX_NONE;
		FBox Bounds = FBox(EForceInit::ForceInit);
	};

	struct FMaterialTopology
	{
		int32 MaterialIndex = INDEX_NONE;
		int32 TriangleCount = 0;
		int32 ComponentCount = 0;
		int32 MedianComponentTriangleCount = 0;
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
			if (TriangleVertexIDs.Num() != 3)
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

	FMaterialTopology MeasureMaterialTopology(
		const int32 MaterialIndex,
		const TArray<FTreeTriangle>& Triangles)
	{
		TArray<int32> SelectedTriangleIndices;
		for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
		{
			if (Triangles[TriangleIndex].MaterialIndex == MaterialIndex)
			{
				SelectedTriangleIndices.Add(TriangleIndex);
			}
		}

		const TArray<FTriangleComponent> Components =
			BuildTriangleComponents(Triangles, SelectedTriangleIndices);
		TArray<int32> ComponentSizes;
		ComponentSizes.Reserve(Components.Num());
		for (const FTriangleComponent& Component : Components)
		{
			ComponentSizes.Add(Component.TriangleIndices.Num());
		}
		ComponentSizes.Sort();

		FMaterialTopology Topology;
		Topology.MaterialIndex = MaterialIndex;
		Topology.TriangleCount = SelectedTriangleIndices.Num();
		Topology.ComponentCount = Components.Num();
		Topology.MedianComponentTriangleCount = ComponentSizes.IsEmpty()
			? 0
			: ComponentSizes[ComponentSizes.Num() / 2];
		return Topology;
	}

	bool IsMoreCardLike(
		const FMaterialTopology& Candidate,
		const FMaterialTopology& Current)
	{
		const int64 CandidateDensity =
			static_cast<int64>(Candidate.ComponentCount) * Current.TriangleCount;
		const int64 CurrentDensity =
			static_cast<int64>(Current.ComponentCount) * Candidate.TriangleCount;
		if (CandidateDensity != CurrentDensity)
		{
			return CandidateDensity > CurrentDensity;
		}
		if (Candidate.MedianComponentTriangleCount !=
			Current.MedianComponentTriangleCount)
		{
			return Candidate.MedianComponentTriangleCount
				< Current.MedianComponentTriangleCount;
		}
		return Candidate.MaterialIndex < Current.MaterialIndex;
	}

	bool FindLeafMaterialIndex(
		const TArray<FTreeTriangle>& Triangles,
		int32& OutLeafMaterialIndex,
		FString& OutError)
	{
		TSet<int32> ReferencedMaterials;
		for (const FTreeTriangle& Triangle : Triangles)
		{
			ReferencedMaterials.Add(Triangle.MaterialIndex);
		}
		if (ReferencedMaterials.Num() < 2)
		{
			OutError = TEXT("Tree hierarchy recognition requires at least two referenced materials so card foliage can be separated from wood by topology.");
			return false;
		}

		TArray<int32> SortedMaterials = ReferencedMaterials.Array();
		SortedMaterials.Sort();
		FMaterialTopology BestTopology =
			MeasureMaterialTopology(SortedMaterials[0], Triangles);
		for (int32 MaterialListIndex = 1;
			MaterialListIndex < SortedMaterials.Num();
			++MaterialListIndex)
		{
			const FMaterialTopology Candidate =
				MeasureMaterialTopology(SortedMaterials[MaterialListIndex], Triangles);
			if (IsMoreCardLike(Candidate, BestTopology))
			{
				BestTopology = Candidate;
			}
		}
		OutLeafMaterialIndex = BestTopology.MaterialIndex;
		return true;
	}

	double Median(TArray<double>& Values)
	{
		if (Values.IsEmpty())
		{
			return 0.0;
		}
		Values.Sort();
		return Values[Values.Num() / 2];
	}

	double FindClosestPositionDistanceSquared(
		const FVector& Target,
		const TArray<FVector>& Positions);

	FVector FindPrincipalAxis(
		const TArray<FVector>& Positions,
		const FBox& Bounds)
	{
		const FVector Extent = Bounds.GetExtent();
		FVector Axis = FVector::ForwardVector;
		if (Extent.Y >= Extent.X && Extent.Y >= Extent.Z)
		{
			Axis = FVector::RightVector;
		}
		else if (Extent.Z >= Extent.X && Extent.Z >= Extent.Y)
		{
			Axis = FVector::UpVector;
		}

		const FVector Center = Bounds.GetCenter();
		for (int32 Iteration = 0; Iteration < 8; ++Iteration)
		{
			FVector NextAxis = FVector::ZeroVector;
			for (const FVector& Position : Positions)
			{
				const FVector Delta = Position - Center;
				NextAxis += Delta * FVector::DotProduct(Delta, Axis);
			}
			if (!NextAxis.Normalize())
			{
				break;
			}
			Axis = NextAxis;
		}
		return Axis;
	}

	FWoodComponent BuildWoodComponent(
		const FTriangleComponent& Component,
		const TArray<FTreeTriangle>& Triangles,
		const TVertexAttributesConstRef<FVector3f>& VertexPositions)
	{
		FWoodComponent Result;
		Result.TriangleIndices = Component.TriangleIndices;
		TSet<FVertexID> UniqueVertexIDs;
		TArray<double> EdgeLengths;
		EdgeLengths.Reserve(Component.TriangleIndices.Num() * 3);
		for (const int32 TriangleIndex : Component.TriangleIndices)
		{
			const FTreeTriangle& Triangle = Triangles[TriangleIndex];
			Result.MinimumTriangleID = FMath::Min(
				Result.MinimumTriangleID,
				Triangle.TriangleID.GetValue());
			for (const FVertexID VertexID : Triangle.VertexIDs)
			{
				UniqueVertexIDs.Add(VertexID);
			}
			for (int32 EdgeIndex = 0; EdgeIndex < 3; ++EdgeIndex)
			{
				const FVector FirstPosition(
					VertexPositions[Triangle.VertexIDs[EdgeIndex]]);
				const FVector SecondPosition(
					VertexPositions[Triangle.VertexIDs[(EdgeIndex + 1) % 3]]);
				const double EdgeLength = FVector::Distance(
					FirstPosition,
					SecondPosition);
				if (EdgeLength > MinimumGeometryScale)
				{
					EdgeLengths.Add(EdgeLength);
				}
			}
		}

		TArray<FVertexID> SortedVertexIDs = UniqueVertexIDs.Array();
		SortedVertexIDs.Sort(
			[](const FVertexID First, const FVertexID Second)
			{
				return First.GetValue() < Second.GetValue();
			});
		Result.Positions.Reserve(SortedVertexIDs.Num());
		for (const FVertexID VertexID : SortedVertexIDs)
		{
			const FVector Position(VertexPositions[VertexID]);
			Result.Positions.Add(Position);
			Result.Bounds += Position;
		}
		Result.GeometryScale = Median(EdgeLengths);

		Result.PrincipalAxis = FindPrincipalAxis(
			Result.Positions,
			Result.Bounds);
		double MinimumProjection = TNumericLimits<double>::Max();
		double MaximumProjection = TNumericLimits<double>::Lowest();
		for (const FVector& Position : Result.Positions)
		{
			const double Projection = FVector::DotProduct(
				Position,
				Result.PrincipalAxis);
			if (Projection < MinimumProjection)
			{
				MinimumProjection = Projection;
				Result.Endpoints[0] = Position;
			}
			if (Projection > MaximumProjection)
			{
				MaximumProjection = Projection;
				Result.Endpoints[1] = Position;
			}
		}
		return Result;
	}

	double BoxDistanceSquared(const FBox& First, const FBox& Second)
	{
		double DistanceSquared = 0.0;
		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			double AxisDistance = 0.0;
			if (First.Max[AxisIndex] < Second.Min[AxisIndex])
			{
				AxisDistance = Second.Min[AxisIndex] - First.Max[AxisIndex];
			}
			else if (Second.Max[AxisIndex] < First.Min[AxisIndex])
			{
				AxisDistance = First.Min[AxisIndex] - Second.Max[AxisIndex];
			}
			DistanceSquared += AxisDistance * AxisDistance;
		}
		return DistanceSquared;
	}

	double EndpointSurfaceDistanceSquared(
		const FWoodComponent& First,
		const FWoodComponent& Second)
	{
		double MinimumDistanceSquared = TNumericLimits<double>::Max();
		for (const FVector& Endpoint : First.Endpoints)
		{
			for (const FVector& Position : Second.Positions)
			{
				MinimumDistanceSquared = FMath::Min(
					MinimumDistanceSquared,
					FVector::DistSquared(Endpoint, Position));
			}
		}
		for (const FVector& Endpoint : Second.Endpoints)
		{
			for (const FVector& Position : First.Positions)
			{
				MinimumDistanceSquared = FMath::Min(
					MinimumDistanceSquared,
					FVector::DistSquared(Endpoint, Position));
			}
		}
		return MinimumDistanceSquared;
	}

	TArray<TArray<FWoodContact>> BuildWoodContactGraph(
		const TArray<FWoodComponent>& Components,
		const double TreeDiagonal)
	{
		TArray<TArray<FWoodContact>> Adjacency;
		Adjacency.SetNum(Components.Num());
		for (int32 FirstIndex = 0; FirstIndex < Components.Num(); ++FirstIndex)
		{
			for (int32 SecondIndex = FirstIndex + 1;
				SecondIndex < Components.Num();
				++SecondIndex)
			{
				const FWoodComponent& First = Components[FirstIndex];
				const FWoodComponent& Second = Components[SecondIndex];
				if (First.GeometryScale <= MinimumGeometryScale
					|| Second.GeometryScale <= MinimumGeometryScale)
				{
					continue;
				}
				const double ScaleThreshold = ComponentContactScale
					* FMath::Sqrt(First.GeometryScale * Second.GeometryScale);
				const double ContactThreshold = FMath::Min(
					ScaleThreshold,
					TreeDiagonal * MaximumContactTreeFraction);
				const double ContactThresholdSquared =
					ContactThreshold * ContactThreshold;
				const double EndpointDistanceSquared =
					EndpointSurfaceDistanceSquared(First, Second);
				if (BoxDistanceSquared(First.Bounds, Second.Bounds)
						> ContactThresholdSquared
					|| EndpointDistanceSquared > ContactThresholdSquared)
				{
					continue;
				}
				const double SharedGeometryScale = FMath::Sqrt(
					First.GeometryScale * Second.GeometryScale);
				const double NormalizedDistance =
					FMath::Sqrt(EndpointDistanceSquared) / SharedGeometryScale;
				const double AxisAlignment = FMath::Abs(FVector::DotProduct(
					First.PrincipalAxis,
					Second.PrincipalAxis));
				const double InheritanceCost = NormalizedDistance
					+ (1.0 - AxisAlignment) * DirectionChangeWeight
					+ UE_DOUBLE_SMALL_NUMBER;
				Adjacency[FirstIndex].Add(FWoodContact{
					SecondIndex,
					InheritanceCost});
				Adjacency[SecondIndex].Add(FWoodContact{
					FirstIndex,
					InheritanceCost});
			}
		}
		for (TArray<FWoodContact>& Contacts : Adjacency)
		{
			Contacts.Sort(
				[](const FWoodContact& First, const FWoodContact& Second)
				{
					return First.NeighborIndex < Second.NeighborIndex;
				});
		}
		return Adjacency;
	}

	int32 FindTrunkComponent(const TArray<FWoodComponent>& Components)
	{
		check(!Components.IsEmpty());
		double TreeMinimumZ = TNumericLimits<double>::Max();
		double TreeMaximumZ = TNumericLimits<double>::Lowest();
		for (const FWoodComponent& Component : Components)
		{
			TreeMinimumZ = FMath::Min(TreeMinimumZ, Component.Bounds.Min.Z);
			TreeMaximumZ = FMath::Max(TreeMaximumZ, Component.Bounds.Max.Z);
		}
		const double RootBandMaximumZ =
			TreeMinimumZ + (TreeMaximumZ - TreeMinimumZ) * 0.05;

		int32 TrunkIndex = INDEX_NONE;
		for (int32 ComponentIndex = 0;
			ComponentIndex < Components.Num();
			++ComponentIndex)
		{
			const FWoodComponent& Candidate = Components[ComponentIndex];
			if (Candidate.Bounds.Min.Z > RootBandMaximumZ)
			{
				continue;
			}
			if (TrunkIndex == INDEX_NONE)
			{
				TrunkIndex = ComponentIndex;
				continue;
			}
			const FWoodComponent& Current = Components[TrunkIndex];
			const double CandidateHeight = Candidate.Bounds.GetSize().Z;
			const double CurrentHeight = Current.Bounds.GetSize().Z;
			if (CandidateHeight > CurrentHeight
				|| (FMath::IsNearlyEqual(CandidateHeight, CurrentHeight)
					&& Candidate.TriangleIndices.Num() > Current.TriangleIndices.Num())
				|| (FMath::IsNearlyEqual(CandidateHeight, CurrentHeight)
					&& Candidate.TriangleIndices.Num() == Current.TriangleIndices.Num()
					&& Candidate.MinimumTriangleID < Current.MinimumTriangleID))
			{
				TrunkIndex = ComponentIndex;
			}
		}
		check(TrunkIndex != INDEX_NONE);
		return TrunkIndex;
	}

	void PropagateWoodParentTree(
		const TArray<TArray<FWoodContact>>& Adjacency,
		const TArray<bool>& LockedParentRoots,
		TArray<bool>& ProcessedComponents,
		TArray<double>& Distances,
		TArray<int32>& ParentComponentIndices)
	{
		while (true)
		{
			int32 CurrentIndex = INDEX_NONE;
			for (int32 ComponentIndex = 0;
				ComponentIndex < Adjacency.Num();
				++ComponentIndex)
			{
				if (ProcessedComponents[ComponentIndex]
					|| Distances[ComponentIndex]
						== TNumericLimits<double>::Max())
				{
					continue;
				}
				if (CurrentIndex == INDEX_NONE
					|| Distances[ComponentIndex] < Distances[CurrentIndex]
					|| (FMath::IsNearlyEqual(
							Distances[ComponentIndex],
							Distances[CurrentIndex],
							BranchDistanceComparisonTolerance)
						&& ComponentIndex < CurrentIndex))
				{
					CurrentIndex = ComponentIndex;
				}
			}
			if (CurrentIndex == INDEX_NONE)
			{
				break;
			}

			ProcessedComponents[CurrentIndex] = true;
			for (const FWoodContact& Contact : Adjacency[CurrentIndex])
			{
				const int32 NeighborIndex = Contact.NeighborIndex;
				if (ProcessedComponents[NeighborIndex]
					|| LockedParentRoots[NeighborIndex])
				{
					continue;
				}

				const double CandidateDistance = Distances[CurrentIndex]
					+ Contact.InheritanceCost;
				const bool bHasNoParent =
					ParentComponentIndices[NeighborIndex] == INDEX_NONE;
				const bool bHasShorterPath = CandidateDistance
					+ BranchDistanceComparisonTolerance
					< Distances[NeighborIndex];
				const bool bWinsEqualDistanceTie = FMath::IsNearlyEqual(
						CandidateDistance,
						Distances[NeighborIndex],
						BranchDistanceComparisonTolerance)
					&& CurrentIndex
						< ParentComponentIndices[NeighborIndex];
				if (bHasNoParent || bHasShorterPath || bWinsEqualDistanceTie)
				{
					Distances[NeighborIndex] = CandidateDistance;
					ParentComponentIndices[NeighborIndex] = CurrentIndex;
				}
			}
		}
	}

	TArray<int32> BuildWoodParentTree(
		const TArray<FWoodComponent>& Components,
		const TArray<TArray<FWoodContact>>& Adjacency,
		const int32 TrunkComponentIndex)
	{
		check(Adjacency.IsValidIndex(TrunkComponentIndex));
		TArray<int32> ParentComponentIndices;
		ParentComponentIndices.Init(INDEX_NONE, Adjacency.Num());
		TArray<double> Distances;
		Distances.Init(TNumericLimits<double>::Max(), Adjacency.Num());
		TArray<bool> LockedParentRoots;
		LockedParentRoots.Init(false, Adjacency.Num());
		TArray<bool> ProcessedComponents;
		ProcessedComponents.Init(false, Adjacency.Num());
		ProcessedComponents[TrunkComponentIndex] = true;

		for (const FWoodContact& Contact : Adjacency[TrunkComponentIndex])
		{
			LockedParentRoots[Contact.NeighborIndex] = true;
			Distances[Contact.NeighborIndex] = Contact.InheritanceCost;
			ParentComponentIndices[Contact.NeighborIndex] = TrunkComponentIndex;
		}
		PropagateWoodParentTree(
			Adjacency,
			LockedParentRoots,
			ProcessedComponents,
			Distances,
			ParentComponentIndices);

		// The strict contact graph also has a whole-tree distance cap. Repair
		// components that still fit the existing local geometry threshold;
		// otherwise seed an independent branch root and keep its chain intact.
		while (true)
		{
			int32 FirstUnprocessedIndex = INDEX_NONE;
			int32 RepairRootIndex = INDEX_NONE;
			int32 RepairParentIndex = INDEX_NONE;
			double BestNormalizedDistance = TNumericLimits<double>::Max();
			for (int32 ComponentIndex = 0;
				ComponentIndex < Components.Num();
				++ComponentIndex)
			{
				if (ProcessedComponents[ComponentIndex])
				{
					continue;
				}
				if (FirstUnprocessedIndex == INDEX_NONE)
				{
					FirstUnprocessedIndex = ComponentIndex;
				}
				for (int32 ParentIndex = 0;
					ParentIndex < Components.Num();
					++ParentIndex)
				{
					if (!ProcessedComponents[ParentIndex])
					{
						continue;
					}
					const double SharedGeometryScale = FMath::Sqrt(
						Components[ComponentIndex].GeometryScale
							* Components[ParentIndex].GeometryScale);
					if (SharedGeometryScale <= MinimumGeometryScale)
					{
						continue;
					}
					const double NormalizedDistance = FMath::Sqrt(
						EndpointSurfaceDistanceSquared(
							Components[ComponentIndex],
							Components[ParentIndex])) / SharedGeometryScale;
					const bool bIsCloser = NormalizedDistance
						+ BranchDistanceComparisonTolerance
						< BestNormalizedDistance;
					const bool bWinsEqualDistanceTie = FMath::IsNearlyEqual(
							NormalizedDistance,
							BestNormalizedDistance,
							BranchDistanceComparisonTolerance)
						&& (ComponentIndex < RepairRootIndex
							|| (ComponentIndex == RepairRootIndex
								&& ParentIndex < RepairParentIndex));
					if (RepairRootIndex == INDEX_NONE
						|| bIsCloser
						|| bWinsEqualDistanceTie)
					{
						RepairRootIndex = ComponentIndex;
						RepairParentIndex = ParentIndex;
						BestNormalizedDistance = NormalizedDistance;
					}
				}
			}
			if (FirstUnprocessedIndex == INDEX_NONE)
			{
				break;
			}

			const bool bHasReliableRepairParent =
				RepairRootIndex != INDEX_NONE
				&& BestNormalizedDistance <= ComponentContactScale;
			const int32 NewRootIndex = bHasReliableRepairParent
				? RepairRootIndex
				: FirstUnprocessedIndex;
			if (bHasReliableRepairParent)
			{
				ParentComponentIndices[NewRootIndex] = RepairParentIndex;
			}
			LockedParentRoots[NewRootIndex] = true;
			Distances[NewRootIndex] = 0.0;
			PropagateWoodParentTree(
				Adjacency,
				LockedParentRoots,
				ProcessedComponents,
				Distances,
				ParentComponentIndices);
		}
		return ParentComponentIndices;
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

	double FindClosestPositionDistanceSquared(
		const FVector& Target,
		const TArray<FVector>& Positions)
	{
		check(!Positions.IsEmpty());
		double ClosestDistanceSquared = TNumericLimits<double>::Max();
		for (const FVector& Position : Positions)
		{
			const double DistanceSquared = FVector::DistSquared(
				Target,
				Position);
			if (DistanceSquared < ClosestDistanceSquared)
			{
				ClosestDistanceSquared = DistanceSquared;
			}
		}
		return ClosestDistanceSquared;
	}

	FVector FindAveragePosition(const TArray<FVector>& Positions)
	{
		check(!Positions.IsEmpty());
		FVector Average = FVector::ZeroVector;
		for (const FVector& Position : Positions)
		{
			Average += Position;
		}
		return Average / static_cast<double>(Positions.Num());
	}

	FVector FindPreviewPrincipalAxis(
		const FWoodComponent& Component,
		const FVector& Center)
	{
		FVector Axis = Component.PrincipalAxis;
		for (int32 Iteration = 0; Iteration < 8; ++Iteration)
		{
			FVector NextAxis = FVector::ZeroVector;
			for (const FVector& Position : Component.Positions)
			{
				const FVector Delta = Position - Center;
				NextAxis += Delta * FVector::DotProduct(Delta, Axis);
			}
			if (!NextAxis.Normalize())
			{
				break;
			}
			Axis = NextAxis;
		}
		return Axis;
	}

	double FindCoordinateMedian(
		const TArray<FVector>& Positions,
		const int32 AxisIndex)
	{
		check(!Positions.IsEmpty());
		TArray<double> Coordinates;
		Coordinates.Reserve(Positions.Num());
		for (const FVector& Position : Positions)
		{
			Coordinates.Add(Position[AxisIndex]);
		}
		Coordinates.Sort();
		return Coordinates[Coordinates.Num() / 2];
	}

	FVector FindMedianPosition(const TArray<FVector>& Positions)
	{
		return FVector(
			FindCoordinateMedian(Positions, 0),
			FindCoordinateMedian(Positions, 1),
			FindCoordinateMedian(Positions, 2));
	}

	double FindMedianProjection(
		const TArray<FVector>& Positions,
		const FVector& Center,
		const FVector& Axis)
	{
		check(!Positions.IsEmpty());
		TArray<double> Projections;
		Projections.Reserve(Positions.Num());
		for (const FVector& Position : Positions)
		{
			Projections.Add(FVector::DotProduct(Position - Center, Axis));
		}
		Projections.Sort();
		return Projections[Projections.Num() / 2];
	}

	void MarkCenterlinePointsToKeep(
		const TArray<FVector>& Points,
		const int32 FirstIndex,
		const int32 LastIndex,
		const double ToleranceSquared,
		TArray<bool>& KeepPoints)
	{
		check(FirstIndex >= 0);
		check(LastIndex < Points.Num());
		check(FirstIndex < LastIndex);
		if (LastIndex == FirstIndex + 1)
		{
			return;
		}

		int32 FurthestIndex = INDEX_NONE;
		double FurthestDistanceSquared = 0.0;
		for (int32 PointIndex = FirstIndex + 1;
			PointIndex < LastIndex;
			++PointIndex)
		{
			const FVector ClosestPoint = FMath::ClosestPointOnSegment(
				Points[PointIndex],
				Points[FirstIndex],
				Points[LastIndex]);
			const double DistanceSquared = FVector::DistSquared(
				Points[PointIndex],
				ClosestPoint);
			if (DistanceSquared > FurthestDistanceSquared)
			{
				FurthestDistanceSquared = DistanceSquared;
				FurthestIndex = PointIndex;
			}
		}
		if (FurthestIndex == INDEX_NONE
			|| FurthestDistanceSquared <= ToleranceSquared)
		{
			return;
		}

		KeepPoints[FurthestIndex] = true;
		MarkCenterlinePointsToKeep(
			Points,
			FirstIndex,
			FurthestIndex,
			ToleranceSquared,
			KeepPoints);
		MarkCenterlinePointsToKeep(
			Points,
			FurthestIndex,
			LastIndex,
			ToleranceSquared,
			KeepPoints);
	}

	TArray<FVector> SimplifyCenterline(
		const TArray<FVector>& Points,
		const double Tolerance)
	{
		check(Points.Num() >= 2);
		TArray<bool> KeepPoints;
		KeepPoints.Init(false, Points.Num());
		KeepPoints[0] = true;
		KeepPoints.Last() = true;
		MarkCenterlinePointsToKeep(
			Points,
			0,
			Points.Num() - 1,
			Tolerance * Tolerance,
			KeepPoints);

		TArray<FVector> Result;
		for (int32 PointIndex = 0; PointIndex < Points.Num(); ++PointIndex)
		{
			if (KeepPoints[PointIndex])
			{
				Result.Add(Points[PointIndex]);
			}
		}
		return Result;
	}

	TArray<FVector> SmoothCenterline(
		const TArray<FVector>& Centerline,
		const int32 PassCount)
	{
		check(PassCount > 0);
		if (Centerline.Num() <= 2)
		{
			return Centerline;
		}
		TArray<FVector> Result = Centerline;
		for (int32 PassIndex = 0; PassIndex < PassCount; ++PassIndex)
		{
			const TArray<FVector> Previous = Result;
			for (int32 PointIndex = 1;
				PointIndex < Previous.Num() - 1;
				++PointIndex)
			{
				Result[PointIndex] =
					Previous[PointIndex - 1] * 0.2
					+ Previous[PointIndex] * 0.6
					+ Previous[PointIndex + 1] * 0.2;
			}
		}
		return Result;
	}

	TArray<FVector> BuildPreviewCenterline(
		const FWoodComponent& Component,
		const bool bIsTrunk)
	{
		check(Component.Positions.Num() >= 2);
		const FVector Center = FindAveragePosition(Component.Positions);
		const FVector Axis = bIsTrunk
			? FVector::UpVector
			: FindPreviewPrincipalAxis(Component, Center);
		double MinimumProjection = TNumericLimits<double>::Max();
		double MaximumProjection = TNumericLimits<double>::Lowest();
		for (const FVector& Position : Component.Positions)
		{
			const double Projection = FVector::DotProduct(Position - Center, Axis);
			MinimumProjection = FMath::Min(MinimumProjection, Projection);
			MaximumProjection = FMath::Max(MaximumProjection, Projection);
		}

		TArray<FVector> PrincipalLine;
		PrincipalLine.Add(Center + Axis * MinimumProjection);
		PrincipalLine.Add(Center + Axis * MaximumProjection);
		const double AxialLength = MaximumProjection - MinimumProjection;
		if (AxialLength <= MinimumGeometryScale)
		{
			return PrincipalLine;
		}

		const double TargetSliceLength = FMath::Max(
			Component.GeometryScale * PreviewSliceLengthScale,
			MinimumGeometryScale);
		const int32 SliceCount = FMath::Clamp(
			FMath::CeilToInt(AxialLength / TargetSliceLength),
			2,
			MaximumPreviewSliceCount);
		TArray<TArray<FVector>> SlicePositions;
		SlicePositions.SetNum(SliceCount);
		for (const FVector& Position : Component.Positions)
		{
			const double Projection = FVector::DotProduct(Position - Center, Axis);
			const double NormalizedPosition =
				(Projection - MinimumProjection) / AxialLength;
			const int32 SliceIndex = FMath::Clamp(
				FMath::FloorToInt(NormalizedPosition * SliceCount),
				0,
				SliceCount - 1);
			SlicePositions[SliceIndex].Add(Position);
		}

		TArray<FVector> SlicedCenterline;
		for (const TArray<FVector>& Positions : SlicePositions)
		{
			if (!Positions.IsEmpty())
			{
				FVector SliceCenter = FindMedianPosition(Positions);
				const double SliceProjection = FindMedianProjection(
					Positions,
					Center,
					Axis);
				SliceCenter += Axis * (
					SliceProjection
					- FVector::DotProduct(SliceCenter - Center, Axis));
				SlicedCenterline.Add(SliceCenter);
			}
		}
		if (SlicedCenterline.Num() < 2)
		{
			return PrincipalLine;
		}

		SlicedCenterline[0] += Axis * (
			MinimumProjection
			- FVector::DotProduct(SlicedCenterline[0] - Center, Axis));
		SlicedCenterline.Last() += Axis * (
			MaximumProjection
			- FVector::DotProduct(SlicedCenterline.Last() - Center, Axis));
		SlicedCenterline = SmoothCenterline(
			SlicedCenterline,
			PreviewCenterlineSmoothingPassCount);

		const double SimplificationTolerance = FMath::Max(
			Component.GeometryScale * PreviewCenterlineSimplificationScale,
			AxialLength * PreviewCenterlineSimplificationLengthFraction);
		return SimplifyCenterline(
			SlicedCenterline,
			SimplificationTolerance);
	}

	void AnchorTrunkCenterlineToPivot(TArray<FVector>& Centerline)
	{
		check(Centerline.Num() >= 2);
		TArray<FVector> AnchoredCenterline;
		AnchoredCenterline.Reserve(Centerline.Num() + 1);
		AnchoredCenterline.Add(FVector::ZeroVector);
		for (const FVector& Point : Centerline)
		{
			if (Point.Z <= MinimumGeometryScale)
			{
				continue;
			}
			AnchoredCenterline.Add(Point);
		}
		if (AnchoredCenterline.Num() == 1)
		{
			AnchoredCenterline.Add(Centerline.Last());
		}
		Centerline = MoveTemp(AnchoredCenterline);
		check(Centerline[0] == FVector::ZeroVector);
	}

	double MeasureCenterlineLength(const TArray<FVector>& Centerline)
	{
		check(Centerline.Num() >= 2);
		double Length = 0.0;
		for (int32 PointIndex = 1; PointIndex < Centerline.Num(); ++PointIndex)
		{
			Length += FVector::Distance(
				Centerline[PointIndex - 1],
				Centerline[PointIndex]);
		}
		return Length;
	}

	FClosestCenterlineLocation FindClosestCenterlineLocation(
		const FVector& Position,
		const TArray<FVector>& Centerline)
	{
		check(Centerline.Num() >= 2);
		FClosestCenterlineLocation Result;
		double TraversedLength = 0.0;
		for (int32 PointIndex = 1; PointIndex < Centerline.Num(); ++PointIndex)
		{
			const double SegmentLength = FVector::Distance(
				Centerline[PointIndex - 1],
				Centerline[PointIndex]);
			const FVector Candidate = FMath::ClosestPointOnSegment(
				Position,
				Centerline[PointIndex - 1],
				Centerline[PointIndex]);
			const double DistanceSquared = FVector::DistSquared(
				Position,
				Candidate);
			if (DistanceSquared < Result.DistanceSquared)
			{
				const double SegmentFraction = SegmentLength
					> MinimumGeometryScale
					? FVector::Distance(
							Centerline[PointIndex - 1],
							Candidate) / SegmentLength
					: 0.0;
				Result.Position = Candidate;
				Result.DistanceAlong = TraversedLength
					+ SegmentLength * SegmentFraction;
				Result.DistanceSquared = DistanceSquared;
			}
			TraversedLength += SegmentLength;
		}
		return Result;
	}

	FVector FindClosestPointOnCenterline(
		const FVector& Position,
		const TArray<FVector>& Centerline)
	{
		return FindClosestCenterlineLocation(Position, Centerline).Position;
	}

	FVector FindPointAlongCenterline(
		const TArray<FVector>& Centerline,
		const double Fraction)
	{
		check(Centerline.Num() >= 2);
		double TotalLength = 0.0;
		for (int32 PointIndex = 1; PointIndex < Centerline.Num(); ++PointIndex)
		{
			TotalLength += FVector::Distance(
				Centerline[PointIndex - 1],
				Centerline[PointIndex]);
		}
		const double TargetLength = TotalLength * Fraction;
		double TraversedLength = 0.0;
		for (int32 PointIndex = 1; PointIndex < Centerline.Num(); ++PointIndex)
		{
			const double SegmentLength = FVector::Distance(
				Centerline[PointIndex - 1],
				Centerline[PointIndex]);
			if (TraversedLength + SegmentLength >= TargetLength)
			{
				const double SegmentFraction = SegmentLength > MinimumGeometryScale
					? (TargetLength - TraversedLength) / SegmentLength
					: 0.0;
				return FMath::Lerp(
					Centerline[PointIndex - 1],
					Centerline[PointIndex],
					SegmentFraction);
			}
			TraversedLength += SegmentLength;
		}
		return Centerline.Last();
	}

	void RetargetCenterlineStart(
		TArray<FVector>& Centerline,
		const FVector& TargetStart)
	{
		check(Centerline.Num() >= 2);
		const FVector AttachmentOffset = TargetStart - Centerline[0];
		if (AttachmentOffset.IsNearlyZero())
		{
			Centerline[0] = TargetStart;
			return;
		}

		double TotalLength = 0.0;
		for (int32 PointIndex = 1; PointIndex < Centerline.Num(); ++PointIndex)
		{
			TotalLength += FVector::Distance(
				Centerline[PointIndex - 1],
				Centerline[PointIndex]);
		}
		if (TotalLength <= MinimumGeometryScale)
		{
			Centerline[0] = TargetStart;
			return;
		}

		if (Centerline.Num() == 2
			&& AttachmentOffset.Size() > TotalLength * 0.1)
		{
			const FVector Start = Centerline[0];
			const FVector End = Centerline[1];
			Centerline.Insert(FMath::Lerp(Start, End, 1.0 / 3.0), 1);
			Centerline.Insert(FMath::Lerp(Start, End, 2.0 / 3.0), 2);
		}

		TArray<double> PointFractions;
		PointFractions.Init(0.0, Centerline.Num());
		double TraversedLength = 0.0;
		for (int32 PointIndex = 1; PointIndex < Centerline.Num(); ++PointIndex)
		{
			TraversedLength += FVector::Distance(
				Centerline[PointIndex - 1],
				Centerline[PointIndex]);
			PointFractions[PointIndex] = FMath::Clamp(
				TraversedLength / TotalLength,
				0.0,
				1.0);
		}
		for (int32 PointIndex = 0; PointIndex < Centerline.Num(); ++PointIndex)
		{
			const double Fraction = PointFractions[PointIndex];
			const double SmoothFraction =
				Fraction * Fraction * (3.0 - 2.0 * Fraction);
			Centerline[PointIndex] +=
				AttachmentOffset * (1.0 - SmoothFraction);
		}
		Centerline[0] = TargetStart;
	}

	TArray<int32> BuildHierarchyOrder(
		const TArray<int32>& ParentComponentIndices,
		const int32 TrunkComponentIndex)
	{
		check(ParentComponentIndices.IsValidIndex(TrunkComponentIndex));
		TArray<int32> ComponentDepths;
		ComponentDepths.Init(INDEX_NONE, ParentComponentIndices.Num());
		for (int32 ComponentIndex = 0;
			ComponentIndex < ParentComponentIndices.Num();
			++ComponentIndex)
		{
			if (ComponentIndex == TrunkComponentIndex
				|| !ParentComponentIndices.IsValidIndex(
					ParentComponentIndices[ComponentIndex]))
			{
				ComponentDepths[ComponentIndex] = 0;
			}
		}
		for (int32 PassIndex = 0;
			PassIndex < ParentComponentIndices.Num();
			++PassIndex)
		{
			for (int32 ComponentIndex = 0;
				ComponentIndex < ParentComponentIndices.Num();
				++ComponentIndex)
			{
				if (ComponentDepths[ComponentIndex] != INDEX_NONE)
				{
					continue;
				}
				const int32 ParentIndex =
					ParentComponentIndices[ComponentIndex];
				check(ParentComponentIndices.IsValidIndex(ParentIndex));
				if (ComponentDepths[ParentIndex] != INDEX_NONE)
				{
					ComponentDepths[ComponentIndex] =
						ComponentDepths[ParentIndex] + 1;
				}
			}
		}

		TArray<int32> ComponentOrder;
		ComponentOrder.Reserve(ParentComponentIndices.Num());
		for (int32 ComponentIndex = 0;
			ComponentIndex < ParentComponentIndices.Num();
			++ComponentIndex)
		{
			checkf(
				ComponentDepths[ComponentIndex] != INDEX_NONE,
				TEXT("Tree hierarchy parent graph contains a cycle."));
			ComponentOrder.Add(ComponentIndex);
		}
		ComponentOrder.Sort(
			[&ComponentDepths](const int32 FirstIndex, const int32 SecondIndex)
			{
				if (ComponentDepths[FirstIndex]
					!= ComponentDepths[SecondIndex])
				{
					return ComponentDepths[FirstIndex]
						< ComponentDepths[SecondIndex];
				}
				return FirstIndex < SecondIndex;
			});
		return ComponentOrder;
	}

	TArray<TArray<FVector>> BuildComponentCenterlines(
		const TArray<FWoodComponent>& WoodComponents,
		const int32 TrunkComponentIndex,
		const TArray<int32>& ParentComponentIndices)
	{
		check(WoodComponents.IsValidIndex(TrunkComponentIndex));
		check(ParentComponentIndices.Num() == WoodComponents.Num());
		TArray<TArray<FVector>> ComponentCenterlines;
		ComponentCenterlines.Reserve(WoodComponents.Num());
		for (int32 ComponentIndex = 0;
			ComponentIndex < WoodComponents.Num();
			++ComponentIndex)
		{
			ComponentCenterlines.Add(BuildPreviewCenterline(
				WoodComponents[ComponentIndex],
				ComponentIndex == TrunkComponentIndex));
		}

		for (int32 ComponentIndex = 0;
			ComponentIndex < WoodComponents.Num();
			++ComponentIndex)
		{
			TArray<FVector>& Centerline = ComponentCenterlines[ComponentIndex];
			if (ComponentIndex == TrunkComponentIndex)
			{
				if (Centerline[0].Z > Centerline.Last().Z)
				{
					Algo::Reverse(Centerline);
				}
				continue;
			}

			const int32 ParentIndex = ParentComponentIndices[ComponentIndex];
			if (WoodComponents.IsValidIndex(ParentIndex))
			{
				const FWoodComponent& Parent = WoodComponents[ParentIndex];
				const double FirstDistanceSquared =
					FindClosestPositionDistanceSquared(
						Centerline[0],
						Parent.Positions);
				const double SecondDistanceSquared =
					FindClosestPositionDistanceSquared(
						Centerline.Last(),
						Parent.Positions);
				if (SecondDistanceSquared < FirstDistanceSquared)
				{
					Algo::Reverse(Centerline);
				}
			}
			else if (Centerline[0].Z > Centerline.Last().Z)
			{
				Algo::Reverse(Centerline);
			}
		}

		AnchorTrunkCenterlineToPivot(
			ComponentCenterlines[TrunkComponentIndex]);
		check(ComponentCenterlines[TrunkComponentIndex][0]
			== FVector::ZeroVector);

		const TArray<int32> ComponentOrder = BuildHierarchyOrder(
			ParentComponentIndices,
			TrunkComponentIndex);
		for (const int32 ComponentIndex : ComponentOrder)
		{
			if (ComponentIndex == TrunkComponentIndex)
			{
				continue;
			}
			const int32 ParentIndex = ParentComponentIndices[ComponentIndex];
			if (!WoodComponents.IsValidIndex(ParentIndex))
			{
				continue;
			}
			const FVector AttachmentPoint = FindClosestPointOnCenterline(
				ComponentCenterlines[ComponentIndex][0],
				ComponentCenterlines[ParentIndex]);
			RetargetCenterlineStart(
				ComponentCenterlines[ComponentIndex],
				AttachmentPoint);
			check(ComponentCenterlines[ComponentIndex][0].Equals(
				AttachmentPoint,
				MinimumGeometryScale));
		}
		return ComponentCenterlines;
	}

	TArray<FCenterlineRadialSample> BuildCenterlineRadialSamples(
		const FWoodComponent& Component,
		const TArray<FVector>& Centerline)
	{
		TArray<FCenterlineRadialSample> Samples;
		Samples.Reserve(Component.Positions.Num());
		for (const FVector& Position : Component.Positions)
		{
			const FClosestCenterlineLocation Location =
				FindClosestCenterlineLocation(Position, Centerline);
			FCenterlineRadialSample& Sample = Samples.AddDefaulted_GetRef();
			Sample.DistanceAlong = Location.DistanceAlong;
			Sample.Radius = FMath::Sqrt(Location.DistanceSquared);
		}
		return Samples;
	}

	double MeasureLocalCenterlineDiameter(
		const FWoodComponent& Component,
		const TArray<FCenterlineRadialSample>& RadialSamples,
		const double CenterlineLength,
		const double DistanceAlongCenterline)
	{
		check(CenterlineLength > MinimumGeometryScale);
		const double SupportLength = FMath::Max(
			CenterlineLength * LocalTrunkDiameterSupportLengthFraction,
			Component.GeometryScale * LocalTrunkDiameterGeometryScale);
		TArray<double> Radii;
		for (const FCenterlineRadialSample& Sample : RadialSamples)
		{
			if (FMath::Abs(Sample.DistanceAlong - DistanceAlongCenterline)
				<= SupportLength)
			{
				Radii.Add(Sample.Radius);
			}
		}
		const double MeasuredDiameter = Median(Radii) * 2.0;
		return FMath::Max(
			MeasuredDiameter,
			Component.GeometryScale * 2.0);
	}

	FVertexEdgeKey MakeVertexEdgeKey(
		const FVertexID FirstVertexID,
		const FVertexID SecondVertexID)
	{
		const int32 FirstValue = FirstVertexID.GetValue();
		const int32 SecondValue = SecondVertexID.GetValue();
		return FirstValue < SecondValue
			? FVertexEdgeKey{FirstValue, SecondValue}
			: FVertexEdgeKey{SecondValue, FirstValue};
	}

	FPositionKey MakePositionKey(const FVector3f& Position)
	{
		const float X = Position.X == 0.0f ? 0.0f : Position.X;
		const float Y = Position.Y == 0.0f ? 0.0f : Position.Y;
		const float Z = Position.Z == 0.0f ? 0.0f : Position.Z;
		return FPositionKey{
			FMath::AsUInt(X),
			FMath::AsUInt(Y),
			FMath::AsUInt(Z)};
	}

	TArray<FVertexID> BuildBoundaryVertexIDs(
		const FTriangleComponent& Component,
		const TArray<FTreeTriangle>& Triangles)
	{
		TMap<FVertexEdgeKey, int32> EdgeUseCounts;
		for (const int32 TriangleIndex : Component.TriangleIndices)
		{
			if (!Triangles.IsValidIndex(TriangleIndex))
			{
				continue;
			}
			const FTreeTriangle& Triangle = Triangles[TriangleIndex];
			for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex)
			{
				const FVertexEdgeKey Edge = MakeVertexEdgeKey(
					Triangle.VertexIDs[CornerIndex],
					Triangle.VertexIDs[(CornerIndex + 1) % 3]);
				++EdgeUseCounts.FindOrAdd(Edge);
			}
		}

		TSet<FVertexID> BoundaryVertexIDs;
		for (const TPair<FVertexEdgeKey, int32>& EdgeUse : EdgeUseCounts)
		{
			if (EdgeUse.Value == 1)
			{
				BoundaryVertexIDs.Add(FVertexID(EdgeUse.Key.First));
				BoundaryVertexIDs.Add(FVertexID(EdgeUse.Key.Second));
			}
		}
		TArray<FVertexID> Result = BoundaryVertexIDs.Array();
		Result.Sort(
			[](const FVertexID First, const FVertexID Second)
			{
				return First.GetValue() < Second.GetValue();
			});
		return Result;
	}

	void BuildWoodBoundaryOwners(
		const TArray<FTriangleComponent>& WoodTriangleComponents,
		const TArray<FTreeTriangle>& Triangles,
		const TVertexAttributesConstRef<FVector3f>& VertexPositions,
		TMap<FVertexID, TArray<int32>>& OutOwnersByVertexID,
		TMap<FPositionKey, TArray<int32>>& OutOwnersByPosition)
	{
		for (int32 ComponentIndex = 0;
			ComponentIndex < WoodTriangleComponents.Num();
			++ComponentIndex)
		{
			const TArray<FVertexID> BoundaryVertexIDs = BuildBoundaryVertexIDs(
				WoodTriangleComponents[ComponentIndex],
				Triangles);
			for (const FVertexID VertexID : BoundaryVertexIDs)
			{
				OutOwnersByVertexID.FindOrAdd(VertexID).AddUnique(ComponentIndex);
				OutOwnersByPosition.FindOrAdd(
					MakePositionKey(VertexPositions[VertexID])).AddUnique(
						ComponentIndex);
			}
		}
	}

	int32 FindBoundaryOwnerComponent(
		const FTriangleComponent& Island,
		const TArray<FTreeTriangle>& Triangles,
		const TVertexAttributesConstRef<FVector3f>& VertexPositions,
		const TMap<FVertexID, TArray<int32>>& OwnersByVertexID,
		const TMap<FPositionKey, TArray<int32>>& OwnersByPosition)
	{
		const TArray<FVertexID> BoundaryVertexIDs = BuildBoundaryVertexIDs(
			Island,
			Triangles);
		if (BoundaryVertexIDs.Num() < 3)
		{
			return INDEX_NONE;
		}

		TMap<int32, int32> MatchCounts;
		for (const FVertexID VertexID : BoundaryVertexIDs)
		{
			if (OwnersByVertexID.Contains(VertexID))
			{
				for (const int32 OwnerComponentIndex :
					OwnersByVertexID.FindChecked(VertexID))
				{
					++MatchCounts.FindOrAdd(OwnerComponentIndex);
				}
				continue;
			}

			const FPositionKey PositionKey = MakePositionKey(
				VertexPositions[VertexID]);
			if (!OwnersByPosition.Contains(PositionKey))
			{
				return INDEX_NONE;
			}
			for (const int32 OwnerComponentIndex :
				OwnersByPosition.FindChecked(PositionKey))
			{
				++MatchCounts.FindOrAdd(OwnerComponentIndex);
			}
		}

		int32 Result = INDEX_NONE;
		for (const TPair<int32, int32>& MatchCount : MatchCounts)
		{
			if (MatchCount.Value != BoundaryVertexIDs.Num())
			{
				continue;
			}
			if (Result != INDEX_NONE)
			{
				return INDEX_NONE;
			}
			Result = MatchCount.Key;
		}
		return Result;
	}

	TArray<FWoodCapIsland> FindTopologyOwnedWoodCapIslands(
		const TArray<FTriangleComponent>& LeafTriangleComponents,
		const TArray<FTriangleComponent>& WoodTriangleComponents,
		const TArray<FTreeTriangle>& Triangles,
		const TVertexAttributesConstRef<FVector3f>& VertexPositions)
	{
		TMap<FVertexID, TArray<int32>> OwnersByVertexID;
		TMap<FPositionKey, TArray<int32>> OwnersByPosition;
		BuildWoodBoundaryOwners(
			WoodTriangleComponents,
			Triangles,
			VertexPositions,
			OwnersByVertexID,
			OwnersByPosition);

		TArray<FWoodCapIsland> Result;
		for (const FTriangleComponent& LeafComponent : LeafTriangleComponents)
		{
			const int32 OwnerComponentIndex = FindBoundaryOwnerComponent(
				LeafComponent,
				Triangles,
				VertexPositions,
				OwnersByVertexID,
				OwnersByPosition);
			if (OwnerComponentIndex == INDEX_NONE)
			{
				continue;
			}

			FWoodCapIsland& CapIsland = Result.AddDefaulted_GetRef();
			CapIsland.TriangleIndices = LeafComponent.TriangleIndices;
			CapIsland.OwnerComponentIndex = OwnerComponentIndex;
		}
		return Result;
	}

	TArray<FLeafBranchAssignment> AssignLeafComponentsToWood(
		const TArray<FTriangleComponent>& LeafTriangleComponents,
		const TArray<FWoodCapIsland>& WoodCapIslands,
		const TArray<FWoodComponent>& WoodComponents,
		const TArray<FTreeTriangle>& Triangles,
		const TVertexAttributesConstRef<FVector3f>& VertexPositions)
	{
		TSet<int32> WoodCapTriangleIndices;
		for (const FWoodCapIsland& WoodCapIsland : WoodCapIslands)
		{
			for (const int32 TriangleIndex : WoodCapIsland.TriangleIndices)
			{
				WoodCapTriangleIndices.Add(TriangleIndex);
			}
		}

		TArray<FLeafBranchAssignment> Result;
		for (const FTriangleComponent& LeafComponent : LeafTriangleComponents)
		{
			if (LeafComponent.TriangleIndices.IsEmpty()
				|| WoodCapTriangleIndices.Contains(
					LeafComponent.TriangleIndices[0]))
			{
				continue;
			}

			FLeafBranchAssignment Assignment;
			Assignment.TriangleIndices = LeafComponent.TriangleIndices;
			TSet<FVertexID> UniqueLeafVertexIDs;
			for (const int32 TriangleIndex : LeafComponent.TriangleIndices)
			{
				if (!Triangles.IsValidIndex(TriangleIndex))
				{
					continue;
				}
				for (const FVertexID VertexID : Triangles[TriangleIndex].VertexIDs)
				{
					UniqueLeafVertexIDs.Add(VertexID);
					Assignment.TrianglePositions.Add(
						VertexPositions[VertexID]);
				}
			}

			TArray<FVector> LeafPositions;
			LeafPositions.Reserve(UniqueLeafVertexIDs.Num());
			for (const FVertexID VertexID : UniqueLeafVertexIDs)
			{
				const FVector Position(VertexPositions[VertexID]);
				LeafPositions.Add(Position);
				Assignment.Bounds += Position;
			}

			double ClosestDistanceSquared = TNumericLimits<double>::Max();
			for (int32 ComponentIndex = 0;
				ComponentIndex < WoodComponents.Num();
				++ComponentIndex)
			{
				const FWoodComponent& WoodComponent =
					WoodComponents[ComponentIndex];
				if (BoxDistanceSquared(
						Assignment.Bounds,
						WoodComponent.Bounds)
					> ClosestDistanceSquared)
				{
					continue;
				}

				for (const FVector& LeafPosition : LeafPositions)
				{
					for (const int32 WoodTriangleIndex :
						WoodComponent.TriangleIndices)
					{
						const FTreeTriangle& WoodTriangle =
							Triangles[WoodTriangleIndex];
						const FVector ClosestWoodPosition =
							FMath::ClosestPointOnTriangleToPoint(
								LeafPosition,
								FVector(VertexPositions[
									WoodTriangle.VertexIDs[0]]),
								FVector(VertexPositions[
									WoodTriangle.VertexIDs[1]]),
								FVector(VertexPositions[
									WoodTriangle.VertexIDs[2]]));
						const double DistanceSquared = FVector::DistSquared(
							LeafPosition,
							ClosestWoodPosition);
						const bool bCloser = DistanceSquared
							< ClosestDistanceSquared
							- BranchDistanceComparisonTolerance;
						const bool bStableTie = FMath::IsNearlyEqual(
							DistanceSquared,
							ClosestDistanceSquared,
							BranchDistanceComparisonTolerance)
							&& (Assignment.ParentComponentIndex == INDEX_NONE
								|| ComponentIndex
									< Assignment.ParentComponentIndex);
						if (bCloser || bStableTie)
						{
							ClosestDistanceSquared = DistanceSquared;
							Assignment.ParentComponentIndex = ComponentIndex;
						}
					}
				}
			}

			if (Assignment.ParentComponentIndex != INDEX_NONE)
			{
				Result.Add(MoveTemp(Assignment));
			}
		}
		return Result;
	}

	void MeasureComponentSubtrees(
		const TArray<int32>& ParentComponentIndices,
		const int32 TrunkComponentIndex,
		const TArray<TArray<FVector>>& ComponentCenterlines,
		TArray<double>& OutPersistenceLengths,
		TArray<double>& OutMaximumZValues)
	{
		check(ParentComponentIndices.Num() == ComponentCenterlines.Num());
		OutPersistenceLengths.SetNum(ComponentCenterlines.Num());
		OutMaximumZValues.SetNum(ComponentCenterlines.Num());
		TArray<double> ComponentLengths;
		ComponentLengths.SetNum(ComponentCenterlines.Num());
		for (int32 ComponentIndex = 0;
			ComponentIndex < ComponentCenterlines.Num();
			++ComponentIndex)
		{
			ComponentLengths[ComponentIndex] = MeasureCenterlineLength(
				ComponentCenterlines[ComponentIndex]);
			OutPersistenceLengths[ComponentIndex] =
				ComponentLengths[ComponentIndex];
			double MaximumZ = TNumericLimits<double>::Lowest();
			for (const FVector& Point : ComponentCenterlines[ComponentIndex])
			{
				MaximumZ = FMath::Max(MaximumZ, Point.Z);
			}
			OutMaximumZValues[ComponentIndex] = MaximumZ;
		}

		const TArray<int32> ComponentOrder = BuildHierarchyOrder(
			ParentComponentIndices,
			TrunkComponentIndex);
		for (int32 OrderIndex = ComponentOrder.Num() - 1;
			OrderIndex >= 0;
			--OrderIndex)
		{
			const int32 ComponentIndex = ComponentOrder[OrderIndex];
			const int32 ParentIndex = ParentComponentIndices[ComponentIndex];
			if (!ParentComponentIndices.IsValidIndex(ParentIndex))
			{
				continue;
			}
			OutPersistenceLengths[ParentIndex] = FMath::Max(
				OutPersistenceLengths[ParentIndex],
				ComponentLengths[ParentIndex]
					+ OutPersistenceLengths[ComponentIndex]);
			OutMaximumZValues[ParentIndex] = FMath::Max(
				OutMaximumZValues[ParentIndex],
				OutMaximumZValues[ComponentIndex]);
		}
	}

	FBranchAssignment AssignBranchIDs(
		const TArray<FWoodComponent>& WoodComponents,
		const int32 TrunkComponentIndex,
		const TArray<int32>& ParentComponentIndices,
		const TArray<TArray<FVector>>& ComponentCenterlines)
	{
		check(WoodComponents.IsValidIndex(TrunkComponentIndex));
		check(ParentComponentIndices.Num() == WoodComponents.Num());
		check(ComponentCenterlines.Num() == WoodComponents.Num());
		FBranchAssignment Result;
		Result.BranchIDs.Init(INDEX_NONE, WoodComponents.Num());

		const TArray<FVector>& TrunkCenterline =
			ComponentCenterlines[TrunkComponentIndex];
		const double TrunkLength = MeasureCenterlineLength(TrunkCenterline);
		check(TrunkLength > MinimumGeometryScale);
		const TArray<FCenterlineRadialSample> TrunkRadialSamples =
			BuildCenterlineRadialSamples(
				WoodComponents[TrunkComponentIndex],
				TrunkCenterline);
		const double BaseTrunkDiameter = MeasureLocalCenterlineDiameter(
			WoodComponents[TrunkComponentIndex],
			TrunkRadialSamples,
			TrunkLength,
			0.0);
		const double RootBandLength = FMath::Clamp(
			BaseTrunkDiameter * RootBandBaseDiameterScale,
			TrunkLength * MinimumRootBandLengthFraction,
			TrunkLength * MaximumRootBandLengthFraction);

		TArray<double> PersistenceLengths;
		TArray<double> MaximumZValues;
		MeasureComponentSubtrees(
			ParentComponentIndices,
			TrunkComponentIndex,
			ComponentCenterlines,
			PersistenceLengths,
			MaximumZValues);

		// H (trunk length), D (local trunk diameter), and P (subtree
		// persistence) make root recognition scale-independent. Every other
		// direct trunk child is a distinct wind branch; trunk stubs remain
		// manually editable branches instead of relying on a fragile guess.
		for (int32 ComponentIndex = 0;
			ComponentIndex < WoodComponents.Num();
			++ComponentIndex)
		{
			if (ParentComponentIndices[ComponentIndex]
				!= TrunkComponentIndex)
			{
				continue;
			}
			const TArray<FVector>& Centerline =
				ComponentCenterlines[ComponentIndex];
			const FClosestCenterlineLocation Attachment =
				FindClosestCenterlineLocation(Centerline[0], TrunkCenterline);
			const double PersistenceLength =
				PersistenceLengths[ComponentIndex];
			const double InitialRise =
				Centerline.Last().Z - Centerline[0].Z;
			const bool bIsRoot =
				Attachment.DistanceAlong <= RootBandLength
				&& MaximumZValues[ComponentIndex] <= RootBandLength
				&& InitialRise <= PersistenceLength
					* RootMaximumRisePersistenceFraction;
			if (bIsRoot)
			{
				++Result.RootSubtreeCount;
			}
			else
			{
				Result.BranchIDs[ComponentIndex] = Result.BranchCount;
				++Result.BranchCount;
			}
		}

		const TArray<int32> ComponentOrder = BuildHierarchyOrder(
			ParentComponentIndices,
			TrunkComponentIndex);
		for (const int32 ComponentIndex : ComponentOrder)
		{
			if (ComponentIndex == TrunkComponentIndex
				|| ParentComponentIndices[ComponentIndex]
					== TrunkComponentIndex)
			{
				continue;
			}
			const int32 ParentIndex = ParentComponentIndices[ComponentIndex];
			if (ParentComponentIndices.IsValidIndex(ParentIndex))
			{
				Result.BranchIDs[ComponentIndex] = Result.BranchIDs[ParentIndex];
			}
			else
			{
				Result.BranchIDs[ComponentIndex] = Result.BranchCount;
				++Result.BranchCount;
			}
		}
		return Result;
	}

	void AddCenterlineGeometry(
		const TArray<FVector>& Centerline,
		const double Radius,
		FFoliageBakerTreeHierarchyPreviewBranch& PreviewBranch)
	{
		check(Centerline.Num() >= 2);
		for (int32 PointIndex = 1; PointIndex < Centerline.Num(); ++PointIndex)
		{
			if (Centerline[PointIndex - 1].Equals(
					Centerline[PointIndex],
					MinimumGeometryScale))
			{
				continue;
			}
			FFoliageBakerTreeHierarchyPreviewCylinder& Cylinder =
				PreviewBranch.Cylinders.AddDefaulted_GetRef();
			Cylinder.Start = Centerline[PointIndex - 1];
			Cylinder.End = Centerline[PointIndex];
			Cylinder.Radius = Radius;
		}
		for (int32 PointIndex = 1;
			PointIndex < Centerline.Num() - 1;
			++PointIndex)
		{
			const FVector IncomingDirection = (
				Centerline[PointIndex] - Centerline[PointIndex - 1]).GetSafeNormal();
			const FVector OutgoingDirection = (
				Centerline[PointIndex + 1] - Centerline[PointIndex]).GetSafeNormal();
			if (IncomingDirection.IsNearlyZero()
				|| OutgoingDirection.IsNearlyZero()
				|| FVector::DotProduct(IncomingDirection, OutgoingDirection)
				> PreviewJointMinimumDirectionDot)
			{
				continue;
			}
			FFoliageBakerTreeHierarchyPreviewJoint& Joint =
				PreviewBranch.Joints.AddDefaulted_GetRef();
			Joint.Position = Centerline[PointIndex];
			Joint.Radius = Radius * JointPreviewRadiusScale;
		}
	}

	TSharedPtr<FFoliageBakerTreeHierarchyPreviewData> BuildPreviewData(
		UStaticMesh& StaticMesh,
		const TArray<FTreeTriangle>& Triangles,
		const TArray<FWoodComponent>& WoodComponents,
		const int32 TrunkComponentIndex,
		const TArray<int32>& ParentComponentIndices,
		const TArray<int32>& BranchIDs,
		const TArray<TArray<FVector>>& ComponentCenterlines,
		const TArray<FWoodCapIsland>& WoodCapIslands,
		const TArray<FLeafBranchAssignment>& LeafBranchAssignments,
		const FBox& WoodBounds,
		const int32 SourceLODIndex)
	{
		check(WoodComponents.IsValidIndex(TrunkComponentIndex));
		check(ParentComponentIndices.Num() == WoodComponents.Num());
		check(BranchIDs.Num() == WoodComponents.Num());
		check(ComponentCenterlines.Num() == WoodComponents.Num());

		TSharedRef<FFoliageBakerTreeHierarchyPreviewData> PreviewData =
			MakeShared<FFoliageBakerTreeHierarchyPreviewData>();
		PreviewData->AssetName = StaticMesh.GetName();
		PreviewData->Bounds = StaticMesh.GetBoundingBox();
		PreviewData->SourceStaticMesh = &StaticMesh;
		PreviewData->SourceLODIndex = SourceLODIndex;
		const double BranchRadius = FMath::Max(
			WoodBounds.GetSize().Size() * BranchPreviewRadiusTreeFraction,
			MinimumBranchPreviewRadius);

		const TArray<FVector>& TrunkCenterline =
			ComponentCenterlines[TrunkComponentIndex];
		FFoliageBakerTreeHierarchyPreviewBranch& TrunkBranch =
			PreviewData->Branches.AddDefaulted_GetRef();
		TrunkBranch.Color = FLinearColor::White;
		TrunkBranch.Label = TEXT("Trunk");
		TrunkBranch.LabelPosition = FindPointAlongCenterline(
			TrunkCenterline,
			0.55);
		AddCenterlineGeometry(
			TrunkCenterline,
			BranchRadius * TrunkPreviewRadiusScale,
			TrunkBranch);
		check(!TrunkBranch.Cylinders.IsEmpty());
		check(TrunkBranch.Cylinders[0].Start == FVector::ZeroVector);

		TMap<int32, int32> PreviewBranchIndexByID;

		for (int32 ComponentIndex = 0;
			ComponentIndex < WoodComponents.Num();
			++ComponentIndex)
		{
			if (ComponentIndex == TrunkComponentIndex
				|| BranchIDs[ComponentIndex] == INDEX_NONE)
			{
				continue;
			}

			const int32 ParentIndex = ParentComponentIndices[ComponentIndex];
			const TArray<FVector>& ComponentCenterline =
				ComponentCenterlines[ComponentIndex];
			const FVector& ComponentStart = ComponentCenterline[0];

			const int32 BranchID = BranchIDs[ComponentIndex];
			int32& PreviewBranchIndex = PreviewBranchIndexByID.FindOrAdd(
				BranchID,
				INDEX_NONE);
			if (PreviewBranchIndex == INDEX_NONE)
			{
				PreviewBranchIndex = PreviewData->Branches.AddDefaulted();
				PreviewData->Branches[PreviewBranchIndex].BranchID = BranchID;
				PreviewData->Branches[PreviewBranchIndex].Color = FLinearColor(
					MakeBranchColor(StaticMesh, BranchID));
			}
			FFoliageBakerTreeHierarchyPreviewBranch& PreviewBranch =
				PreviewData->Branches[PreviewBranchIndex];
			AddCenterlineGeometry(
				ComponentCenterline,
				BranchRadius,
				PreviewBranch);
			for (const int32 TriangleIndex :
				WoodComponents[ComponentIndex].TriangleIndices)
			{
				PreviewBranch.SourceTriangleIDs.Add(
					Triangles[TriangleIndex].TriangleID.GetValue());
			}

			if (WoodComponents.IsValidIndex(ParentIndex)
				&& ParentIndex != TrunkComponentIndex
				&& BranchIDs[ParentIndex] == BranchID)
			{
				FFoliageBakerTreeHierarchyPreviewJoint& Joint =
					PreviewBranch.Joints.AddDefaulted_GetRef();
				Joint.Position = ComponentStart;
				Joint.Radius = BranchRadius * JointPreviewRadiusScale;
			}
			if (ParentIndex == TrunkComponentIndex || ParentIndex == INDEX_NONE)
			{
				PreviewBranch.Label = FString::Printf(
					TEXT("ID %d"),
					BranchID);
				PreviewBranch.LabelPosition = FindPointAlongCenterline(
					ComponentCenterline,
					0.55);
			}
		}

		for (const FWoodCapIsland& CapIsland : WoodCapIslands)
		{
			if (!BranchIDs.IsValidIndex(CapIsland.OwnerComponentIndex))
			{
				continue;
			}
			const int32 BranchID = BranchIDs[CapIsland.OwnerComponentIndex];
			if (!PreviewBranchIndexByID.Contains(BranchID))
			{
				continue;
			}
			const int32 PreviewBranchIndex =
				PreviewBranchIndexByID.FindRef(BranchID);
			if (!PreviewData->Branches.IsValidIndex(PreviewBranchIndex))
			{
				continue;
			}
			FFoliageBakerTreeHierarchyPreviewBranch& PreviewBranch =
				PreviewData->Branches[PreviewBranchIndex];
			for (const int32 TriangleIndex : CapIsland.TriangleIndices)
			{
				PreviewBranch.SourceTriangleIDs.Add(
					Triangles[TriangleIndex].TriangleID.GetValue());
			}
		}

		for (const FLeafBranchAssignment& Assignment : LeafBranchAssignments)
		{
			if (!BranchIDs.IsValidIndex(Assignment.ParentComponentIndex))
			{
				continue;
			}
			FFoliageBakerTreeHierarchyPreviewLeafCluster& LeafCluster =
				PreviewData->LeafClusters.AddDefaulted_GetRef();
			LeafCluster.ParentBranchID =
				BranchIDs[Assignment.ParentComponentIndex];
			LeafCluster.Bounds = Assignment.Bounds;
			LeafCluster.TrianglePositions = Assignment.TrianglePositions;
			for (const int32 TriangleIndex : Assignment.TriangleIndices)
			{
				LeafCluster.SourceTriangleIDs.Add(
					Triangles[TriangleIndex].TriangleID.GetValue());
			}
		}
		return PreviewData;
	}

	struct FSkeletonClosestLocation
	{
		int32 EdgeIndex = INDEX_NONE;
		double DistanceSquared = TNumericLimits<double>::Max();
	};

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

	EFoliageBakerTreeHierarchyPreviewNodeKind ConvertNodeKind(
		const EFoliageBakerTreeSkeletonNodeKind Kind)
	{
		switch (Kind)
		{
		case EFoliageBakerTreeSkeletonNodeKind::Root:
			return EFoliageBakerTreeHierarchyPreviewNodeKind::Root;
		case EFoliageBakerTreeSkeletonNodeKind::Fork:
			return EFoliageBakerTreeHierarchyPreviewNodeKind::Fork;
		case EFoliageBakerTreeSkeletonNodeKind::Tip:
		default:
			return EFoliageBakerTreeHierarchyPreviewNodeKind::Tip;
		}
	}

	TSharedPtr<FFoliageBakerTreeHierarchyPreviewData> BuildSkeletonPreviewData(
		UStaticMesh& StaticMesh,
		const int32 SourceLODIndex,
		const TArray<FTreeTriangle>& Triangles,
		const TVertexAttributesConstRef<FVector3f>& VertexPositions,
		const TArray<int32>& WoodTriangleIndices,
		const TArray<FTriangleComponent>& LeafTriangleComponents,
		const TArray<FWoodCapIsland>& WoodCapIslands,
		const FFoliageBakerTreeSkeletonResult& Skeleton,
		TArray<int32>& OutTriangleBranchIDs)
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
		PreviewData->RootNodeID = Skeleton.RootNodeID;
		PreviewData->SkeletonCellSize = Skeleton.CellSize;
		PreviewData->WoodVolumeComponentCount = Skeleton.OccupiedComponentCount;
		PreviewData->UncoveredGuideTerminalCount =
			Skeleton.UncoveredGuideTerminalCount;
		for (const FFoliageBakerTreeSkeletonNode& SkeletonNode : Skeleton.Nodes)
		{
			FFoliageBakerTreeHierarchyPreviewNode& Node =
				PreviewData->SkeletonNodes.AddDefaulted_GetRef();
			Node.NodeID = SkeletonNode.NodeID;
			Node.ParentNodeID = SkeletonNode.ParentNodeID;
			Node.Position = SkeletonNode.Position;
			Node.Radius = SkeletonNode.Radius;
			Node.Kind = ConvertNodeKind(SkeletonNode.Kind);
		}

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
			FFoliageBakerTreeHierarchyPreviewEdge& Edge =
				PreviewData->SkeletonEdges.AddDefaulted_GetRef();
			Edge.EdgeID = SkeletonEdge.EdgeID;
			Edge.StartNodeID = SkeletonEdge.StartNodeID;
			Edge.EndNodeID = SkeletonEdge.EndNodeID;
			Edge.BranchID = SkeletonEdge.BranchID;
			Edge.ParentBranchID = SkeletonEdge.ParentBranchID;
			Edge.bTrunk = SkeletonEdge.bTrunk;
			Edge.Polyline = SkeletonEdge.Polyline;

			const int32 BranchID = SkeletonEdge.bTrunk
				? INDEX_NONE
				: SkeletonEdge.BranchID;
			int32 PreviewBranchIndex = INDEX_NONE;
			if (SkeletonEdge.bTrunk)
			{
				PreviewBranchIndex = TrunkPreviewBranchIndex;
			}
			else
			{
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
					Branch.LabelPosition = SkeletonEdge.Polyline.IsEmpty()
						? FVector::ZeroVector
						: SkeletonEdge.Polyline[SkeletonEdge.Polyline.Num() / 2];
				}
				PreviewBranchIndex = BranchIndex;
			}
			FFoliageBakerTreeHierarchyPreviewBranch& Branch =
				PreviewData->Branches[PreviewBranchIndex];
			const double PreviewRadius = SkeletonEdge.bTrunk
				? MinimumPreviewRadius * TrunkPreviewRadiusScale
				: MinimumPreviewRadius;
			for (int32 PointIndex = 1;
				PointIndex < SkeletonEdge.Polyline.Num();
				++PointIndex)
			{
				if (SkeletonEdge.Polyline[PointIndex - 1].Equals(
						SkeletonEdge.Polyline[PointIndex],
						MinimumGeometryScale))
				{
					continue;
				}
				FFoliageBakerTreeHierarchyPreviewCylinder& Cylinder =
					Branch.Cylinders.AddDefaulted_GetRef();
				Cylinder.Start = SkeletonEdge.Polyline[PointIndex - 1];
				Cylinder.End = SkeletonEdge.Polyline[PointIndex];
				Cylinder.Radius = PreviewRadius;
			}
		}

		for (const FFoliageBakerTreeSkeletonNode& SkeletonNode : Skeleton.Nodes)
		{
			if (SkeletonNode.Kind != EFoliageBakerTreeSkeletonNodeKind::Fork)
			{
				continue;
			}
			for (FFoliageBakerTreeHierarchyPreviewBranch& Branch : PreviewData->Branches)
			{
				bool bUsesNode = false;
				for (const FFoliageBakerTreeHierarchyPreviewEdge& Edge :
					PreviewData->SkeletonEdges)
				{
					const bool bSameBranch =
						(Branch.Label == TEXT("Trunk") && Edge.bTrunk)
						|| (Branch.BranchID != INDEX_NONE
							&& !Edge.bTrunk
							&& Edge.BranchID == Branch.BranchID);
					if (bSameBranch
						&& (Edge.StartNodeID == SkeletonNode.NodeID
							|| Edge.EndNodeID == SkeletonNode.NodeID))
					{
						bUsesNode = true;
						break;
					}
				}
				if (!bUsesNode)
				{
					continue;
				}
				FFoliageBakerTreeHierarchyPreviewJoint& Joint =
					Branch.Joints.AddDefaulted_GetRef();
				Joint.Position = SkeletonNode.Position;
				Joint.Radius = MinimumPreviewRadius * JointPreviewRadiusScale;
			}
		}
		FFoliageBakerTreeHierarchyPreviewBranch& FinalTrunk =
			PreviewData->Branches[TrunkPreviewBranchIndex];
		if (!FinalTrunk.Cylinders.IsEmpty())
		{
			FinalTrunk.LabelPosition = FinalTrunk.Cylinders[
				FinalTrunk.Cylinders.Num() / 2].Start;
		}
		OutTriangleBranchIDs.Init(INDEX_NONE, Triangles.Num());
		TBitArray<> WoodOwned(false, Triangles.Num());
		TArray<int32> SkeletonWoodTriangleIndices = WoodTriangleIndices;
		TSet<int32> WoodCapTriangleIndices;
		for (const FWoodCapIsland& CapIsland : WoodCapIslands)
		{
			for (const int32 TriangleIndex : CapIsland.TriangleIndices)
			{
				WoodCapTriangleIndices.Add(TriangleIndex);
				SkeletonWoodTriangleIndices.AddUnique(TriangleIndex);
			}
		}
		for (const int32 TriangleIndex : SkeletonWoodTriangleIndices)
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
			OutTriangleBranchIDs[TriangleIndex] = BranchID;
			WoodOwned[TriangleIndex] = true;
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
				PreviewData->Branches[PreviewBranchIndex].SourceTriangleIDs.Add(
					Triangle.TriangleID.GetValue());
			}
		}

		for (const FTriangleComponent& LeafComponent : LeafTriangleComponents)
		{
			TArray<int32> LeafTriangles;
			FVector Representative = FVector::ZeroVector;
			int32 RepresentativeCount = 0;
			FBox Bounds(EForceInit::ForceInit);
			TArray<FVector3f> TrianglePositions;
			for (const int32 TriangleIndex : LeafComponent.TriangleIndices)
			{
				if (WoodCapTriangleIndices.Contains(TriangleIndex)
					|| !Triangles.IsValidIndex(TriangleIndex))
				{
					continue;
				}
				LeafTriangles.Add(TriangleIndex);
				for (const FVertexID VertexID : Triangles[TriangleIndex].VertexIDs)
				{
					const FVector3f Position = VertexPositions[VertexID];
					Representative += FVector(Position);
					++RepresentativeCount;
					Bounds += FVector(Position);
					TrianglePositions.Add(Position);
				}
			}
			if (LeafTriangles.IsEmpty() || RepresentativeCount == 0)
			{
				continue;
			}
			Representative /= static_cast<double>(RepresentativeCount);
			const FSkeletonClosestLocation Closest = FindClosestSkeletonEdge(
				Representative,
				Skeleton.Edges);
			if (!Skeleton.Edges.IsValidIndex(Closest.EdgeIndex))
			{
				continue;
			}
			const FFoliageBakerTreeSkeletonEdge& Edge = Skeleton.Edges[Closest.EdgeIndex];
			FFoliageBakerTreeHierarchyPreviewLeafCluster& LeafCluster =
				PreviewData->LeafClusters.AddDefaulted_GetRef();
			LeafCluster.ParentBranchID = Edge.bTrunk
				? INDEX_NONE
				: Edge.BranchID;
			LeafCluster.Bounds = Bounds;
			LeafCluster.TrianglePositions = MoveTemp(TrianglePositions);
			for (const int32 TriangleIndex : LeafTriangles)
			{
				LeafCluster.SourceTriangleIDs.Add(
					Triangles[TriangleIndex].TriangleID.GetValue());
			}
		}
		return PreviewData;
	}
}

FFoliageBakerTreeHierarchyColorBakeResult
FFoliageBakerTreeHierarchyColorBaker::Bake(
	UStaticMesh& StaticMesh,
	const int32 SourceLODIndex)
{
	FFoliageBakerTreeHierarchyColorBakeResult Result;
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

	int32 LeafMaterialIndex = INDEX_NONE;
	if (!FindLeafMaterialIndex(Triangles, LeafMaterialIndex, Error))
	{
		Result.Report = FString::Printf(
			TEXT("%s\n  failed: %s"),
			*StaticMesh.GetName(),
			*Error);
		return Result;
	}

	TArray<int32> WoodTriangleIndices;
	TArray<int32> LeafTriangleIndices;
	for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
	{
		if (Triangles[TriangleIndex].MaterialIndex != LeafMaterialIndex)
		{
			WoodTriangleIndices.Add(TriangleIndex);
		}
		else
		{
			LeafTriangleIndices.Add(TriangleIndex);
		}
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
	const TArray<FTriangleComponent> LeafTriangleComponents =
		BuildTriangleComponents(Triangles, LeafTriangleIndices);
	if (WoodTriangleComponents.IsEmpty())
	{
		Result.Report = FString::Printf(
			TEXT("%s\n  failed: No wood geometry remains after card-foliage classification."),
			*StaticMesh.GetName());
		return Result;
	}

	const TVertexAttributesConstRef<FVector3f> VertexPositions =
		SourceMeshDescription.GetVertexPositions();
	TArray<FWoodComponent> WoodComponents;
	WoodComponents.Reserve(WoodTriangleComponents.Num());
	FBox WoodBounds(EForceInit::ForceInit);
	for (const FTriangleComponent& Component : WoodTriangleComponents)
	{
		FWoodComponent& WoodComponent = WoodComponents.Add_GetRef(
			BuildWoodComponent(Component, Triangles, VertexPositions));
		WoodBounds += WoodComponent.Bounds;
	}
	TArray<int32> WoodComponentIDByTriangle;
	WoodComponentIDByTriangle.Init(INDEX_NONE, Triangles.Num());
	for (int32 ComponentIndex = 0;
		ComponentIndex < WoodComponents.Num();
		++ComponentIndex)
	{
		for (const int32 TriangleIndex : WoodComponents[ComponentIndex].TriangleIndices)
		{
			if (WoodComponentIDByTriangle.IsValidIndex(TriangleIndex))
			{
				WoodComponentIDByTriangle[TriangleIndex] = ComponentIndex;
			}
		}
	}
	const TArray<FWoodCapIsland> WoodCapIslands =
		FindTopologyOwnedWoodCapIslands(
			LeafTriangleComponents,
			WoodTriangleComponents,
			Triangles,
			VertexPositions);

	TArray<int32> SkeletonWoodTriangleIndices = WoodTriangleIndices;
	for (const FWoodCapIsland& CapIsland : WoodCapIslands)
	{
		for (const int32 TriangleIndex : CapIsland.TriangleIndices)
		{
			SkeletonWoodTriangleIndices.AddUnique(TriangleIndex);
			if (WoodComponentIDByTriangle.IsValidIndex(TriangleIndex))
			{
				WoodComponentIDByTriangle[TriangleIndex] =
					CapIsland.OwnerComponentIndex;
			}
		}
	}
	TArray<FFoliageBakerTreeSkeletonTriangle> SkeletonTriangles;
	SkeletonTriangles.Reserve(SkeletonWoodTriangleIndices.Num());
	for (const int32 TriangleIndex : SkeletonWoodTriangleIndices)
	{
		if (!Triangles.IsValidIndex(TriangleIndex))
		{
			continue;
		}
		const FTreeTriangle& Triangle = Triangles[TriangleIndex];
		FFoliageBakerTreeSkeletonTriangle& SkeletonTriangle =
			SkeletonTriangles.AddDefaulted_GetRef();
		SkeletonTriangle.SourceTriangleID = Triangle.TriangleID.GetValue();
		SkeletonTriangle.SourceComponentID =
			WoodComponentIDByTriangle[TriangleIndex];
		SkeletonTriangle.A = FVector(VertexPositions[Triangle.VertexIDs[0]]);
		SkeletonTriangle.B = FVector(VertexPositions[Triangle.VertexIDs[1]]);
		SkeletonTriangle.C = FVector(VertexPositions[Triangle.VertexIDs[2]]);
	}
	UE_LOG(
		LogFoliageBakerTreeHierarchy,
		Display,
		TEXT("%s: starting pure global GPU skeletonization with %d wood triangles."),
		*StaticMesh.GetName(),
		SkeletonTriangles.Num());
	const double SkeletonStartSeconds = FPlatformTime::Seconds();
	const FFoliageBakerTreeSkeletonResult Skeleton =
		FFoliageBakerTreeSkeleton::Build(
			SkeletonTriangles,
			{},
			FVector::ZeroVector);
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

	TArray<int32> TriangleBranchIDs;
	Result.PreviewData = BuildSkeletonPreviewData(
		StaticMesh,
		SourceLODIndex,
		Triangles,
		VertexPositions,
		WoodTriangleIndices,
		LeafTriangleComponents,
		WoodCapIslands,
		Skeleton,
		TriangleBranchIDs);
	Result.LeafClusterCount = Result.PreviewData.IsValid()
		? Result.PreviewData->LeafClusters.Num()
		: 0;

	TArray<FVector4f> TriangleColors;
	TriangleColors.Init(FVector4f(0.0f, 0.0f, 0.0f, 1.0f), Triangles.Num());
	for (const int32 TriangleIndex : SkeletonWoodTriangleIndices)
	{
		if (!Triangles.IsValidIndex(TriangleIndex)
			|| !TriangleBranchIDs.IsValidIndex(TriangleIndex))
		{
			continue;
		}
		const int32 BranchID = TriangleBranchIDs[TriangleIndex];
		const FVector4f Color = BranchID == INDEX_NONE
			? FVector4f(1.0f, 1.0f, 1.0f, 1.0f)
			: MakeBranchColor(StaticMesh, BranchID);
		TriangleColors[TriangleIndex] = Color;
	}
	StaticMesh.Modify();
	if (!StaticMesh.ModifyMeshDescription(SourceLODIndex, false))
	{
		Result.Report = FString::Printf(
			TEXT("%s\n  failed: Source LOD %d could not be added to the undo transaction."),
			*StaticMesh.GetName(),
			SourceLODIndex);
		return Result;
	}
	StaticMesh.PreEditChange(nullptr);
	FMeshDescription& EditableMeshDescription =
		*StaticMesh.GetMeshDescription(SourceLODIndex);
	if (!EditableMeshDescription.VertexInstanceAttributes().HasAttribute(
			MeshAttribute::VertexInstance::Color))
	{
		EditableMeshDescription.VertexInstanceAttributes()
			.RegisterAttribute<FVector4f>(
				MeshAttribute::VertexInstance::Color,
				1,
				FVector4f(1.0f, 1.0f, 1.0f, 1.0f),
				EMeshAttributeFlags::Lerpable | EMeshAttributeFlags::Mandatory);
	}
	FStaticMeshAttributes Attributes(EditableMeshDescription);
	TVertexInstanceAttributesRef<FVector4f> VertexInstanceColors =
		Attributes.GetVertexInstanceColors();
	for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
	{
		const TArrayView<const FVertexInstanceID> VertexInstanceIDs =
			EditableMeshDescription.GetTriangleVertexInstances(
				Triangles[TriangleIndex].TriangleID);
		if (VertexInstanceIDs.Num() != 3)
		{
			continue;
		}
		for (const FVertexInstanceID VertexInstanceID : VertexInstanceIDs)
		{
			VertexInstanceColors[VertexInstanceID] = TriangleColors[TriangleIndex];
		}
	}

	UStaticMesh::FCommitMeshDescriptionParams CommitParams;
	CommitParams.bMarkPackageDirty = true;
	CommitParams.bUseHashAsGuid = false;
	StaticMesh.CommitMeshDescription(SourceLODIndex, CommitParams);
	StaticMesh.PostEditChange();
	StaticMesh.MarkPackageDirty();

	Result.bSucceeded = true;
	Result.CreatedAssets.Add(TStrongObjectPtr<UObject>(&StaticMesh));
	Result.Report = FString::Printf(
		TEXT("%s\n  wrote LOD %d hierarchy test colors: trunk white, card foliage black, %d branch ID color(s), %d leaf cluster parent assignment(s), %d topology-owned wood cap island(s), %d source wood component(s) (median %d, maximum %d triangles).\n  %s"),
		*StaticMesh.GetName(),
		SourceLODIndex,
		Result.BranchCount,
		Result.LeafClusterCount,
		WoodCapIslands.Num(),
		WoodTriangleComponents.Num(),
		MedianWoodComponentTriangleCount,
		MaximumWoodComponentTriangleCount,
		*Skeleton.Report);
	return Result;
}

bool FFoliageBakerTreeHierarchyColorBaker::MarkBranchAsTrunk(
	UStaticMesh& StaticMesh,
	const int32 SourceLODIndex,
	const TArray<int32>& SourceTriangleIDs,
	FString& OutError)
{
	if (SourceLODIndex < 0
		|| !StaticMesh.IsSourceModelValid(SourceLODIndex)
		|| !StaticMesh.IsMeshDescriptionValid(SourceLODIndex))
	{
		OutError = FString::Printf(
			TEXT("Source LOD %d has no editable MeshDescription."),
			SourceLODIndex);
		return false;
	}
	const FMeshDescription& SourceMeshDescription =
		*StaticMesh.GetMeshDescription(SourceLODIndex);
	bool bHasValidTriangle = false;
	for (const int32 SourceTriangleID : SourceTriangleIDs)
	{
		if (SourceMeshDescription.IsTriangleValid(
				FTriangleID(SourceTriangleID)))
		{
			bHasValidTriangle = true;
			break;
		}
	}
	if (!bHasValidTriangle)
	{
		OutError = TEXT("The selected branch has no valid source triangles.");
		return false;
	}

	StaticMesh.Modify();
	if (!StaticMesh.ModifyMeshDescription(SourceLODIndex, false))
	{
		OutError = FString::Printf(
			TEXT("Source LOD %d could not be added to the undo transaction."),
			SourceLODIndex);
		return false;
	}
	StaticMesh.PreEditChange(nullptr);
	FMeshDescription& EditableMeshDescription =
		*StaticMesh.GetMeshDescription(SourceLODIndex);
	if (!EditableMeshDescription.VertexInstanceAttributes().HasAttribute(
		MeshAttribute::VertexInstance::Color))
	{
		EditableMeshDescription.VertexInstanceAttributes()
			.RegisterAttribute<FVector4f>(
				MeshAttribute::VertexInstance::Color,
				1,
				FVector4f(1.0f, 1.0f, 1.0f, 1.0f),
				EMeshAttributeFlags::Lerpable | EMeshAttributeFlags::Mandatory);
	}
	FStaticMeshAttributes Attributes(EditableMeshDescription);
	TVertexInstanceAttributesRef<FVector4f> VertexInstanceColors =
		Attributes.GetVertexInstanceColors();
	for (const int32 SourceTriangleID : SourceTriangleIDs)
	{
		const FTriangleID TriangleID(SourceTriangleID);
		if (!EditableMeshDescription.IsTriangleValid(TriangleID))
		{
			continue;
		}
		const TArrayView<const FVertexInstanceID> VertexInstanceIDs =
			EditableMeshDescription.GetTriangleVertexInstances(TriangleID);
		if (VertexInstanceIDs.Num() != 3)
		{
			continue;
		}
		for (const FVertexInstanceID VertexInstanceID : VertexInstanceIDs)
		{
			VertexInstanceColors[VertexInstanceID] = FVector4f(
				1.0f,
				1.0f,
				1.0f,
				1.0f);
		}
	}

	UStaticMesh::FCommitMeshDescriptionParams CommitParams;
	CommitParams.bMarkPackageDirty = true;
	CommitParams.bUseHashAsGuid = false;
	StaticMesh.CommitMeshDescription(SourceLODIndex, CommitParams);
	StaticMesh.PostEditChange();
	StaticMesh.MarkPackageDirty();
	return true;
}
