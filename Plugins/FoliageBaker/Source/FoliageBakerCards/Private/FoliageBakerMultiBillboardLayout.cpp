#include "FoliageBakerMultiBillboardLayout.h"

namespace UE::FoliageBaker::Cards::MultiBillboardLayout
{
	namespace
	{
		struct FQuantizedPositionKey
		{
			int64 X = 0;
			int64 Y = 0;
			int64 Z = 0;

			bool operator==(const FQuantizedPositionKey& Other) const
			{
				return X == Other.X && Y == Other.Y && Z == Other.Z;
			}

			friend uint32 GetTypeHash(const FQuantizedPositionKey& Key)
			{
				return HashCombineFast(
					HashCombineFast(::GetTypeHash(Key.X), ::GetTypeHash(Key.Y)),
					::GetTypeHash(Key.Z));
			}
		};

		int32 FindComponentRoot(TArray<int32>& Parents, int32 Index)
		{
			int32 Root = Index;
			while (Parents[Root] != Root)
			{
				Root = Parents[Root];
			}
			while (Parents[Index] != Index)
			{
				const int32 Parent = Parents[Index];
				Parents[Index] = Root;
				Index = Parent;
			}
			return Root;
		}

		void UnionComponents(TArray<int32>& Parents, TArray<uint8>& Ranks, int32 A, int32 B)
		{
			int32 RootA = FindComponentRoot(Parents, A);
			int32 RootB = FindComponentRoot(Parents, B);
			if (RootA == RootB)
			{
				return;
			}
			if (Ranks[RootA] < Ranks[RootB])
			{
				Swap(RootA, RootB);
			}
			Parents[RootB] = RootA;
			if (Ranks[RootA] == Ranks[RootB])
			{
				++Ranks[RootA];
			}
		}
	}

	TArray<FComponent> BuildConnectedLeafComponents(
		const TArray<PlaneCover::FSourceTriangle>& Triangles,
		const double PositionTolerance)
	{
		TArray<FComponent> Components;
		if (Triangles.IsEmpty())
		{
			return Components;
		}

		TArray<int32> Parents;
		TArray<uint8> Ranks;
		Parents.SetNumUninitialized(Triangles.Num());
		Ranks.SetNumZeroed(Triangles.Num());
		for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
		{
			Parents[TriangleIndex] = TriangleIndex;
		}

		const double SafeTolerance = FMath::Max(PositionTolerance, 1.0e-6);
		const double InverseTolerance = 1.0 / SafeTolerance;
		TMap<FQuantizedPositionKey, int32> FirstTriangleByPosition;
		FirstTriangleByPosition.Reserve(Triangles.Num() * 3);
		for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
		{
			for (const FVector& Vertex : Triangles[TriangleIndex].Vertices)
			{
				const FQuantizedPositionKey Key{
					FMath::RoundToInt64(Vertex.X * InverseTolerance),
					FMath::RoundToInt64(Vertex.Y * InverseTolerance),
					FMath::RoundToInt64(Vertex.Z * InverseTolerance)
				};
				if (const int32* ExistingTriangle = FirstTriangleByPosition.Find(Key))
				{
					UnionComponents(Parents, Ranks, TriangleIndex, *ExistingTriangle);
				}
				else
				{
					FirstTriangleByPosition.Add(Key, TriangleIndex);
				}
			}
		}

		TMap<int32, int32> ComponentIndexByRoot;
		ComponentIndexByRoot.Reserve(Triangles.Num());
		for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
		{
			const int32 Root = FindComponentRoot(Parents, TriangleIndex);
			int32 ComponentIndex = INDEX_NONE;
			if (const int32* ExistingComponentIndex = ComponentIndexByRoot.Find(Root))
			{
				ComponentIndex = *ExistingComponentIndex;
			}
			else
			{
				ComponentIndex = Components.AddDefaulted();
				ComponentIndexByRoot.Add(Root, ComponentIndex);
			}
			FComponent& Component = Components[ComponentIndex];
			const PlaneCover::FSourceTriangle& Triangle = Triangles[TriangleIndex];
			const FVector TriangleCenter =
				(Triangle.Vertices[0] + Triangle.Vertices[1] + Triangle.Vertices[2]) / 3.0;
			const double TriangleArea = FMath::Max(Triangle.Area, UE_DOUBLE_SMALL_NUMBER);
			Component.TriangleIndices.Add(TriangleIndex);
			Component.Center += TriangleCenter * TriangleArea;
			Component.Area += TriangleArea;
		}

		for (FComponent& Component : Components)
		{
			Component.Center /= FMath::Max(Component.Area, UE_DOUBLE_SMALL_NUMBER);
		}
		return Components;
	}

