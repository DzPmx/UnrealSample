#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerMaskedMaterialBaker.h"
#include "FoliageBakerPlaneCover.h"

class UStaticMesh;

namespace UE::FoliageBaker::L1Visibility
{
	bool BakeUpperHemisphere(
		const UStaticMesh& SourceStaticMesh,
		const FBoxSphereBounds& PrimitiveBounds,
		const FBoxSphereBounds& SourceBounds,
		const TArray<PlaneCover::FSourceTriangle>& Triangles,
		const TArray<PlaneCover::FSourceTriangle>& BakeTriangles,
		const FFoliageBakerBakeMaterialOverrideSet& BakeMaterialOverrides,
		const TArray<PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		const PlaneCover::FPlaneProxySettings& Settings,
		const TArray<FColor>& SourceTriangleIdAndDepthPixels,
		int32 AtlasWidth,
		int32 AtlasHeight,
		int32 RequestedSampleCount,
		int32 ShadowMapResolution,
		TArray<FColor>& OutPixels,
		FString& OutError);
}
