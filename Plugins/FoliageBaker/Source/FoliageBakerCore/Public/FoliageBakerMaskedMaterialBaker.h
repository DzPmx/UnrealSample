#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
struct FMeshData;

struct FOLIAGEBAKERCORE_API FFoliageBakerDepthCorrectTileMaterialInput
{
	UMaterialInterface* MaterialInterface = nullptr;
	const FMeshData* MeshSettings = nullptr;
	const TArray<int32>* RasterSourceTriangleIndices = nullptr;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerDepthCorrectTileRequest
{
	FIntPoint TextureSize = FIntPoint::ZeroValue;
	FVector CaptureRayDirection = FVector::ZeroVector;
	FBoxSphereBounds SourceBounds = FBoxSphereBounds(ForceInitToZero);
	TArray<FFoliageBakerDepthCorrectTileMaterialInput> Materials;
	bool bBakeBaseColor = true;
	bool bBakeObjectSpaceNormal = true;
	bool bBakePackedMix = false;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerDepthCorrectTileResult
{
	TArray<FColor> BaseColor;
	TArray<FColor> ObjectSpaceNormal;
	TArray<FColor> PackedMix;
	TArray<FColor> SourceTriangleIdAndDepth;
};

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
	static bool BakeDepthCorrectTile(
		const FFoliageBakerDepthCorrectTileRequest& Request,
		FFoliageBakerDepthCorrectTileResult& OutResult,
		FString* OutError = nullptr);

	/** Returns INDEX_NONE for the reserved uncovered value or malformed data. */
	static int32 DecodeSourceTriangleId(const FColor& EncodedTriangleId);
};