	TArray<FCluster> ClusterLeafComponents(
		const TArray<FComponent>& Components,
		const int32 RequestedClusterCount)
	{
		TArray<FCluster> Clusters;
		if (Components.IsEmpty())
		{
			return Clusters;
		}

		const int32 ClusterCount = FMath::Clamp(RequestedClusterCount, 1, Components.Num());
		TArray<FVector> Centers;
		Centers.Reserve(ClusterCount);
		int32 FirstCenterIndex = 0;
		for (int32 ComponentIndex = 1; ComponentIndex < Components.Num(); ++ComponentIndex)
		{
			if (Components[ComponentIndex].Area > Components[FirstCenterIndex].Area)
			{
				FirstCenterIndex = ComponentIndex;
			}
		}
		Centers.Add(Components[FirstCenterIndex].Center);

		while (Centers.Num() < ClusterCount)
		{
			int32 FarthestComponentIndex = INDEX_NONE;
			double FarthestDistanceSquared = -1.0;
			for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ++ComponentIndex)
			{
				double NearestDistanceSquared = TNumericLimits<double>::Max();
				for (const FVector& Center : Centers)
				{
					NearestDistanceSquared = FMath::Min(
						NearestDistanceSquared,
						FVector::DistSquared(Components[ComponentIndex].Center, Center));
				}
				if (NearestDistanceSquared > FarthestDistanceSquared)
				{
					FarthestDistanceSquared = NearestDistanceSquared;
					FarthestComponentIndex = ComponentIndex;
				}
			}
			if (FarthestComponentIndex == INDEX_NONE)
			{
				break;
			}
			Centers.Add(Components[FarthestComponentIndex].Center);
		}

		TArray<int32> Assignments;
		Assignments.Init(INDEX_NONE, Components.Num());
		constexpr int32 ClusteringIterations = 32;
		for (int32 Iteration = 0; Iteration < ClusteringIterations; ++Iteration)
		{
			bool bAssignmentChanged = false;
			for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ++ComponentIndex)
			{
				int32 NearestClusterIndex = 0;
				double NearestDistanceSquared =
					FVector::DistSquared(Components[ComponentIndex].Center, Centers[0]);
				for (int32 ClusterIndex = 1; ClusterIndex < Centers.Num(); ++ClusterIndex)
				{
					const double DistanceSquared =
						FVector::DistSquared(Components[ComponentIndex].Center, Centers[ClusterIndex]);
					if (DistanceSquared < NearestDistanceSquared)
					{
						NearestDistanceSquared = DistanceSquared;
						NearestClusterIndex = ClusterIndex;
					}
				}
				if (Assignments[ComponentIndex] != NearestClusterIndex)
				{
					Assignments[ComponentIndex] = NearestClusterIndex;
					bAssignmentChanged = true;
				}
			}

