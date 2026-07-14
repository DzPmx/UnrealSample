#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerPlaneCover.h"

struct FMeshDescription;

namespace UE::FoliageBaker::ProjectedMaterialBake
{
	struct FOLIAGEBAKERCORE_API FNormalBasisSample
	{
		FVector Normal = FVector::UpVector;
		FVector Tangent = FVector::ForwardVector;
		double CaptureDepth = TNumericLimits<double>::Max();
		float BinormalSign = 1.0f;
		float OutputNormalSign = 1.0f;
		bool bValid = false;
	};

	struct FOLIAGEBAKERCORE_API FPlaneSideBakeParams
	{
		FIntPoint TileSize = FIntPoint::ZeroValue;
		FVector CaptureRayDirection = FVector::ZeroVector;
		PlaneCover::EAtlasVConvention AtlasVConvention = PlaneCover::EAtlasVConvention::GeometryMinVToTextureMinV;
		int32 MaterialIndexFilter = INDEX_NONE;
		int32 NumSourceUVChannels = 0;
		bool bBackSide = false;
		bool bFlipTwoSidedBackFaceOutputNormals = false;
		bool bBuildNormalBasisMap = true;
	};

	/**
	 * Builds the projected material-bake mesh, custom tile UVs, and the matching
	 * source TBN basis map in one operation. Both outputs use the same sorted
	 * fragment order and the same effective winding. When requested, the final
	 * array records the original source-triangle index for every generated raster
	 * triangle in MeshDescription triangle order (including crack fan triangles).
	 * The basis map is optional through FPlaneSideBakeParams. A successful
	 * sub-pixel geometry build may also return an empty basis map when no pixel
	 * center is hit.
	 */
	FOLIAGEBAKERCORE_API bool BuildPlaneSideBakeInputs(
		const TArray<PlaneCover::FSourceTriangle>& Triangles,
		const TArray<int32>& TriangleIndices,
		const TArray<PlaneCover::FCrackReductionProjection>& CrackReductionProjections,
		const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
		const FPlaneSideBakeParams& Params,
		FMeshDescription& OutMeshDescription,
		TArray<FVector2D>& OutCustomTextureCoordinates,
		TArray<FNormalBasisSample>& OutNormalBasisMap,
		int32& OutMatchingTriangleCount,
		FString* OutError = nullptr,
		TArray<int32>* OutRasterSourceTriangleIndices = nullptr);

	FOLIAGEBAKERCORE_API FColor EncodeObjectSpaceNormalToColor(
		const FVector& InNormal,
		uint8 Alpha = 255);

	FOLIAGEBAKERCORE_API FColor EncodeBakedTangentSpaceNormalToObjectSpaceColor(
		const FColor& RawBakedTangentSpaceNormal,
		const FNormalBasisSample& Basis,
		uint8 AlphaOverride);
}
