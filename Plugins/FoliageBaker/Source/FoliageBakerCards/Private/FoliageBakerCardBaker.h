#pragma once

#include "CoreMinimal.h"

class UMaterialInstanceConstant;
class UStaticMesh;
class UTexture2D;

enum class EFoliageBakerCardBakeMode : uint8
{
	SingleBillboard,
	CrossCards,
	MultiBillboard
};

enum class EFoliageBakerCaptureAxis : uint8
{
	PositiveX,
	NegativeX,
	PositiveY,
	NegativeY
};

enum class EFoliageBakerBillboardPlaneMode : uint8
{
	SinglePlane,
	DoublePlanes
};

enum class EFoliageBakerCrossCardGeometryMode : uint8
{
	TwoSidedTwoUVs,
	SeparateOneSidedFaces
};

struct FFoliageBakerCardBakeRequest
{
	UStaticMesh* SourceStaticMesh = nullptr;
	UMaterialInstanceConstant* MaterialTemplate = nullptr;
	int32 SourceLODIndex = 0;

	EFoliageBakerCardBakeMode Mode = EFoliageBakerCardBakeMode::SingleBillboard;
	EFoliageBakerBillboardPlaneMode BillboardPlaneMode = EFoliageBakerBillboardPlaneMode::SinglePlane;
	EFoliageBakerCaptureAxis SingleCaptureAxis = EFoliageBakerCaptureAxis::PositiveX;
	int32 CrossCardPlaneCount = 2;
	EFoliageBakerCrossCardGeometryMode CrossCardGeometryMode = EFoliageBakerCrossCardGeometryMode::TwoSidedTwoUVs;
	TArray<FString> TrunkMaterialKeywords = { TEXT("Trunk") };
	TArray<FString> LeafMaterialKeywords = { TEXT("Leaf") };
	int32 MultiBillboardClusterCount = 16;
	int32 MultiBillboardsPerCluster = 3;
	bool bIncludeReducedTrunk = true;
	float TrunkTrianglePercentage = 0.5f;

	int32 TextureResolution = 1024;
	int32 AlphaCropGuardPixels = 2;
	bool bPreserveAlphaMaskValues = true;
	float MipMaskCoverageThreshold = 0.35f;
	bool bTrimUnusedAtlasSpace = false;
	bool bBakeBaseColorOpacity = true;
	bool bBakeNormalDepth = true;
	bool bBakeMix = false;
	bool bBakeUpperHemisphereL1Visibility = false;
	int32 UpperHemisphereL1TextureResolution = 512;
	int32 UpperHemisphereL1SampleCount = 12;
	int32 UpperHemisphereL1ShadowMapResolution = 1024;
	FName BaseColorOpacityTextureParameterName = TEXT("ColorOpacity");
	FName NormalDepthTextureParameterName = TEXT("NormalMask");
	FName MixTextureParameterName = TEXT("Mix");
	FName UpperHemisphereL1VisibilityTextureParameterName = TEXT("UpperHemisphereL1Visibility");
	FName LeafRoughnessParameterName = TEXT("LeafRoughness");
	FName LeafSpecularParameterName = TEXT("LeafSpecular");
	FName TrunkRoughnessParameterName = TEXT("TrunkRoughness");
	FName TrunkSpecularParameterName = TEXT("TrunkSpecular");

	FString TextureOutputFolderName = TEXT("Textures");
	FString MaterialOutputFolderName = TEXT("Materials");
	bool bPlaceGeneratedAssetsNearReplacedLODAssets = true;
	FString TextureNamePrefix = TEXT("T_");
	FString BaseColorOpacityTextureSuffix = TEXT("_DA");
	FString NormalDepthTextureSuffix = TEXT("_NR");
	FString MixTextureSuffix = TEXT("_M");
	FString UpperHemisphereL1VisibilityTextureSuffix = TEXT("_L1V");
	FString MaterialInstanceNamePrefix = TEXT("MI_");
	FString MaterialInstanceNameSuffix;
};

struct FFoliageBakerCardBakeResult
{
	bool bSucceeded = false;
	bool bCancelled = false;
	UStaticMesh* ProxyMesh = nullptr;
	int32 SourceMeshLODIndex = INDEX_NONE;
	UTexture2D* ColorOpacityTexture = nullptr;
	UTexture2D* NormalDepthTexture = nullptr;
	UTexture2D* MixTexture = nullptr;
	UTexture2D* UpperHemisphereL1VisibilityTexture = nullptr;
	UMaterialInstanceConstant* MaterialInstance = nullptr;
	TArray<UObject*> CreatedAssets;
	FString Report;
};


class FFoliageBakerCardBaker final
{
public:
	static FFoliageBakerCardBakeResult Bake(const FFoliageBakerCardBakeRequest& Request);
};