			TArray<FVector> WeightedCenterSums;
			TArray<double> WeightSums;
			WeightedCenterSums.Init(FVector::ZeroVector, Centers.Num());
			WeightSums.Init(0.0, Centers.Num());
			for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ++ComponentIndex)
			{
				const int32 ClusterIndex = Assignments[ComponentIndex];
				const double Weight = FMath::Max(Components[ComponentIndex].Area, UE_DOUBLE_SMALL_NUMBER);
				WeightedCenterSums[ClusterIndex] += Components[ComponentIndex].Center * Weight;
				WeightSums[ClusterIndex] += Weight;
			}
			for (int32 ClusterIndex = 0; ClusterIndex < Centers.Num(); ++ClusterIndex)
			{
				if (WeightSums[ClusterIndex] > 0.0)
				{
					Centers[ClusterIndex] = WeightedCenterSums[ClusterIndex] / WeightSums[ClusterIndex];
				}
			}
			if (!bAssignmentChanged)
			{
				break;
			}
		}

		Clusters.SetNum(Centers.Num());
		for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ++ComponentIndex)
		{
			FCluster& Cluster = Clusters[Assignments[ComponentIndex]];
			const FComponent& Component = Components[ComponentIndex];
			const double Weight = FMath::Max(Component.Area, UE_DOUBLE_SMALL_NUMBER);
			Cluster.ComponentIndices.Add(ComponentIndex);
			Cluster.Center += Component.Center * Weight;
			Cluster.Area += Weight;
		}
		Clusters.RemoveAll([](const FCluster& Cluster)
		{
			return Cluster.ComponentIndices.IsEmpty();
		});
		for (FCluster& Cluster : Clusters)
		{
			Cluster.Center /= FMath::Max(Cluster.Area, UE_DOUBLE_SMALL_NUMBER);
		}
		return Clusters;
	}

	TArray<FLayer> BuildClusterLayers(
		const TArray<FComponent>& Components,
		const FCluster& Cluster,
		const FVector& CaptureNormal,
		const int32 RequestedLayerCount)
	{
		TArray<FLayer> Layers;
		if (Cluster.ComponentIndices.IsEmpty())
		{
			return Layers;
		}

		const int32 LayerCount = FMath::Clamp(
			RequestedLayerCount,
			1,
			Cluster.ComponentIndices.Num());
		double MinDepth = TNumericLimits<double>::Max();
		double MaxDepth = -TNumericLimits<double>::Max();
		for (const int32 ComponentIndex : Cluster.ComponentIndices)
		{
			const double Depth = FVector::DotProduct(Components[ComponentIndex].Center, CaptureNormal);
			MinDepth = FMath::Min(MinDepth, Depth);
			MaxDepth = FMath::Max(MaxDepth, Depth);
		}

		const double DepthRange = MaxDepth - MinDepth;
		constexpr double DepthEpsilon = 1.0e-6;
		const int32 EffectiveLayerCount = DepthRange > DepthEpsilon ? LayerCount : 1;
		Layers.SetNum(EffectiveLayerCount);
		TArray<double> WeightedDepthSums;
		WeightedDepthSums.Init(0.0, EffectiveLayerCount);
		for (const int32 ComponentIndex : Cluster.ComponentIndices)
		{
			const FComponent& Component = Components[ComponentIndex];
			const double Depth = FVector::DotProduct(Component.Center, CaptureNormal);
			const double NormalizedDepth = DepthRange > DepthEpsilon
				? FMath::Clamp((Depth - MinDepth) / DepthRange, 0.0, 1.0)
				: 0.0;
			const int32 LayerIndex = FMath::Min(
				FMath::FloorToInt(NormalizedDepth * static_cast<double>(EffectiveLayerCount)),
				EffectiveLayerCount - 1);
			FLayer& Layer = Layers[LayerIndex];
			const double Weight = FMath::Max(Component.Area, UE_DOUBLE_SMALL_NUMBER);
			Layer.TriangleIndices.Append(Component.TriangleIndices);
			Layer.Area += Weight;
			WeightedDepthSums[LayerIndex] += Depth * Weight;
		}

		for (int32 LayerIndex = Layers.Num() - 1; LayerIndex >= 0; --LayerIndex)
		{
			FLayer& Layer = Layers[LayerIndex];
			if (Layer.TriangleIndices.IsEmpty())
			{
				Layers.RemoveAt(LayerIndex);
				WeightedDepthSums.RemoveAt(LayerIndex);
				continue;
			}
			Layer.Rho =
				WeightedDepthSums[LayerIndex]
				/ FMath::Max(Layer.Area, UE_DOUBLE_SMALL_NUMBER);
		}
		return Layers;
	}
}
