#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerPlaneCover.h"

namespace UE::FoliageBaker::Atlas
{
	FOLIAGEBAKERCORE_API uint8 EncodeTrunkLeafAlpha(bool bIsTrunk);

	FOLIAGEBAKERCORE_API void NormalizeEncodedObjectSpaceNormals(TArray<FColor>& Pixels);

	// Assign every atlas pixel to one non-overlapping tile. Pixels outside all
	// tile interiors belong to the tile that supplies their infinite padding.
	FOLIAGEBAKERCORE_API bool BuildTileOwnerMap(
		int32 Width,
		int32 Height,
		const TArray<FIntRect>& TileRects,
		TArray<uint16>& OutTileOwners);

	FOLIAGEBAKERCORE_API bool ResizeTileIsolated(
		const TArray<FColor>& SourcePixels,
		int32 SourceWidth,
		int32 SourceHeight,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& SourcePlaneInfos,
		int32 RequestedMaximumDimension,
		FColor BackgroundColor,
		TArray<FColor>& OutPixels,
		int32& OutWidth,
		int32& OutHeight,
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& OutPlaneInfos,
		FString& OutError);

	FOLIAGEBAKERCORE_API void FillTransparentRGBInsideTiles(
		TArray<FColor>& Pixels,
		int32 Width,
		int32 Height,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		const TBitArray<>* CoverageMask = nullptr,
		bool bFillAlpha = false);

	FOLIAGEBAKERCORE_API void WriteUnionSdfToAlpha(
		TArray<FColor>& Pixels,
		int32 Width,
		int32 Height,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		const TBitArray<>& CoverageMask,
		int32 SdfRangePixels);


	FOLIAGEBAKERCORE_API int32 BuildAlphaAwareTileCrops(
		const TArray<FColor>& Pixels,
		int32 Width,
		int32 Height,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		int32 GuardPixels,
		uint8 AlphaThreshold,
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop>& OutTileCrops);
}
