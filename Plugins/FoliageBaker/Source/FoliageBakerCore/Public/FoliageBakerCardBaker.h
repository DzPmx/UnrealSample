#pragma once

#include "CoreMinimal.h"

class UMaterialInstanceConstant;
class UStaticMesh;
class UTexture2D;

enum class EFoliageBakerCardBakeMode : uint8
{
	SingleBillboard,
	CrossCards
};

enum class EFoliageBakerCaptureAxis : uint8
{
	PositiveX,
	NegativeX,
	PositiveY,
	NegativeY
};

struct FOLIAGEBAKERCORE_API FFoliageBakerCardBakeRequest
{
	UStaticMesh* SourceStaticMesh = nullptr;
	UMaterialInstanceConstant* MaterialTemplate = nullptr;
	int32 SourceLODIndex = 0;

	EFoliageBakerCardBakeMode Mode = EFoliageBakerCardBakeMode::SingleBillboard;
	EFoliageBakerCaptureAxis SingleCaptureAxis = EFoliageBakerCaptureAxis::PositiveX;
	int32 CrossCardPlaneCount = 2;
	TArray<FString> TrunkMaterialKeywords = { TEXT("Trunk") };

	int32 TextureResolution = 1024;
	int32 AlphaCropGuardPixels = 2;
	int32 OpacitySdfRangePixels = 16;
	bool bTrimUnusedAtlasSpace = false;
	bool bBakeBaseColorOpacity = true;
	bool bBakeNormalDepth = true;
	bool bBakeMix = false;
	FName BaseColorOpacityTextureParameterName = TEXT("ColorOpacity");
	FName NormalDepthTextureParameterName = TEXT("NormalMask");
	FName MixTextureParameterName = TEXT("Mix");

	FString TextureOutputFolderName = TEXT("Textures");
	FString MaterialOutputFolderName = TEXT("Materials");
	FString TextureNamePrefix = TEXT("T_");
	FString BaseColorOpacityTextureSuffix = TEXT("_DA");
	FString NormalDepthTextureSuffix = TEXT("_NR");
	FString MixTextureSuffix = TEXT("_M");
	FString MaterialInstanceNamePrefix = TEXT("MI_");
	FString MaterialInstanceNameSuffix;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerCardBakeResult
{
	bool bSucceeded = false;
	bool bCancelled = false;
	UStaticMesh* ProxyMesh = nullptr;
	int32 SourceMeshLODIndex = INDEX_NONE;
	UTexture2D* ColorOpacityTexture = nullptr;
	UTexture2D* NormalDepthTexture = nullptr;
	UTexture2D* MixTexture = nullptr;
	UMaterialInstanceConstant* MaterialInstance = nullptr;
	TArray<UObject*> CreatedAssets;
	FString Report;
};


class FOLIAGEBAKERCORE_API FFoliageBakerCardBaker final
{
public:
	static FFoliageBakerCardBakeResult Bake(const FFoliageBakerCardBakeRequest& Request);
};
