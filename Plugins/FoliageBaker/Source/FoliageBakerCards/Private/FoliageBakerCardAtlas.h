#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerProjectedAtlasBake.h"
#include "FoliageBakerProxyGeometry.h"

namespace UE::FoliageBaker::Cards::Atlas
{
	enum class EOuterCropMode : uint8
	{
		TightBlockAligned,
		PowerOfTwoUsedBounds
	};

	int32 MergeTwoViewTileCrops(
		TArray<PlaneCover::FPlaneProxyTileCrop>& TileCrops);

	int32 MergeGroupedTileCrops(
		TArray<PlaneCover::FPlaneProxyTileCrop>& TileCrops,
		const TArray<int32>& PlaneGroupIndices);

	bool CropToUsedSpace(
		FFoliageBakerProxyGeometry& InOutGeometry,
		TArray<FColor>& ColorAtlasPixels,
		TArray<FColor>& NormalPixels,
		TArray<FColor>& MixPixels,
		ProjectedAtlasBake::FStats& InOutStats,
		EOuterCropMode CropMode,
		FString& OutError);
}
