#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerMeshOutput.h"
#include "FoliageBakerPlaneCover.h"
#include "FoliageBakerProxyGeometry.h"

class UMaterialInterface;
class UStaticMesh;

namespace UE::FoliageBaker::Cards::Geometry
{
	struct FRetainedTrunkResult
	{
		TArray<FFoliageBakerMeshMaterialSlot> MaterialSlots;
		int32 OriginalTriangleCount = 0;
		int32 ReducedTriangleCount = 0;
		int32 UVChannelCount = 0;
	};

	bool AppendReducedTrunk(
		const UStaticMesh& StaticMesh,
		const TArray<PlaneCover::FSourceTriangle>& TrunkTriangles,
		float TrianglePercentage,
		UMaterialInterface* ProxyMaterial,
		FMeshDescription& InOutMeshDescription,
		FRetainedTrunkResult& OutResult,
		FString& OutError);

	bool BuildDoublePlanesOutput(
		const FVector& OutputNormal,
		const PlaneCover::FPlaneProxySettings& PlaneSettings,
		const FFoliageBakerProxyGeometry& CaptureGeometry,
		FMeshDescription& OutMeshDescription,
		PlaneCover::FPlaneProxyMeshStats& OutStats,
		FString& OutError);

	bool BuildMultiBillboardOutput(
		const TArray<int32>& PlaneGroupIndices,
		const TArray<FVector>& ClusterCenters,
		const PlaneCover::FPlaneProxySettings& PlaneSettings,
		const FFoliageBakerProxyGeometry& CaptureGeometry,
		FMeshDescription& OutMeshDescription,
		PlaneCover::FPlaneProxyMeshStats& OutStats,
		FString& OutError);
}
