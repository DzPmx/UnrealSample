#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureDefines.h"
#include "FoliageBakerMaterialResolver.h"
#include "FoliageBakerPlaneCover.h"

class FFoliageBakerAssetTransaction;
class UStaticMesh;
class UTexture2D;

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
			const TArray<PlaneCover::FSourceTriangle>& InTriangles,
			const TArray<PlaneCover::FPlaneProxyPlaneInfo>& InPlaneInfos,
			const PlaneCover::FPlaneProxyMeshStats& InProxyStats,
			const PlaneCover::FPlaneProxySettings& InSettings)
			: SourceStaticMesh(InSourceStaticMesh)
			, SourceLODBounds(InSourceLODBounds)
			, Triangles(InTriangles)
			, PlaneInfos(InPlaneInfos)
			, ProxyStats(InProxyStats)
			, Settings(InSettings)
		{
		}

		const UStaticMesh& SourceStaticMesh;
		FBoxSphereBounds SourceLODBounds;
		const TArray<PlaneCover::FSourceTriangle>& Triangles;
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
		TArray<FColor> BaseColorOpacityPixels;
		TArray<FColor> NormalPixels;
		TArray<FColor> MixPixels;
		TArray<FColor> SourceTriangleIdAndDepthPixels;
		FStats Stats;
	};

	struct FOLIAGEBAKERCORE_API FTextureAssetRequest
	{
		FString OutputFolderName;
		FString OutputPackagePathOverride;
		FString AssetNamePrefix;
		FString AssetNameSuffix;
		FColor MipBackgroundColor = FColor(0, 0, 0, 0);
		TextureCompressionSettings CompressionSettings = TC_Default;
		TextureGroup LODGroup = TEXTUREGROUP_World;
		bool bSRGB = true;
		float SemanticMaskMipCoverageThreshold = 0.0f;
		FString EmptyPixelsError = TEXT("No atlas pixels were generated.");
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

	/** Creates a texture whose mip levels keep every packed atlas tile isolated. */
	FOLIAGEBAKERCORE_API UTexture2D* CreateTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FTextureAssetRequest& Request,
		const TArray<FColor>& Pixels,
		const FStats& Stats,
		const TArray<PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError);
}
