#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureDefines.h"
#include "FoliageBakerPlaneCover.h"

class UStaticMesh;

namespace UE::FoliageBaker::MaterialResolver
{
	enum class EOpacityMaskChannel : uint8
	{
		Red,
		Green,
		Blue,
		Alpha
	};

	struct FOLIAGEBAKERCORE_API FTexturePixels
	{
		TArray64<uint8> Bytes;
		int32 Width = 0;
		int32 Height = 0;
		ETextureSourceFormat Format = TSF_Invalid;
		bool bLinearColor = false;

		bool IsValid() const;
		FColor SampleRawColor(const FVector2f& UV) const;
		FLinearColor Sample(const FVector2f& UV) const;
		void SetFromColors(const TArray<FColor>& Colors, const FIntPoint& Size, bool bInLinearColor);
		bool AlphaLooksLikeCutoutMask() const;
	};

	struct FOLIAGEBAKERCORE_API FMaterialOutputSelection
	{
		bool bBaseColorOpacity = true;
		bool bNormalMask = true;
		bool bMix = false;

		bool HasAnyOutput() const;
	};

	struct FOLIAGEBAKERCORE_API FMaterialScalarBakeData
	{
		float Constant = 0.0f;
		FTexturePixels Texture;
		bool bHasReadableTexture = false;
		bool bUseLuminance = false;
		EOpacityMaskChannel Channel = EOpacityMaskChannel::Red;
		FString Source;
	};

	struct FOLIAGEBAKERCORE_API FMaterialBakeData
	{
		FLinearColor BaseColor = FLinearColor::White;
		FTexturePixels BaseColorTexture;
		FTexturePixels NormalTexture;
		FTexturePixels OpacityMaskTexture;
		FMaterialScalarBakeData AmbientOcclusion;
		FMaterialScalarBakeData Roughness;
		FMaterialScalarBakeData Metallic;
		FMaterialScalarBakeData Emission;
		bool bHasReadableBaseColorTexture = false;
		bool bHasReadableNormalTexture = false;
		bool bHasReadableOpacityMaskTexture = false;
		bool bUseTextureAlphaAsOpacity = false;
		bool bTwoSided = false;
		bool bSourceTangentSpaceNormal = true;
		EOpacityMaskChannel OpacityMaskChannel = EOpacityMaskChannel::Alpha;
		float OpacityMaskClipValue = 0.33333334f;
		double OpacityMaskTransparentRatio = 0.0;
		FString OpacityMaskSource;
	};

	struct FOLIAGEBAKERCORE_API FMaterialResolveStats
	{
		int32 ReadableMaterialTextures = 0;
		int32 SourceMixTextureMaterials = 0;
		int32 TextureAlphaOpacityMaterials = 0;
		FString MaterialAlphaPolicyDetails;
	};

	struct FOLIAGEBAKERCORE_API FMaterialKeywordMatchResult
	{
		bool bEnabled = false;
		int32 MatchedMaterialCount = 0;
		TArray<uint8> MatchingMaterialFlags;

		bool IsMatch(int32 MaterialIndex) const;
	};

	FOLIAGEBAKERCORE_API float SampleOpacityMaskValue(
		const FTexturePixels& Texture,
		const FVector2f& UV,
		EOpacityMaskChannel Channel);

	FOLIAGEBAKERCORE_API FMaterialKeywordMatchResult ResolveMaterialKeywordMatches(
		const UStaticMesh& StaticMesh,
		const TArray<FString>& RawKeywords);

	FOLIAGEBAKERCORE_API TArray<FMaterialBakeData> ResolveMaterialBakeData(
		const UStaticMesh& SourceStaticMesh,
		int32 SourceLODIndex,
		const FBoxSphereBounds& SourceLODBounds,
		const TArray<PlaneCover::FSourceTriangle>& Triangles,
		const FMaterialOutputSelection& OutputSelection,
		int32 SourceMaterialBakeResolution,
		bool bBakeNormalTexture,
		FMaterialResolveStats& InOutStats);
}
