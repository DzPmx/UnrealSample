#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerPlaneCover.h"

namespace UE::FoliageBaker::Cards::MultiBillboardLayout
{
	struct FComponent
	{
		TArray<int32> TriangleIndices;
		FVector Center = FVector::ZeroVector;
		double Area = 0.0;
	};

	struct FCluster
	{
		TArray<int32> ComponentIndices;
		FVector Center = FVector::ZeroVector;
		double Area = 0.0;
	};

	struct FLayer
	{
		TArray<int32> TriangleIndices;
		double Rho = 0.0;
		double Area = 0.0;
	};

	TArray<FComponent> BuildConnectedLeafComponents(
		const TArray<PlaneCover::FSourceTriangle>& Triangles,
		double PositionTolerance);

	TArray<FCluster> ClusterLeafComponents(
		const TArray<FComponent>& Components,
		int32 RequestedClusterCount);

	TArray<FLayer> BuildClusterLayers(
		const TArray<FComponent>& Components,
		const FCluster& Cluster,
		const FVector& CaptureNormal,
		int32 RequestedLayerCount);
}
