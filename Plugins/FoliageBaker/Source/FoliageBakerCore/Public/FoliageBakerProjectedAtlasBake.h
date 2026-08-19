#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerMaskedMaterialBaker.h"
#include "FoliageBakerMaterialResolver.h"
#include "FoliageBakerPlaneCover.h"

class UStaticMesh;

namespace UE::FoliageBaker::ProjectedAtlasBake
{
	enum class ENormalAlphaMode : uint8
	{
		TrunkLeafClassification,
		SourceDepth
	};

	enum class EInvalidMaterialPolicy : uint8
	{
		Fail,
		UseDefaultMaterial
	};

	struct FOLIAGEBAKERCORE_API FStats
	{
		int32 Width = 0;
		int32 Height = 0;
		int32 TileResolution = 0;
		int32 PaintedPixels = 0;
		int32 FrontTileCount = 0;
		int32 BackTileCount = 0;
		double PackedTileUtilizationPercent = 0.0;
		int32 RasterizedTriangleReferences = 0;
		int32 CrackReductionTriangleReferences = 0;
		int32 MaskedMaterialBakeReferences = 0;
		int32 AlphaAwareCroppedPlanes = 0;
		int32 AlphaAwareTileCropGuardPixels = 0;
		FString MaterialAlphaPolicyDetails;
		MaterialResolver::FTrunkLeafMaterialAverages MaterialAverages;
	};

	struct FOLIAGEBAKERCORE_API FInputs
	{
		FInputs(
			const UStaticMesh& InSourceStaticMesh,
			const FBoxSphereBounds& InSourceLODBounds,
			const FBoxSphereBounds& InFixedFrameWPOBounds,
			const TArray<PlaneCover::FSourceTriangle>& InTriangles,
			const FFoliageBakerBakeMaterialOverrideSet& InBakeMaterialOverrides,
			const TArray<PlaneCover::FPlaneProxyPlaneInfo>& InPlaneInfos,
			const PlaneCover::FPlaneProxyMeshStats& InProxyStats,
			const PlaneCover::FPlaneProxySettings& InSettings)
			: SourceStaticMesh(InSourceStaticMesh)
			, SourceLODBounds(InSourceLODBounds)
			, FixedFrameWPOBounds(InFixedFrameWPOBounds)
			, Triangles(InTriangles)
			, BakeMaterialOverrides(InBakeMaterialOverrides)
			, PlaneInfos(InPlaneInfos)
			, ProxyStats(InProxyStats)
			, Settings(InSettings)
		{
		}

		const UStaticMesh& SourceStaticMesh;
		FBoxSphereBounds SourceLODBounds;
		FBoxSphereBounds FixedFrameWPOBounds;
		const TArray<PlaneCover::FSourceTriangle>& Triangles;
		const FFoliageBakerBakeMaterialOverrideSet& BakeMaterialOverrides;
		const TArray<PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos;
		const PlaneCover::FPlaneProxyMeshStats& ProxyStats;
		const PlaneCover::FPlaneProxySettings& Settings;
	};

	struct FOLIAGEBAKERCORE_API FPolicy
	{
		MaterialResolver::FMaterialOutputSelection OutputSelection;
		ENormalAlphaMode NormalAlphaMode = ENormalAlphaMode::TrunkLeafClassification;
		EInvalidMaterialPolicy InvalidMaterialPolicy = EInvalidMaterialPolicy::Fail;
		bool bConvertNormalsToCaptureFrame = false;
		bool bCaptureSourceTriangleIdAndDepth = false;
		bool bIncludeCrackReductionForTrunkCards = true;
		FString DiagnosticName = TEXT("Projected atlas");
		FString MaterialAlphaPolicyDetails;
	};

	struct FOLIAGEBAKERCORE_API FResult
	{
		TArray<FColor> ColorAtlasPixels;
		TArray<FColor> NormalPixels;
		TArray<FColor> MixPixels;
		TArray<FColor> SourceTriangleIdAndDepthPixels;
		FStats Stats;
	};

	struct FOLIAGEBAKERCORE_API FTargetDensityAlphaCropStats
	{
		int32 CroppedPlaneCount = 0;
		int32 ResolutionLimitedPrepassPlaneCount = 0;
	};

	/**
	 * Bakes all plane sides through one shared masked-material depth competition.
	 * Feature modules only select the normal representation and crack policy.
	 */
	FOLIAGEBAKERCORE_API bool Bake(
		const FInputs& Inputs,
		const FPolicy& Policy,
		FResult& OutResult,
		FString& OutError);

	/**
	 * Detects alpha bounds one plane at a time at the configured target world
	 * texel density, independent of whether all target-density tiles fit one atlas.
	 * Front and back coverage share the same prepass tile so the crop is
	 * conservative for both sides.
	 */
	FOLIAGEBAKERCORE_API bool BuildTargetDensityAlphaAwareTileCrops(
		const FInputs& Inputs,
		const FPolicy& Policy,
		int32 GuardPixels,
		uint8 AlphaThreshold,
		TArray<PlaneCover::FPlaneProxyTileCrop>& OutTileCrops,
		FTargetDensityAlphaCropStats& OutStats,
		FString& OutError);
}
