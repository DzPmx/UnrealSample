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
		TArray<FColor>& BaseColorOpacityPixels,
		TArray<FColor>& NormalPixels,
		TArray<FColor>& MixPixels,
		TArray<FColor>& SourceTriangleIdAndDepthPixels,
		ProjectedAtlasBake::FStats& InOutStats,
		EOuterCropMode CropMode,
		FString& OutError);

	bool ResizeTileIsolated(
		const TArray<FColor>& SourcePixels,
		const ProjectedAtlasBake::FStats& SourceStats,
		const TArray<PlaneCover::FPlaneProxyPlaneInfo>& SourcePlaneInfos,
		int32 RequestedMaximumDimension,
		FColor BackgroundColor,
		TArray<FColor>& OutPixels,
		ProjectedAtlasBake::FStats& OutStats,
		TArray<PlaneCover::FPlaneProxyPlaneInfo>& OutPlaneInfos,
		FString& OutError);
}
