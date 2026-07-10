#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "BillboardCloudsEditorSettings.generated.h"

class UMaterialInstanceConstant;
class UStaticMesh;

UENUM()
enum class EBillboardCloudsTechnique : uint8
{
	PlaneSpaceGreedy UMETA(DisplayName = "Paper 1 - Plane-Space Greedy Cover"),
	KMeansClustering UMETA(DisplayName = "Paper 2 - K-Means Best-Fit Planes"),
	GodOfWarCards UMETA(DisplayName = "God of War - Greedy Card Capture")
};

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
	OnePointFiveX UMETA(DisplayName = "1.5x"),
	TwoX UMETA(DisplayName = "2.0x")
};

UENUM()
enum class EBillboardCloudsMeshOutputMode : uint8
{
	SeparateMeshAsset UMETA(DisplayName = "Create Separate Mesh Asset"),
	AddToSourceMeshLOD UMETA(DisplayName = "Add To Source Mesh LODs"),
	ReplaceSourceMeshLOD UMETA(DisplayName = "Replace Source Mesh LOD")
};

UCLASS(config = EditorPerProjectUserSettings, defaultconfig, Transient, meta = (DisplayName = "Billboard Clouds"))
class BILLBOARDCLOUDSEDITOR_API UBillboardCloudsEditorSettings : public UObject
{
	GENERATED_BODY()

public:
	UBillboardCloudsEditorSettings();

