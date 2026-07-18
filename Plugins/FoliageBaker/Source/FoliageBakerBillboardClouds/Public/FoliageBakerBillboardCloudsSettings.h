#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "FoliageBakerBillboardCloudsSettings.generated.h"

class UMaterialInstanceConstant;
class UStaticMesh;

UENUM()
enum class EBillboardCloudsCrackReductionMode : uint8
{
	Off UMETA(DisplayName = "Off"),
	ScaledEnvelopeClip UMETA(DisplayName = "Scaled Envelope-Clipped Projection")
};

UENUM()
enum class EBillboardCloudsDoubleSidedBakeMode : uint8
{
	Off UMETA(DisplayName = "Off"),
	TrunkCardsOnly UMETA(DisplayName = "Trunk Cards Only"),
	BillboardPlanesOnly UMETA(DisplayName = "Billboard Planes Only"),
	AllPlanes UMETA(DisplayName = "All Planes")
};

UENUM()
enum class EBillboardCloudsTrunkCardAtlasScale : uint8
{
	HalfX UMETA(DisplayName = "0.5x"),
	OneX UMETA(DisplayName = "1.0x"),
	OnePointFiveX UMETA(DisplayName = "1.5x"),
	TwoX UMETA(DisplayName = "2.0x")
};

UCLASS(config = EditorPerProjectUserSettings, Transient, PrioritizeCategories = ("Mesh", "Feature", "Asset", "Material"), meta = (DisplayName = "Foliage Baker - Billboard Clouds"))
class FOLIAGEBAKERBILLBOARDCLOUDS_API UFoliageBakerBillboardCloudsSettings : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(EditAnywhere, Category = "Mesh", meta = (ToolTip = "Static Mesh assets processed from the selected Source LOD when Bake is clicked. Add assets here directly or use Add Content Browser Selection in the BillboardClouds panel."))
	TArray<TObjectPtr<UStaticMesh>> SourceStaticMeshes;

	UPROPERTY(config, EditAnywhere, Category = "Mesh", meta = (ClampMin = "0", ClampMax = "7", DisplayName = "Source LOD Index", ToolTip = "Source Static Mesh LOD used for geometry extraction, plane-cover bounds, material baking, and shared depth encoding. Every queued mesh must contain this LOD."))
	int32 SourceLODIndex = 0;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Plane Cover", meta = (ClampMin = "0.0", ToolTip = "Relative object-space tolerance used by K-Means proxy footprint padding and minimum extent. The analyzer multiplies this by the selected mesh bounds sphere radius."))
	double RelativeError = 0.02;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Plane Cover", meta = (ClampMin = "0.0", ToolTip = "Minimum K-Means proxy footprint tolerance in Unreal centimeters."))
	double MinimumErrorCm = 1.0;

	UPROPERTY(config, EditAnywhere, Category = "Feature|K-Means", meta = (ClampMin = "1", ClampMax = "512", ToolTip = "Target number of billboard planes for budget-driven K-Means clustering."))
	int32 KMeansPlaneCount = 64;

	UPROPERTY(config, EditAnywhere, Category = "Feature|K-Means", meta = (ClampMin = "1", ClampMax = "512", ToolTip = "Maximum assignment/refit iterations for the K-Means solver."))
	int32 KMeansMaxIterations = 512;

	UPROPERTY(config, EditAnywhere, Category = "Feature|K-Means", meta = (ToolTip = "Crack reduction for K-Means. Scaled Envelope-Clipped Projection clips cross-plane projection fragments against a scaled neighbor envelope before GPU material baking."))
	EBillboardCloudsCrackReductionMode KMeansCrackReductionMode = EBillboardCloudsCrackReductionMode::ScaledEnvelopeClip;

	UPROPERTY(config, EditAnywhere, Category = "Feature|K-Means", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "KMeansCrackReductionMode == EBillboardCloudsCrackReductionMode::ScaledEnvelopeClip", EditConditionHides, ToolTip = "Scale applied to the K-Means crack-reduction envelope before clipping cross-plane projection fragments. 1.0 keeps the full envelope; smaller values reduce overfill on alpha-card foliage."))
	double KMeansCrackReductionProjectionScale = 0.75;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Trunk Cards", meta = (ToolTip = "Route matching trunk/branch material triangles into a fixed vertical cross-card pass instead of K-Means clustering."))
	bool bEnableTrunkCards = true;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Trunk Cards", meta = (ClampMin = "2", ClampMax = "8", EditCondition = "bEnableTrunkCards", EditConditionHides, ToolTip = "Number of evenly spaced vertical trunk cross-card planes, from 2 to 8."))
	int32 TrunkCardPlaneCount = 4;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Trunk Cards", meta = (EditCondition = "bEnableTrunkCards", EditConditionHides, ToolTip = "Resolution weight for trunk cross-card atlas tiles during packing. Values below 1 reduce trunk tile resolution; values above 1 increase it while reducing space available to foliage billboard tiles."))
	EBillboardCloudsTrunkCardAtlasScale TrunkCardAtlasScale = EBillboardCloudsTrunkCardAtlasScale::OneX;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Trunk Cards", meta = (ToolTip = "Material instance or parent material name keywords used to classify ColorOpacity alpha as trunk (0.5). When Trunk Cards is enabled, the same matches are routed into fixed vertical trunk cross-card planes. Empty means every visible pixel is classified as leaf (1)."))
	TArray<FString> TrunkCardMaterialKeywords = { TEXT("Trunk") };

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "256", ClampMax = "4096", ToolTip = "Generated square atlas resolution. Billboard tile sizes are automatically scaled and packed to maximize use of this texture."))
	int32 TextureAtlasResolution = 2048;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ToolTip = "Run a pre-bake alpha pass and crop each billboard tile rectangle to the alpha-painted outer bounds before final packing. This improves per-tile usage by removing transparent outer borders; it does not fill interior alpha holes."))
	bool bEnableAlphaAwareTileCrop = true;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "2", ClampMax = "16", EditCondition = "bEnableAlphaAwareTileCrop", EditConditionHides, ToolTip = "Extra source-tile pixels kept around the alpha-painted bounds when alpha-aware tile crop is enabled."))
	int32 AlphaAwareTileCropGuardPixels = 2;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ToolTip = "Bake a separate back-side atlas tile for selected proxy planes. The material uses TwoSidedSign to sample UV0 on front faces and UV1 on back faces."))
	EBillboardCloudsDoubleSidedBakeMode DoubleSidedBakeMode = EBillboardCloudsDoubleSidedBakeMode::AllPlanes;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Atlas Outputs", meta = (ToolTip = "Generate the BaseColor/Opacity atlas. RGB stores base color and A stores visible source classification: background 0, trunk 0.5, leaf 1."))
	bool bBakeBaseColorOpacityAtlas = true;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Atlas Outputs", meta = (ToolTip = "Generate the Normal/Depth atlas. RGB stores object/local-space normal and A stores shared-range linear depth; the destination material parameter is configured in Material."))
	bool bBakeNormalMaskAtlas = true;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Atlas Outputs", meta = (ToolTip = "Generate the packed material atlas. RGBA stores Occlusion, Roughness, Metallic, and Emission; the destination material parameter is configured in Material."))
	bool bBakeMixAtlas = false;

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (ToolTip = "Texture output folder relative to the parent of the source Static Mesh folder. For a mesh in /Game/Trees/Meshes, the default creates textures in /Game/Trees/Textures."))
	FString TextureOutputFolderName = TEXT("Textures");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (ToolTip = "Material instance output folder relative to the parent of the source Static Mesh folder. For a mesh in /Game/Trees/Meshes, the default creates materials in /Game/Trees/Materials."))
	FString MaterialOutputFolderName = TEXT("Materials");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (ToolTip = "Prefix added before the source Static Mesh name for every generated atlas texture."))
	FString TextureNamePrefix = TEXT("T_");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (ToolTip = "Suffix added to the generated BaseColor/Opacity atlas texture."))
	FString BaseColorOpacityTextureSuffix = TEXT("_BillboardClouds_DA");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (ToolTip = "Suffix added to the generated object-space normal atlas texture."))
	FString NormalTextureSuffix = TEXT("_BillboardClouds_NR");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (ToolTip = "Suffix added to the generated packed Mix atlas texture."))
	FString MixTextureSuffix = TEXT("_BillboardClouds_M");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (ToolTip = "Prefix added before the source Static Mesh name for the generated material instance."))
	FString MaterialInstanceNamePrefix = TEXT("MI_");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (ToolTip = "Optional suffix added after the source Static Mesh name for the generated material instance."))
	FString MaterialInstanceNameSuffix = TEXT("_BillboardClouds");

	UPROPERTY(config, EditDefaultsOnly, Category = "Material", meta = (DisplayName = "Parent Material Instance", ToolTip = "Configured only in Editor Preferences. Generated proxy materials are new child Material Instance Constants whose Parent is this instance."))
	TSoftObjectPtr<UMaterialInstanceConstant> BillboardMaterialTemplate;

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Base Color / Opacity Parameter", ToolTip = "Texture parameter receiving generated BaseColor RGB and trunk/leaf opacity classification in A."))
	FName BaseColorOpacityTextureParameterName = TEXT("ColorOpacity");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Normal / Depth Parameter", ToolTip = "Texture parameter receiving the generated object/local-space Normal and shared-depth atlas when that output is enabled."))
	FName NormalDepthTextureParameterName = TEXT("NormalMask");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Mix Parameter", ToolTip = "Texture parameter receiving the generated Occlusion/Roughness/Metallic/Emission atlas when that output is enabled."))
	FName MixTextureParameterName = TEXT("Mix");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (EditCondition = "!bBakeMixAtlas", DisplayName = "Leaf Roughness Parameter", ToolTip = "Scalar parameter receiving the average baked Roughness of visible leaf pixels when Mix output is disabled and valid leaf pixels exist."))
	FName LeafRoughnessParameterName = TEXT("LeafRoughness");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (EditCondition = "!bBakeMixAtlas", DisplayName = "Leaf Specular Parameter", ToolTip = "Scalar parameter receiving the average baked Specular of visible leaf pixels when Mix output is disabled and valid leaf pixels exist."))
	FName LeafSpecularParameterName = TEXT("LeafSpecular");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (EditCondition = "!bBakeMixAtlas", DisplayName = "Trunk Roughness Parameter", ToolTip = "Scalar parameter receiving the average baked Roughness of visible trunk pixels when Mix output is disabled and valid trunk pixels exist."))
	FName TrunkRoughnessParameterName = TEXT("TrunkRoughness");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (EditCondition = "!bBakeMixAtlas", DisplayName = "Trunk Specular Parameter", ToolTip = "Scalar parameter receiving the average baked Specular of visible trunk pixels when Mix output is disabled and valid trunk pixels exist."))
	FName TrunkSpecularParameterName = TEXT("TrunkSpecular");
};
