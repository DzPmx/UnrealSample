#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
struct FMeshData;

/**
 * Renders projected material data through the source material's real masked
 * base-pass path. The output modes therefore use the real OpacityMask graph,
 * OpacityMaskClipValue, EarlyOpacityMask custom output and two-sided facing.
 *
 * This intentionally lives in FoliageBakerCore so all proxy generators can
 * share it without modifying Engine MaterialBaking sources.
 */
class FOLIAGEBAKERCORE_API FFoliageBakerMaskedMaterialBaker final
{
public:
	/** Returns the source material's evaluated BaseColor in sRGB bytes, clipped by the real source mask. */
	static bool BakeBaseColor(
		UMaterialInterface& MaterialInterface,
		const FMeshData& MeshSettings,
		const FIntPoint& TextureSize,
		const FColor& BackgroundColor,
		TArray<FColor>& OutBaseColor,
		FString* OutError = nullptr);

	static bool BakeFinalCoverage(
		UMaterialInterface& MaterialInterface,
		const FMeshData& MeshSettings,
		const FIntPoint& TextureSize,
		const FColor& BackgroundColor,
		TArray<FColor>& OutCoverage,
		FString* OutError = nullptr);

	/** Returns encoded object/local-space normals in RGB and preserves BackgroundColor for uncovered pixels. */
	static bool BakeObjectSpaceNormal(
		UMaterialInterface& MaterialInterface,
		const FMeshData& MeshSettings,
		const FIntPoint& TextureSize,
		const FColor& BackgroundColor,
		TArray<FColor>& OutNormals,
		FString* OutError = nullptr);

	/**
	 * Returns the 24-bit encoded, one-based original source-triangle index of
	 * the last surviving fragment in the supplied projected painter order.
	 * RGB=(0,0,0) is reserved for uncovered pixels.
	 */
	static bool BakeSourceTriangleId(
		UMaterialInterface& MaterialInterface,
		const FMeshData& MeshSettings,
		const TArray<int32>& RasterSourceTriangleIndices,
		const FIntPoint& TextureSize,
		TArray<FColor>& OutTriangleIds,
		FString* OutError = nullptr);

	/** Returns INDEX_NONE for the reserved uncovered value or malformed data. */
	static int32 DecodeSourceTriangleId(const FColor& EncodedTriangleId);
};
