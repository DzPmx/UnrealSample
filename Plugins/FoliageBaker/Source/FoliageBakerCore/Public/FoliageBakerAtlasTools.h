#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerPlaneCover.h"

namespace UE::FoliageBaker::Atlas
{

	FOLIAGEBAKERCORE_API void NormalizeEncodedObjectSpaceNormals(TArray<FColor>& Pixels);


	FOLIAGEBAKERCORE_API void FillTransparentRGBInsideTiles(
		TArray<FColor>& Pixels,
		int32 Width,
		int32 Height,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		const TBitArray<>* CoverageMask = nullptr,
		bool bFillAlpha = false);


	FOLIAGEBAKERCORE_API int32 BuildAlphaAwareTileCrops(
		const TArray<FColor>& Pixels,
		int32 Width,
		int32 Height,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		int32 GuardPixels,
		uint8 AlphaThreshold,
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop>& OutTileCrops);
}
