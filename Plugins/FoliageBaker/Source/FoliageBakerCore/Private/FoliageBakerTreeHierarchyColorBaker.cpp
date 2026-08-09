#include "FoliageBakerTreeHierarchyColorBaker.h"

#include "Algo/Reverse.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

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
	constexpr double PreviewSliceLengthScale = 2.0;
	constexpr double TrunkPreviewSliceLengthScale = 0.75;
	constexpr int32 MaximumPreviewSliceCount = 48;
	constexpr int32 MaximumTrunkPreviewSliceCount = 128;
	constexpr double PreviewCenterlineSimplificationScale = 2.0;
	constexpr double TrunkPreviewCenterlineSimplificationScale = 0.75;
	constexpr double PreviewCenterlineSimplificationLengthFraction = 0.003;
	constexpr double TrunkPreviewCenterlineSimplificationLengthFraction = 0.001;
	constexpr int32 PreviewCenterlineSmoothingPassCount = 1;
	constexpr int32 TrunkPreviewCenterlineSmoothingPassCount = 3;
	constexpr double PreviewCenterlineImprovementRatio = 0.9;
	constexpr double MaximumPreviewCenterlineLengthRatio = 1.35;
	constexpr double MinimumPreviewCenterlineDirectionDot = 0.5;
	constexpr double PreviewJointMinimumDirectionDot = 0.984807753012208;
	constexpr double ComponentEndpointBandFraction = 0.2;
	constexpr double RootAttachmentHeightFraction = 0.04;
	constexpr double RootMaximumHeightFraction = 0.06;
	constexpr double RootAttachmentRadiusScale = 2.0;
	constexpr double RootMaximumRadiusScale = 3.0;
	constexpr double RootMaximumRiseFraction = 0.25;
	constexpr double TrunkStubMaximumLengthRadiusRatio = 4.0;
	constexpr double TrunkStubContinuationStartFraction = 0.5;
	constexpr double TrunkStubMinimumContinuationDirectionDot = 0.0;

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

	struct FWoodComponentShape
	{
		FVector AttachmentCenter = FVector::ZeroVector;
		FVector TipCenter = FVector::ZeroVector;
		double Length = 0.0;
		double AttachmentRadius = 0.0;
		double TipRadius = 0.0;
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

	FWoodComponentShape MeasureWoodComponentShape(
		const FWoodComponent& Component,
		const FWoodComponent& Trunk)
	{
		check(!Component.Positions.IsEmpty());
		check(!Trunk.Positions.IsEmpty());
		const FVector Axis = Component.PrincipalAxis.GetSafeNormal();
		check(!Axis.IsNearlyZero());

		FVector Center = FVector::ZeroVector;
		for (const FVector& Position : Component.Positions)
		{
			Center += Position;
		}
		Center /= static_cast<double>(Component.Positions.Num());

		double MinimumProjection = TNumericLimits<double>::Max();
		double MaximumProjection = TNumericLimits<double>::Lowest();
		for (const FVector& Position : Component.Positions)
		{
			const double Projection = FVector::DotProduct(Position - Center, Axis);
			MinimumProjection = FMath::Min(MinimumProjection, Projection);
			MaximumProjection = FMath::Max(MaximumProjection, Projection);
		}

		FWoodComponentShape Shape;
		Shape.Length = MaximumProjection - MinimumProjection;
		if (Shape.Length <= MinimumGeometryScale)
		{
			return Shape;
		}

		const FVector MinimumCenter = Center + Axis * MinimumProjection;
		const FVector MaximumCenter = Center + Axis * MaximumProjection;
		const bool bMinimumIsAttachment =
			FindClosestPositionDistanceSquared(MinimumCenter, Trunk.Positions)
			<= FindClosestPositionDistanceSquared(MaximumCenter, Trunk.Positions);
		Shape.AttachmentCenter = bMinimumIsAttachment
			? MinimumCenter
			: MaximumCenter;
		Shape.TipCenter = bMinimumIsAttachment
			? MaximumCenter
			: MinimumCenter;

		const double EndpointBandLength =
			Shape.Length * ComponentEndpointBandFraction;
		TArray<double> MinimumEndRadii;
		TArray<double> MaximumEndRadii;
		for (const FVector& Position : Component.Positions)
		{
			const FVector CenterDelta = Position - Center;
			const double Projection = FVector::DotProduct(CenterDelta, Axis);
			const double Radius = (
				CenterDelta - Axis * Projection).Size();
			if (Projection <= MinimumProjection + EndpointBandLength)
			{
				MinimumEndRadii.Add(Radius);
			}
			if (Projection >= MaximumProjection - EndpointBandLength)
			{
				MaximumEndRadii.Add(Radius);
			}
		}
		const double MinimumRadius = Median(MinimumEndRadii);
		const double MaximumRadius = Median(MaximumEndRadii);
		Shape.AttachmentRadius = bMinimumIsAttachment
			? MinimumRadius
			: MaximumRadius;
		Shape.TipRadius = bMinimumIsAttachment
			? MaximumRadius
			: MinimumRadius;
		return Shape;
	}

	bool IsRootLikeTrunkContact(
		const FWoodComponentShape& Shape,
		const FBox& WoodBounds)
	{
		const double TreeHeight = WoodBounds.GetSize().Z;
		if (TreeHeight <= MinimumGeometryScale
			|| Shape.Length <= MinimumGeometryScale)
		{
			return false;
		}
		const double MaximumRadius = FMath::Max(
			Shape.AttachmentRadius,
			Shape.TipRadius);
		const double MaximumAttachmentZ = FMath::Max(
			TreeHeight * RootAttachmentHeightFraction,
			Shape.AttachmentRadius * RootAttachmentRadiusScale);
		const double MaximumComponentZ = FMath::Max(
			TreeHeight * RootMaximumHeightFraction,
			MaximumRadius * RootMaximumRadiusScale);
		const double VerticalProgress =
			Shape.TipCenter.Z - Shape.AttachmentCenter.Z;
		return Shape.AttachmentCenter.Z <= MaximumAttachmentZ
			&& FMath::Max(Shape.AttachmentCenter.Z, Shape.TipCenter.Z)
				<= MaximumComponentZ
			&& VerticalProgress <= Shape.Length * RootMaximumRiseFraction;
	}

	bool HasStructuralContinuation(
		const int32 ComponentIndex,
		const int32 TrunkComponentIndex,
		const TArray<FWoodComponent>& Components,
		const TArray<TArray<FWoodContact>>& Adjacency,
		const FWoodComponentShape& Shape)
	{
		const FVector GrowthDirection = (
			Shape.TipCenter - Shape.AttachmentCenter).GetSafeNormal();
		if (GrowthDirection.IsNearlyZero())
		{
			return false;
		}
		for (const FWoodContact& Contact : Adjacency[ComponentIndex])
		{
			if (Contact.NeighborIndex == TrunkComponentIndex)
			{
				continue;
			}
			const FWoodComponentShape NeighborShape = MeasureWoodComponentShape(
				Components[Contact.NeighborIndex],
				Components[ComponentIndex]);
			if (NeighborShape.Length <= MinimumGeometryScale)
			{
				continue;
			}
			const double AttachmentProgress = FVector::DotProduct(
				NeighborShape.AttachmentCenter - Shape.AttachmentCenter,
				GrowthDirection) / Shape.Length;
			const FVector NeighborGrowthDirection = (
				NeighborShape.TipCenter
				- NeighborShape.AttachmentCenter).GetSafeNormal();
			if (AttachmentProgress >= TrunkStubContinuationStartFraction
				&& FVector::DotProduct(
					GrowthDirection,
					NeighborGrowthDirection)
					>= TrunkStubMinimumContinuationDirectionDot)
			{
				return true;
			}
		}
		return false;
	}

	bool IsTrunkStub(
		const int32 ComponentIndex,
		const int32 TrunkComponentIndex,
		const TArray<FWoodComponent>& Components,
		const TArray<TArray<FWoodContact>>& Adjacency,
		const FWoodComponentShape& Shape)
	{
		if (Shape.Length <= MinimumGeometryScale
			|| Shape.AttachmentRadius <= MinimumGeometryScale)
		{
			return false;
		}
		const bool bIsShort = Shape.Length
			<= Shape.AttachmentRadius * TrunkStubMaximumLengthRadiusRatio;
		return bIsShort
			&& !HasStructuralContinuation(
				ComponentIndex,
				TrunkComponentIndex,
				Components,
				Adjacency,
				Shape);
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

	void PropagateBranchIDs(
		const TArray<TArray<FWoodContact>>& Adjacency,
		const int32 TrunkComponentIndex,
		const TArray<bool>& LockedBranchRoots,
		TArray<bool>& ProcessedComponents,
		TArray<double>& BranchDistances,
		TArray<int32>& OutParentComponentIndices,
		TArray<int32>& OutBranchIDs)
	{
		while (true)
		{
			int32 CurrentIndex = INDEX_NONE;
			for (int32 ComponentIndex = 0;
				ComponentIndex < Adjacency.Num();
				++ComponentIndex)
			{
				if (ComponentIndex == TrunkComponentIndex
					|| ProcessedComponents[ComponentIndex]
					|| OutBranchIDs[ComponentIndex] == INDEX_NONE)
				{
					continue;
				}
				if (CurrentIndex == INDEX_NONE
					|| BranchDistances[ComponentIndex]
						< BranchDistances[CurrentIndex]
					|| (FMath::IsNearlyEqual(
							BranchDistances[ComponentIndex],
							BranchDistances[CurrentIndex],
							BranchDistanceComparisonTolerance)
						&& OutBranchIDs[ComponentIndex]
							< OutBranchIDs[CurrentIndex]))
				{
					CurrentIndex = ComponentIndex;
				}
			}
			if (CurrentIndex == INDEX_NONE)
			{
				return;
			}

			ProcessedComponents[CurrentIndex] = true;
			for (const FWoodContact& Contact : Adjacency[CurrentIndex])
			{
				const int32 NeighborIndex = Contact.NeighborIndex;
				if (NeighborIndex == TrunkComponentIndex
					|| ProcessedComponents[NeighborIndex]
					|| LockedBranchRoots[NeighborIndex])
				{
					continue;
				}

				const double CandidateDistance = BranchDistances[CurrentIndex]
					+ Contact.InheritanceCost;
				const bool bHasNoBranch =
					OutBranchIDs[NeighborIndex] == INDEX_NONE;
				const bool bHasShorterPath = CandidateDistance
					+ BranchDistanceComparisonTolerance
					< BranchDistances[NeighborIndex];
				const bool bWinsEqualDistanceTie = FMath::IsNearlyEqual(
						CandidateDistance,
						BranchDistances[NeighborIndex],
						BranchDistanceComparisonTolerance)
					&& OutBranchIDs[CurrentIndex]
						< OutBranchIDs[NeighborIndex];
				if (bHasNoBranch || bHasShorterPath || bWinsEqualDistanceTie)
				{
					BranchDistances[NeighborIndex] = CandidateDistance;
					OutParentComponentIndices[NeighborIndex] = CurrentIndex;
					OutBranchIDs[NeighborIndex] = OutBranchIDs[CurrentIndex];
				}
			}
		}
	}

	int32 AssignBranchIDs(
		const TArray<FWoodComponent>& Components,
		const FBox& WoodBounds,
		const TArray<TArray<FWoodContact>>& Adjacency,
		const int32 TrunkComponentIndex,
		TArray<int32>& OutParentComponentIndices,
		TArray<int32>& OutBranchIDs)
	{
		check(Components.Num() == Adjacency.Num());
		check(Components.IsValidIndex(TrunkComponentIndex));
		OutParentComponentIndices.Init(INDEX_NONE, Adjacency.Num());
		OutBranchIDs.Init(INDEX_NONE, Adjacency.Num());
		TArray<double> BranchDistances;
		BranchDistances.Init(TNumericLimits<double>::Max(), Adjacency.Num());
		TArray<bool> LockedBranchRoots;
		LockedBranchRoots.Init(false, Adjacency.Num());
		TArray<bool> ProcessedComponents;
		ProcessedComponents.Init(false, Adjacency.Num());
		ProcessedComponents[TrunkComponentIndex] = true;

		int32 BranchCount = 0;
		// Roots and terminal cut stubs inherit the trunk. Every remaining
		// direct trunk contact is a distinct, immutable branch root.
		for (const FWoodContact& Contact : Adjacency[TrunkComponentIndex])
		{
			const int32 BranchRootIndex = Contact.NeighborIndex;
			if (LockedBranchRoots[BranchRootIndex])
			{
				continue;
			}
			const FWoodComponentShape Shape = MeasureWoodComponentShape(
				Components[BranchRootIndex],
				Components[TrunkComponentIndex]);
			if (IsRootLikeTrunkContact(
					Shape,
					WoodBounds)
				|| IsTrunkStub(
					BranchRootIndex,
					TrunkComponentIndex,
					Components,
					Adjacency,
					Shape))
			{
				ProcessedComponents[BranchRootIndex] = true;
				OutParentComponentIndices[BranchRootIndex] =
					TrunkComponentIndex;
				continue;
			}
			LockedBranchRoots[BranchRootIndex] = true;
			BranchDistances[BranchRootIndex] = 0.0;
			OutParentComponentIndices[BranchRootIndex] = TrunkComponentIndex;
			OutBranchIDs[BranchRootIndex] = BranchCount;
			++BranchCount;
		}
		PropagateBranchIDs(
			Adjacency,
			TrunkComponentIndex,
			LockedBranchRoots,
			ProcessedComponents,
			BranchDistances,
			OutParentComponentIndices,
			OutBranchIDs);

		for (int32 ComponentIndex = 0;
			ComponentIndex < Adjacency.Num();
			++ComponentIndex)
		{
			if (ComponentIndex == TrunkComponentIndex
				|| ProcessedComponents[ComponentIndex]
				|| OutBranchIDs[ComponentIndex] != INDEX_NONE)
			{
				continue;
			}
			LockedBranchRoots[ComponentIndex] = true;
			BranchDistances[ComponentIndex] = 0.0;
			OutBranchIDs[ComponentIndex] = BranchCount;
			++BranchCount;
			PropagateBranchIDs(
				Adjacency,
				TrunkComponentIndex,
				LockedBranchRoots,
				ProcessedComponents,
				BranchDistances,
				OutParentComponentIndices,
				OutBranchIDs);
		}
		return BranchCount;
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

	double FindPointToCenterlineDistanceSquared(
		const FVector& Position,
		const TArray<FVector>& Centerline)
	{
		check(Centerline.Num() >= 2);
		double MinimumDistanceSquared = TNumericLimits<double>::Max();
		for (int32 PointIndex = 1; PointIndex < Centerline.Num(); ++PointIndex)
		{
			const FVector ClosestPoint = FMath::ClosestPointOnSegment(
				Position,
				Centerline[PointIndex - 1],
				Centerline[PointIndex]);
			MinimumDistanceSquared = FMath::Min(
				MinimumDistanceSquared,
				FVector::DistSquared(Position, ClosestPoint));
		}
		return MinimumDistanceSquared;
	}

	double MeasureMeanCenterlineDistance(
		const TArray<FVector>& Positions,
		const TArray<FVector>& Centerline)
	{
		check(!Positions.IsEmpty());
		check(Centerline.Num() >= 2);
		double DistanceSum = 0.0;
		for (const FVector& Position : Positions)
		{
			DistanceSum += FMath::Sqrt(
				FindPointToCenterlineDistanceSquared(Position, Centerline));
		}
		return DistanceSum / static_cast<double>(Positions.Num());
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

	bool IsCenterlineSemanticallyClear(
		const TArray<FVector>& Centerline,
		const FVector& Axis,
		const double AxialLength)
	{
		check(Centerline.Num() >= 2);
		check(AxialLength > MinimumGeometryScale);
		double TotalLength = 0.0;
		FVector PreviousDirection = FVector::ZeroVector;
		for (int32 PointIndex = 1; PointIndex < Centerline.Num(); ++PointIndex)
		{
			const FVector Segment =
				Centerline[PointIndex] - Centerline[PointIndex - 1];
			const double SegmentLength = Segment.Size();
			if (SegmentLength <= MinimumGeometryScale)
			{
				return false;
			}
			const FVector Direction = Segment / SegmentLength;
			if (FVector::DotProduct(Direction, Axis) <= 0.0)
			{
				return false;
			}
			if (!PreviousDirection.IsNearlyZero()
				&& FVector::DotProduct(PreviousDirection, Direction)
				< MinimumPreviewCenterlineDirectionDot)
			{
				return false;
			}
			PreviousDirection = Direction;
			TotalLength += SegmentLength;
		}
		return TotalLength <= AxialLength * MaximumPreviewCenterlineLengthRatio;
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

		const double SliceLengthScale = bIsTrunk
			? TrunkPreviewSliceLengthScale
			: PreviewSliceLengthScale;
		const int32 MaximumSliceCount = bIsTrunk
			? MaximumTrunkPreviewSliceCount
			: MaximumPreviewSliceCount;
		const double TargetSliceLength = FMath::Max(
			Component.GeometryScale * SliceLengthScale,
			MinimumGeometryScale);
		const int32 SliceCount = FMath::Clamp(
			FMath::CeilToInt(AxialLength / TargetSliceLength),
			2,
			MaximumSliceCount);
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
		const int32 SmoothingPassCount = bIsTrunk
			? TrunkPreviewCenterlineSmoothingPassCount
			: PreviewCenterlineSmoothingPassCount;
		SlicedCenterline = SmoothCenterline(
			SlicedCenterline,
			SmoothingPassCount);

		const double SimplificationScale = bIsTrunk
			? TrunkPreviewCenterlineSimplificationScale
			: PreviewCenterlineSimplificationScale;
		const double SimplificationLengthFraction = bIsTrunk
			? TrunkPreviewCenterlineSimplificationLengthFraction
			: PreviewCenterlineSimplificationLengthFraction;
		const double SimplificationTolerance = FMath::Max(
			Component.GeometryScale * SimplificationScale,
			AxialLength * SimplificationLengthFraction);
		TArray<FVector> CandidateCenterline = SimplifyCenterline(
			SlicedCenterline,
			SimplificationTolerance);
		if (bIsTrunk)
		{
			return CandidateCenterline;
		}

		const double PrincipalLineError = MeasureMeanCenterlineDistance(
			Component.Positions,
			PrincipalLine);
		const double CandidateError = MeasureMeanCenterlineDistance(
			Component.Positions,
			CandidateCenterline);
		if (IsCenterlineSemanticallyClear(
				CandidateCenterline,
				Axis,
				AxialLength)
			&& CandidateError
				< PrincipalLineError * PreviewCenterlineImprovementRatio)
		{
			return CandidateCenterline;
		}
		return PrincipalLine;
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

	FVector FindClosestPointOnCenterline(
		const FVector& Position,
		const TArray<FVector>& Centerline)
	{
		check(Centerline.Num() >= 2);
		FVector ClosestPoint = Centerline[0];
		double ClosestDistanceSquared = TNumericLimits<double>::Max();
		for (int32 PointIndex = 1; PointIndex < Centerline.Num(); ++PointIndex)
		{
			const FVector Candidate = FMath::ClosestPointOnSegment(
				Position,
				Centerline[PointIndex - 1],
				Centerline[PointIndex]);
			const double DistanceSquared = FVector::DistSquared(
				Position,
				Candidate);
			if (DistanceSquared < ClosestDistanceSquared)
			{
				ClosestDistanceSquared = DistanceSquared;
				ClosestPoint = Candidate;
			}
		}
		return ClosestPoint;
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
		const TArray<FWoodComponent>& WoodComponents,
		const int32 TrunkComponentIndex,
		const TArray<int32>& ParentComponentIndices,
		const TArray<int32>& BranchIDs,
		const FBox& WoodBounds,
		const int32 SourceLODIndex)
	{
		check(WoodComponents.IsValidIndex(TrunkComponentIndex));
		check(ParentComponentIndices.Num() == WoodComponents.Num());
		check(BranchIDs.Num() == WoodComponents.Num());

		TSharedRef<FFoliageBakerTreeHierarchyPreviewData> PreviewData =
			MakeShared<FFoliageBakerTreeHierarchyPreviewData>();
		PreviewData->AssetName = StaticMesh.GetName();
		PreviewData->Bounds = StaticMesh.GetBoundingBox();
		PreviewData->SourceStaticMesh = &StaticMesh;
		PreviewData->SourceLODIndex = SourceLODIndex;
		const double BranchRadius = FMath::Max(
			WoodBounds.GetSize().Size() * BranchPreviewRadiusTreeFraction,
			MinimumBranchPreviewRadius);
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
			if (WoodComponents.IsValidIndex(ParentIndex))
			{
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
		}

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
				PreviewData->Branches[PreviewBranchIndex].Color = FLinearColor(
					MakeBranchColor(StaticMesh, BranchID));
			}
			FFoliageBakerTreeHierarchyPreviewBranch& PreviewBranch =
				PreviewData->Branches[PreviewBranchIndex];
			AddCenterlineGeometry(
				ComponentCenterline,
				BranchRadius,
				PreviewBranch);

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
	for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
	{
		if (Triangles[TriangleIndex].MaterialIndex != LeafMaterialIndex)
		{
			WoodTriangleIndices.Add(TriangleIndex);
		}
	}
	const TArray<FTriangleComponent> WoodTriangleComponents =
		BuildTriangleComponents(Triangles, WoodTriangleIndices);
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
	const int32 TrunkComponentIndex = FindTrunkComponent(WoodComponents);
	const TArray<TArray<FWoodContact>> WoodContactGraph = BuildWoodContactGraph(
		WoodComponents,
		WoodBounds.GetSize().Size());
	TArray<int32> ParentComponentIndices;
	TArray<int32> BranchIDs;
	Result.BranchCount = AssignBranchIDs(
		WoodComponents,
		WoodBounds,
		WoodContactGraph,
		TrunkComponentIndex,
		ParentComponentIndices,
		BranchIDs);

	TArray<FVector4f> TriangleColors;
	TriangleColors.Init(FVector4f(0.0f, 0.0f, 0.0f, 1.0f), Triangles.Num());
	for (int32 ComponentIndex = 0;
		ComponentIndex < WoodComponents.Num();
		++ComponentIndex)
	{
		const FVector4f Color = ComponentIndex == TrunkComponentIndex
			|| BranchIDs[ComponentIndex] == INDEX_NONE
			? FVector4f(1.0f, 1.0f, 1.0f, 1.0f)
			: MakeBranchColor(StaticMesh, BranchIDs[ComponentIndex]);
		for (const int32 TriangleIndex : WoodComponents[ComponentIndex].TriangleIndices)
		{
			TriangleColors[TriangleIndex] = Color;
		}
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
	Result.PreviewData = BuildPreviewData(
		StaticMesh,
		WoodComponents,
		TrunkComponentIndex,
		ParentComponentIndices,
		BranchIDs,
		WoodBounds,
		SourceLODIndex);
	Result.CreatedAssets.Add(TStrongObjectPtr<UObject>(&StaticMesh));
	Result.Report = FString::Printf(
		TEXT("%s\n  wrote LOD %d hierarchy test colors: trunk/root/stub white, card foliage black, %d branch ID color(s)."),
		*StaticMesh.GetName(),
		SourceLODIndex,
		Result.BranchCount);
	return Result;
}
