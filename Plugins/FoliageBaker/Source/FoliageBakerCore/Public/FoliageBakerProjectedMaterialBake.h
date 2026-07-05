#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerPlaneCover.h"

struct FMeshDescription;

namespace UE::FoliageBaker::ProjectedMaterialBake
{
	struct FOLIAGEBAKERCORE_API FPlaneSideBakeParams
	{
		FVector CaptureRayDirection = FVector::ZeroVector;
		PlaneCover::EAtlasVConvention AtlasVConvention = PlaneCover::EAtlasVConvention::GeometryMinVToTextureMinV;
		TOptional<int32> MaterialIndexFilter;
		int32 NumSourceUVChannels = 0;
		bool bBackSide = false;
	};

	FOLIAGEBAKERCORE_API bool ComputeGpuWinnerBarycentric2D(
		const FVector2D& Point,
		const FVector2D& A,
		const FVector2D& B,
		const FVector2D& C,
		double& OutA,
		double& OutB,
		double& OutC);

	FOLIAGEBAKERCORE_API bool BuildPlaneSideBakeInputs(
		const TArray<PlaneCover::FSourceTriangle>& Triangles,
		const TArray<int32>& TriangleIndices,
		const TArray<PlaneCover::FCrackReductionProjection>& CrackReductionProjections,
		const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
		const FPlaneSideBakeParams& Params,
		FMeshDescription& OutMeshDescription,
		TArray<FVector2D>& OutCustomTextureCoordinates,
		int32& OutMatchingTriangleCount,
		FString& OutError,
		TArray<int32>& OutRasterSourceTriangleIndices);

	FOLIAGEBAKERCORE_API FColor EncodeObjectSpaceNormalToColor(
		const FVector& InNormal,
		uint8 Alpha = 255);
}