	UPROPERTY(EditAnywhere, Category = "Input", meta = (ToolTip = "Static Mesh assets processed when Bake is clicked. Add assets here directly or use Add Content Browser Selection in the Billboard Clouds tool panel."))
	TArray<TObjectPtr<UStaticMesh>> SourceStaticMeshes;

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (ToolTip = "Texture output folder relative to the parent of the source Static Mesh folder. For a mesh in /Game/Trees/Meshes, the default creates textures in /Game/Trees/Textures."))
	FString TextureOutputFolderName = TEXT("Textures");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (ToolTip = "Material instance output folder relative to the parent of the source Static Mesh folder. For a mesh in /Game/Trees/Meshes, the default creates materials in /Game/Trees/Materials."))
	FString MaterialOutputFolderName = TEXT("Materials");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (ToolTip = "Prefix added before the source Static Mesh name for every generated atlas texture."))
	FString TextureNamePrefix = TEXT("T_");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (ToolTip = "Suffix added to the generated BaseColor/Opacity atlas texture."))
	FString BaseColorOpacityTextureSuffix = TEXT("_DA");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (ToolTip = "Suffix added to the generated object-space normal atlas texture."))
	FString NormalTextureSuffix = TEXT("_NR");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (ToolTip = "Suffix added to the generated packed Mix atlas texture."))
	FString MixTextureSuffix = TEXT("_M");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (ToolTip = "Prefix added before the source Static Mesh name for the generated material instance."))
	FString MaterialInstanceNamePrefix = TEXT("MI_");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (ToolTip = "Optional suffix added after the source Static Mesh name for the generated material instance."))
	FString MaterialInstanceNameSuffix;

	UPROPERTY(config, EditAnywhere, Category = "Mesh Output", meta = (ToolTip = "Choose whether generated proxy geometry is written as a separate Static Mesh asset, appended as a new LOD on the selected source Static Mesh, or used to replace an existing source Static Mesh LOD. Texture and material locations are controlled by the Asset settings."))
	EBillboardCloudsMeshOutputMode MeshOutputMode = EBillboardCloudsMeshOutputMode::SeparateMeshAsset;

	UPROPERTY(config, EditAnywhere, Category = "Mesh Output", meta = (ClampMin = "0", EditCondition = "MeshOutputMode == EBillboardCloudsMeshOutputMode::ReplaceSourceMeshLOD", EditConditionHides, ToolTip = "Source Static Mesh LOD index to replace when Mesh Output Mode is Replace Source Mesh LOD. The LOD must already exist; the tool will not create missing replacement LODs."))
	int32 ReplaceSourceLODIndex = 1;

	UPROPERTY(config, EditAnywhere, Category = "Technique", meta = (ToolTip = "Plane-space greedy cover follows the first paper. K-means clustering follows the improved second paper. God of War card capture greedily picks the best card until all faces are claimed."))
	EBillboardCloudsTechnique Technique = EBillboardCloudsTechnique::KMeansClustering;

	UPROPERTY(config, EditAnywhere, Category = "Plane Cover", meta = (ClampMin = "0.0", ToolTip = "Relative object-space error. The analyzer multiplies this by the selected mesh bounds sphere radius. Plane-space and God of War cards use it as the cover/closeness tolerance; K-Means uses it for generated proxy footprint padding and minimum extent."))
	double RelativeError = 0.02;

	UPROPERTY(config, EditAnywhere, Category = "Plane Cover", meta = (ClampMin = "0.0", ToolTip = "Minimum object-space error in Unreal centimeters. This is the lower bound for the derived tolerance used by plane-space cover, God of War card closeness, and K-Means proxy footprint padding."))
	double MinimumErrorCm = 1.0;

	UPROPERTY(config, EditAnywhere, Category = "Plane Space", meta = (ClampMin = "4", EditCondition = "Technique == EBillboardCloudsTechnique::PlaneSpaceGreedy", EditConditionHides, ToolTip = "Number of azimuth samples used in plane-space normal discretization."))
	int32 NormalThetaSteps = 16;

	UPROPERTY(config, EditAnywhere, Category = "Plane Space", meta = (ClampMin = "3", EditCondition = "Technique == EBillboardCloudsTechnique::PlaneSpaceGreedy", EditConditionHides, ToolTip = "Number of polar samples used in plane-space normal discretization."))
	int32 NormalPhiSteps = 9;

	UPROPERTY(config, EditAnywhere, Category = "Plane Space", meta = (ClampMin = "8", EditCondition = "Technique == EBillboardCloudsTechnique::PlaneSpaceGreedy", EditConditionHides, ToolTip = "Number of rho bins used for each sampled normal direction."))
	int32 RhoBinCount = 256;

	UPROPERTY(config, EditAnywhere, Category = "Plane Space", meta = (ClampMin = "0", ClampMax = "16", EditCondition = "Technique == EBillboardCloudsTechnique::PlaneSpaceGreedy", EditConditionHides, ToolTip = "Safety cap for the paper's recursive adaptive refinement after picking the densest plane-space bin. Refinement still stops early when the sub-bin center plane is valid for the whole sub-bin validity set."))
	int32 AdaptiveRefinementDepth = 10;

	UPROPERTY(config, EditAnywhere, Category = "K-Means", meta = (ClampMin = "1", ClampMax = "4096", EditCondition = "Technique == EBillboardCloudsTechnique::KMeansClustering", EditConditionHides, ToolTip = "Target number of billboard planes for the second paper's budget-driven k-means clustering."))
	int32 KMeansPlaneCount = 64;

	UPROPERTY(config, EditAnywhere, Category = "K-Means", meta = (ClampMin = "1", ClampMax = "512", EditCondition = "Technique == EBillboardCloudsTechnique::KMeansClustering", EditConditionHides, ToolTip = "Maximum assignment/refit iterations for the second paper's k-means solver."))
	int32 KMeansMaxIterations = 512;

	UPROPERTY(config, EditAnywhere, Category = "K-Means", meta = (EditCondition = "Technique == EBillboardCloudsTechnique::KMeansClustering", EditConditionHides, ToolTip = "Crack reduction for K-Means. Scaled Envelope-Clipped Projection clips cross-plane projection fragments against a scaled neighbor envelope before GPU material baking."))
	EBillboardCloudsCrackReductionMode KMeansCrackReductionMode = EBillboardCloudsCrackReductionMode::ScaledEnvelopeClip;

	UPROPERTY(config, EditAnywhere, Category = "K-Means", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "Technique == EBillboardCloudsTechnique::KMeansClustering && KMeansCrackReductionMode == EBillboardCloudsCrackReductionMode::ScaledEnvelopeClip", EditConditionHides, ToolTip = "Scale applied to the K-Means crack-reduction envelope before clipping cross-plane projection fragments. 1.0 keeps the full envelope; smaller values reduce overfill on alpha-card foliage."))
	double KMeansCrackReductionProjectionScale = 0.75;

	UPROPERTY(config, EditAnywhere, Category = "God of War Cards", meta = (ClampMin = "0", ClampMax = "5", EditCondition = "Technique == EBillboardCloudsTechnique::GodOfWarCards", EditConditionHides, ToolTip = "Subdivisions of the geodesic sphere used for half-sphere card directions. Higher values test more directions."))
	int32 GodOfWarGeodesicSubdivisions = 3;

	UPROPERTY(config, EditAnywhere, Category = "God of War Cards", meta = (ClampMin = "0.1", ClampMax = "8.0", EditCondition = "Technique == EBillboardCloudsTechnique::GodOfWarCards", EditConditionHides, ToolTip = "Distance spacing between candidate card planes as a multiplier of the closeness/error metric."))
	double GodOfWarCandidateSpacingMultiplier = 1.0;

	UPROPERTY(config, EditAnywhere, Category = "Trunk Cards", meta = (ToolTip = "Route matching trunk/branch material triangles into a fixed vertical cross-card pass instead of the selected Billboard Clouds technique."))
	bool bEnableTrunkCards = true;

	UPROPERTY(config, EditAnywhere, Category = "Trunk Cards", meta = (ClampMin = "2", ClampMax = "4", EditCondition = "bEnableTrunkCards", EditConditionHides, ToolTip = "Number of vertical trunk cross-card planes. 2 is a cross card, 3 is a three-way star, 4 is a four-way star."))
	int32 TrunkCardPlaneCount = 4;

	UPROPERTY(config, EditAnywhere, Category = "Trunk Cards", meta = (EditCondition = "bEnableTrunkCards", EditConditionHides, ToolTip = "Resolution weight for trunk cross-card atlas tiles during packing. The atlas resolution stays fixed, so larger trunk tiles reduce space available to foliage billboard tiles."))
	EBillboardCloudsTrunkCardAtlasScale TrunkCardAtlasScale = EBillboardCloudsTrunkCardAtlasScale::OnePointFiveX;

	UPROPERTY(config, EditAnywhere, Category = "Trunk Cards", meta = (EditCondition = "bEnableTrunkCards", EditConditionHides, ToolTip = "Material instance or parent material name keywords used to route triangles into fixed vertical trunk cross-card planes instead of the selected Billboard Clouds technique. Empty means no triangles are routed."))
	TArray<FString> TrunkCardMaterialKeywords = { TEXT("Trunk") };

	UPROPERTY(config, EditAnywhere, Category = "Texture", meta = (ClampMin = "0.0", ClampMax = "8.0", EditCondition = "Technique == EBillboardCloudsTechnique::PlaneSpaceGreedy", EditConditionHides, ToolTip = "Paper Section 6.1 compactness weight. Higher values favor spatially compact validity sets and compact clusters."))
	double TextureCompactnessWeight = 0.25;

	UPROPERTY(config, EditAnywhere, Category = "Texture", meta = (ClampMin = "0", ClampMax = "128", ToolTip = "Transparent gutter around each billboard tile, in pixels."))
	int32 TextureTilePaddingPixels = 16;

	UPROPERTY(config, EditAnywhere, Category = "Texture", meta = (ClampMin = "256", ClampMax = "8192", ToolTip = "Generated square atlas resolution. Billboard tile sizes are automatically scaled and packed to maximize use of this texture."))
	int32 TextureAtlasResolution = 2048;

	UPROPERTY(config, EditAnywhere, Category = "Texture", meta = (ClampMin = "256", ClampMax = "8192", ToolTip = "Resolution used by the fallback/evaluation path for source-material UV-space data, mainly opacity fallback and alpha analysis. Final atlas channels are baked per billboard tile."))
	int32 SourceMaterialBakeResolution = 2048;

	UPROPERTY(config, EditAnywhere, Category = "Texture", meta = (ToolTip = "Run a pre-bake alpha pass and crop each billboard tile rectangle to the alpha-painted outer bounds before final packing. This improves per-tile usage by removing transparent outer borders; it does not fill interior alpha holes."))
	bool bEnableAlphaAwareTileCrop = true;

	UPROPERTY(config, EditAnywhere, Category = "Texture", meta = (ClampMin = "0", ClampMax = "16", EditCondition = "bEnableAlphaAwareTileCrop", EditConditionHides, ToolTip = "Extra source-tile pixels kept around the alpha-painted bounds when alpha-aware tile crop is enabled."))
	int32 AlphaAwareTileCropGuardPixels = 2;

	UPROPERTY(config, EditAnywhere, Category = "Texture", meta = (ToolTip = "Bake a separate back-side atlas tile for selected proxy planes. The material uses TwoSidedSign to sample UV0 on front faces and UV1 on back faces."))
	EBillboardCloudsDoubleSidedBakeMode DoubleSidedBakeMode = EBillboardCloudsDoubleSidedBakeMode::TrunkCardsOnly;

	UPROPERTY(config, EditAnywhere, Category = "Texture|Atlas Outputs", meta = (ToolTip = "Generate and assign the BaseColor/Opacity atlas to the ColorOpacity texture parameter. RGB stores base color, A stores the opacity mask."))
	bool bBakeBaseColorOpacityAtlas = true;

	UPROPERTY(config, EditAnywhere, Category = "Texture|Atlas Outputs", meta = (ToolTip = "Generate and assign the object-space normal atlas to the NormalMask texture parameter."))
	bool bBakeNormalMaskAtlas = true;

	UPROPERTY(config, EditAnywhere, Category = "Texture|Atlas Outputs", meta = (ToolTip = "Generate and assign the packed material mask atlas to the Mix texture parameter. RGBA stores Occlusion, Roughness, Metallic, Emission."))
	bool bBakeMixAtlas = false;

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (ToolTip = "Template material instance copied for every generated proxy. Enabled atlas outputs are assigned to ColorOpacity, NormalMask, and Mix texture parameters."))
	TSoftObjectPtr<UMaterialInstanceConstant> BillboardMaterialTemplate;
};
