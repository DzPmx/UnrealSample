#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerMaskedMaterialBaker.h"
#include "FoliageBakerTextureResolution.h"
#include "UObject/Object.h"

#include "FoliageBakerCardsSettings.generated.h"

class UMaterialInstanceConstant;
class UStaticMesh;

UENUM()
enum class EFoliageBakerCardMode : uint8
{
	SingleBillboard UMETA(DisplayName = "Single Billboard"),
	CrossCards UMETA(DisplayName = "Cross Cards"),
	MultiBillboard UMETA(DisplayName = "MultiBillboard")
};

UENUM()
enum class EFoliageBakerSingleCaptureAxis : uint8
{
	PositiveX UMETA(DisplayName = "+X"),
	NegativeX UMETA(DisplayName = "-X"),
	PositiveY UMETA(DisplayName = "+Y"),
	NegativeY UMETA(DisplayName = "-Y")
};

UENUM()
enum class EFoliageBakerBillboardMode : uint8
{
	SinglePlane UMETA(DisplayName = "Single Plane"),
	DoublePlanes UMETA(DisplayName = "Double Planes")
};

UENUM()
enum class EFoliageBakerCrossCardFaceMode : uint8
{
	TwoSidedTwoUVs UMETA(DisplayName = "Two-Sided (UV0 / UV1)"),
	SeparateOneSidedFaces UMETA(DisplayName = "Separate One-Sided Faces")
};

UCLASS(config = EditorPerProjectUserSettings, Transient, PrioritizeCategories = ("Mesh", "Feature", "Asset", "Material"), meta = (DisplayName = "Foliage Baker - Billboard & Cross Cards"))
class FOLIAGEBAKERCARDS_API UFoliageBakerCardsSettings : public UObject
{
	GENERATED_BODY()

public:
	UFoliageBakerCardsSettings();

	UPROPERTY(Transient, EditAnywhere, Category = "Mesh", meta = (ToolTip = "Static Mesh assets baked from the selected Source LOD. Add assets directly or use Add Content Browser Selection."))
	TArray<TObjectPtr<UStaticMesh>> SourceStaticMeshes;

	UPROPERTY(config, EditAnywhere, Category = "Mesh", meta = (ClampMin = "0", ClampMax = "7", DisplayName = "Source LOD Index", ToolTip = "Source Static Mesh LOD used for geometry extraction, projected bounds, material baking, and trunk/leaf classification. Every queued mesh must contain this LOD."))
	int32 SourceLODIndex = 0;


	UPROPERTY(config)
	EFoliageBakerCardMode Mode = EFoliageBakerCardMode::SingleBillboard;

	UPROPERTY(config, EditAnywhere, Category = "Feature", meta = (DisplayName = "Billboard Mode", EditCondition = "Mode == EFoliageBakerCardMode::SingleBillboard", EditConditionHides, ToolTip = "Single Plane captures one view and emits one camera-facing plane. Double Planes captures the selected primary axis plus a second horizontal axis rotated 90 degrees around local +Z, then emits two parallel camera-facing planes for angle-based material blending."))
	EFoliageBakerBillboardMode BillboardMode = EFoliageBakerBillboardMode::SinglePlane;

	UPROPERTY(config, EditAnywhere, Category = "Feature", meta = (DisplayName = "Capture Axis", EditCondition = "Mode == EFoliageBakerCardMode::SingleBillboard || Mode == EFoliageBakerCardMode::MultiBillboard", EditConditionHides, ToolTip = "Camera is placed on the selected local axis and looks toward the source geometry. Double Planes derives its second capture direction by rotating this axis 90 degrees around local +Z. MultiBillboard uses this axis as the shared normal and depth-slicing direction for every cluster's parallel Billboard stack."))
	EFoliageBakerSingleCaptureAxis SingleCaptureAxis = EFoliageBakerSingleCaptureAxis::PositiveX;

	UPROPERTY(config, EditAnywhere, Category = "Feature", meta = (ClampMin = "2", ClampMax = "5", EditCondition = "Mode == EFoliageBakerCardMode::CrossCards", EditConditionHides, ToolTip = "Number of vertical planes distributed evenly over 180 degrees. Every plane is baked from both sides, and all planes intersect on the vertical axis through the source Static Mesh local origin (asset pivot)."))
	int32 CrossCardPlaneCount = 2;

	UPROPERTY(config, EditAnywhere, Category = "Feature", meta = (DisplayName = "Cross Card Face Mode", EditCondition = "Mode == EFoliageBakerCardMode::CrossCards", EditConditionHides, ToolTip = "Two-Sided keeps one quad per direction and stores front/back atlas tiles in UV0/UV1. Separate One-Sided Faces emits an oppositely wound quad for each side, stores that side's tile in UV0, and forces the generated material instance to be one-sided."))
	EFoliageBakerCrossCardFaceMode CrossCardFaceMode = EFoliageBakerCrossCardFaceMode::TwoSidedTwoUVs;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Trunk Leaf Mask", meta = (DisplayName = "Trunk Material Keywords", EditCondition = "Mode != EFoliageBakerCardMode::MultiBillboard", EditConditionHides, ToolTip = "Same classification rule as BillboardClouds: source material instance or parent material names containing any keyword are trunk; all other visible source triangles are leaf. Empty means all visible pixels are leaf."))
	TArray<FString> TrunkMaterialKeywords = { TEXT("Trunk") };

	UPROPERTY(config, EditAnywhere, Category = "Feature|Leaf Selection", meta = (DisplayName = "Leaf Material Keywords", EditCondition = "Mode == EFoliageBakerCardMode::MultiBillboard", EditConditionHides, ToolTip = "Source material instance or parent material names containing any keyword are included as leaf geometry. Non-matching triangles, including trunk and branches, are excluded from Billboard clustering and atlas baking; Include Reduced Trunk can retain and simplify them as normal mesh geometry. At least one keyword must be configured."))
	TArray<FString> LeafMaterialKeywords = { TEXT("Leaf") };

	UPROPERTY(config, EditAnywhere, Category = "Feature|Clustering", meta = (ClampMin = "1", ClampMax = "128", DisplayName = "Cluster Count", EditCondition = "Mode == EFoliageBakerCardMode::MultiBillboard", EditConditionHides, ToolTip = "Maximum number of spatial leaf clusters. Connected leaf components are clustered in source-local 3D space; the actual count cannot exceed the number of connected components."))
	int32 MultiBillboardClusterCount = 16;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Clustering", meta = (ClampMin = "2", ClampMax = "8", DisplayName = "Billboards Per Cluster", EditCondition = "Mode == EFoliageBakerCardMode::MultiBillboard", EditConditionHides, ToolTip = "Maximum number of parallel depth layers generated inside each spatial leaf cluster. Connected leaf components are assigned to layers by their center depth along Capture Axis. Empty layers are omitted."))
	int32 MultiBillboardsPerCluster = 3;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Trunk", meta = (DisplayName = "Include Reduced Trunk", EditCondition = "Mode == EFoliageBakerCardMode::MultiBillboard", EditConditionHides, ToolTip = "Keeps all non-leaf source geometry, simplifies it with Unreal Engine's standard Static Mesh reducer, preserves its source materials and UVs, and merges it with the generated leaf Billboards."))
	bool bIncludeReducedTrunk = true;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Trunk", meta = (ClampMin = "0.05", ClampMax = "1.0", UIMin = "0.05", UIMax = "1.0", DisplayName = "Trunk Triangle Percentage", EditCondition = "Mode == EFoliageBakerCardMode::MultiBillboard && bIncludeReducedTrunk", EditConditionHides, ToolTip = "Fraction of non-leaf source triangles retained by the trunk simplifier. 0.5 targets roughly half of the original trunk and branch triangles."))
	float TrunkTrianglePercentage = 0.5f;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (DisplayName = "Resolution Mode", ToolTip = "Auto chooses the smallest power-of-two atlas that reaches the requested world-space texel density. Manual preserves the configured atlas resolution behavior."))
	EFoliageBakerTextureResolutionMode TextureResolutionMode =
		EFoliageBakerTextureResolutionMode::AutoWorldTexelSize;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "0.01", UIMin = "10.0", UIMax = "1000.0", Suffix = "texels/m", DisplayName = "Target Texels Per Meter", EditCondition = "TextureResolutionMode == EFoliageBakerTextureResolutionMode::AutoWorldTexelSize", EditConditionHides, ToolTip = "Requested atlas texels per source-local meter. Larger values produce higher texture detail and may require a larger atlas."))
	double TargetTexelsPerMeter = 20.0;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "64", ClampMax = "4096", DisplayName = "Minimum Atlas Resolution", EditCondition = "TextureResolutionMode == EFoliageBakerTextureResolutionMode::AutoWorldTexelSize", EditConditionHides, ToolTip = "Smallest power-of-two square packing canvas Auto mode may select before optional outer atlas cropping. Non-power-of-two values are rounded up."))
	int32 MinimumTextureAtlasResolution = 64;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "64", ClampMax = "4096", DisplayName = "Maximum Atlas Resolution", EditCondition = "Mode == EFoliageBakerCardMode::SingleBillboard", EditConditionHides, ToolTip = "Maximum permitted atlas resolution. Manual mode scales tiles to use this limit; Auto mode stops increasing resolution at this limit."))
	int32 SingleTextureResolution = 4096;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "64", ClampMax = "4096", DisplayName = "Maximum Atlas Resolution", EditCondition = "Mode == EFoliageBakerCardMode::CrossCards", EditConditionHides, ToolTip = "Maximum permitted atlas resolution. Manual mode scales tiles to use this limit; Auto mode stops increasing resolution at this limit."))
	int32 CrossTextureResolution = 4096;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "64", ClampMax = "4096", DisplayName = "Maximum Atlas Resolution", EditCondition = "Mode == EFoliageBakerCardMode::MultiBillboard", EditConditionHides, ToolTip = "Maximum permitted atlas resolution containing all local leaf-cluster Billboard tiles. Manual mode scales tiles to use this limit; Auto mode stops increasing resolution at this limit."))
	int32 MultiBillboardTextureResolution = 4096;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "2", ClampMax = "16", DisplayName = "Alpha Crop Guard", ToolTip = "Extra prepass pixels retained around the detected visible-alpha bounds. Auto measures every plane independently at Target Texels Per Meter before packing; Manual measures the packed prepass atlas. Front/back and grouped-view bounds are conservatively merged when present. Alpha cropping is always enabled for Billboard, Cross Cards, and MultiBillboard."))
	int32 AlphaCropGuardPixels = 2;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Mip", meta = (DisplayName = "Preserve Alpha Mask Values", ToolTip = "Generate semantic mask mips independently inside every Billboard, Cross Cards, or MultiBillboard atlas tile. Alpha remains exactly background 0, trunk 0.5, or leaf 1 instead of being averaged to gray values."))
	bool bPreserveAlphaMaskValues = true;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Mip", meta = (ClampMin = "0.01", ClampMax = "1.0", EditCondition = "bPreserveAlphaMaskValues", EditConditionHides, DisplayName = "Mip Mask Coverage Threshold", ToolTip = "Minimum fraction of covered Mip 0 samples required to keep a destination mip pixel. Lower values preserve fuller foliage silhouettes; higher values remove more thin coverage."))
	float MipMaskCoverageThreshold = 0.35f;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Optimization", meta = (DisplayName = "Trim Unused Atlas Space", ToolTip = "Enabled: tightly removes unused outer atlas rows and columns using block-aligned dimensions, which may be non-power-of-two. Disabled: fits the atlas to the used UV tile bounds, rounds each dimension up to a power of two, and balances the remaining block-aligned space around the tiles, allowing rectangular outputs such as 512x1024. UV-island RGB padding fills all remaining atlas pixels and every generated mip. In this mode a non-power-of-two maximum resolution is rounded down to the nearest power of two. Per-view alpha bounds are always cropped independently."))
	bool bTrimUnusedAtlasSpace = false;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (DisplayName = "Bake Base Color / Opacity", ToolTip = "RGB stores base color. A stores visible source classification: background 0, trunk 0.5, leaf 1."))
	bool bBakeBaseColorOpacity = true;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (DisplayName = "Bake Normal / Trunk Leaf Mask", ToolTip = "RGB stores object/local-space normal for Single Plane and Cross Cards. Double Planes re-expresses each view's normal in its capture Facing/Right/Up frame so both views use the same billboard decoder. A stores the visible trunk/leaf classification after source opacity clipping: background 0, trunk 0.5, leaf 1."))
	bool bBakeNormalDepth = true;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (ToolTip = "RGBA stores Occlusion, Roughness, Metallic, and Emission. The destination material texture parameter is configured in Material."))
	bool bBakeMix = false;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (DisplayName = "Bake Upper Hemisphere L1 Visibility", EditCondition = "Mode == EFoliageBakerCardMode::SingleBillboard", EditConditionHides, ToolTip = "Bakes low-frequency self-visibility for source-local light directions over the upper hemisphere. RGB store signed X/Y/Z directional coefficients remapped from -1..1 to 0..1; A stores the constant coefficient. Runtime reconstruction is saturate(A + dot(RGB * 2 - 1, BakedLightDirection)). BakedLightDirection points toward the light and must be transformed back through the Billboard WPO rotation into the original bake frame."))
	bool bBakeUpperHemisphereL1Visibility = false;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (ClampMin = "64", ClampMax = "1024", DisplayName = "L1 Visibility Texture Resolution", EditCondition = "Mode == EFoliageBakerCardMode::SingleBillboard && bBakeUpperHemisphereL1Visibility", EditConditionHides, ToolTip = "Maximum dimension of the generated L1 coefficient atlas. The Billboard atlas aspect ratio and normalized tile layout are preserved, and every tile is resized independently to prevent cross-tile filtering."))
	int32 UpperHemisphereL1TextureResolution = 512;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (ClampMin = "4", ClampMax = "32", DisplayName = "L1 Visibility Samples", EditCondition = "Mode == EFoliageBakerCardMode::SingleBillboard && bBakeUpperHemisphereL1Visibility", EditConditionHides, ToolTip = "Number of uniformly distributed upper-hemisphere directions used to fit the four L1 visibility coefficients. Higher values improve stability but increase offline bake time."))
	int32 UpperHemisphereL1SampleCount = 12;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (ClampMin = "64", ClampMax = "1024", DisplayName = "L1 Shadow Map Resolution", EditCondition = "Mode == EFoliageBakerCardMode::SingleBillboard && bBakeUpperHemisphereL1Visibility", EditConditionHides, ToolTip = "Maximum internal masked shadow-map dimension used for each sampled light direction. Each receiver uses a fixed 5x5 PCF depth-comparison kernel before L1 fitting. This does not change the generated coefficient atlas resolution."))
	int32 UpperHemisphereL1ShadowMapResolution = 1024;

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString TextureOutputFolderName = TEXT("Textures");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MaterialOutputFolderName = TEXT("Materials");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (DisplayName = "Place Assets Near Replaced LOD Assets", ToolTip = "When enabled and Replace LOD is selected, creates the generated material near the materials used by the target LOD and creates generated textures in the nearest referenced texture folder. Falls back to the configured output folders when no suitable source folder can be resolved."))
	bool bPlaceGeneratedAssetsNearReplacedLODAssets = true;

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString TextureNamePrefix = TEXT("T_");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (DisplayName = "Base Color / Opacity Texture Suffix"))
	FString BaseColorOpacityTextureSuffix = TEXT("_DA");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (DisplayName = "Normal / Mask Texture Suffix"))
	FString NormalDepthTextureSuffix = TEXT("_NR");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MixTextureSuffix = TEXT("_M");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (DisplayName = "Upper Hemisphere L1 Visibility Texture Suffix", EditCondition = "Mode == EFoliageBakerCardMode::SingleBillboard", EditConditionHides))
	FString UpperHemisphereL1VisibilityTextureSuffix = TEXT("_L1V");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MaterialInstanceNamePrefix = TEXT("MI_");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MaterialInstanceNameSuffix;

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Standard Parent Material Instance", ToolTip = "Editor Preferences provides the default. The current tool panel can override it for this session. Used by Single Plane Billboard, Cross Cards, or MultiBillboard. Double Planes Billboard uses its dedicated Parent Material Instance slot. Generated proxy materials are new child Material Instance Constants."))
	TSoftObjectPtr<UMaterialInstanceConstant> MaterialInstanceTemplate;

	UPROPERTY(config, EditAnywhere, Category = "Material|Source Bake Override", meta = (DisplayName = "Override Static Switches During Bake", ToolTip = "Creates one transient child Material Instance per unique selected-LOD material and applies every configured Global static switch override that exists on that material. Missing switches emit warnings and the remaining overrides continue. Source material assets are never modified. World Position Offset is evaluated with animation time fixed at zero; Displacement remains disabled."))
	bool bOverrideBakeStaticSwitch = false;

	UPROPERTY(config, EditAnywhere, Category = "Material|Source Bake Override", meta = (DisplayName = "Static Switch Overrides", EditCondition = "bOverrideBakeStaticSwitch", EditConditionHides, ToolTip = "Global static switches and their temporary Bake values. Each switch may appear only once. A missing switch warns and is skipped for that material."))
	TArray<FFoliageBakerBakeStaticSwitchOverride> BakeStaticSwitchOverrides = {
		FFoliageBakerBakeStaticSwitchOverride()
	};

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Base Color / Opacity Parameter", ToolTip = "Texture parameter receiving BaseColor RGB and trunk/leaf opacity classification in A."))
	FName BaseColorOpacityTextureParameterName = TEXT("ColorOpacity");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Normal / Trunk Leaf Mask Parameter", ToolTip = "Texture parameter receiving Normal RGB and the trunk/leaf classification in A. Double Planes Normal RGB uses the shared capture-frame convention required by its billboard decoder."))
	FName NormalDepthTextureParameterName = TEXT("NormalMask");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Mix Parameter", ToolTip = "Texture parameter receiving the generated Occlusion/Roughness/Metallic/Emission texture when that output is enabled."))
	FName MixTextureParameterName = TEXT("Mix");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Upper Hemisphere L1 Visibility Parameter", EditCondition = "Mode == EFoliageBakerCardMode::SingleBillboard", EditConditionHides, ToolTip = "Texture parameter receiving the optional upper-hemisphere L1 self-visibility coefficient atlas. The plugin only assigns this parameter; it does not modify the parent material graph."))
	FName UpperHemisphereL1VisibilityTextureParameterName = TEXT("UpperHemisphereL1Visibility");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (EditCondition = "!bBakeMix", DisplayName = "Leaf Roughness Parameter", ToolTip = "Scalar parameter receiving the average baked Roughness of visible leaf pixels when Mix output is disabled and valid leaf pixels exist."))
	FName LeafRoughnessParameterName = TEXT("LeafRoughness");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (EditCondition = "!bBakeMix", DisplayName = "Leaf Specular Parameter", ToolTip = "Scalar parameter receiving the average baked Specular of visible leaf pixels when Mix output is disabled and valid leaf pixels exist."))
	FName LeafSpecularParameterName = TEXT("LeafSpecular");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (EditCondition = "!bBakeMix", DisplayName = "Trunk Roughness Parameter", ToolTip = "Scalar parameter receiving the average baked Roughness of visible trunk pixels when Mix output is disabled and valid trunk pixels exist."))
	FName TrunkRoughnessParameterName = TEXT("TrunkRoughness");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (EditCondition = "!bBakeMix", DisplayName = "Trunk Specular Parameter", ToolTip = "Scalar parameter receiving the average baked Specular of visible trunk pixels when Mix output is disabled and valid trunk pixels exist."))
	FName TrunkSpecularParameterName = TEXT("TrunkSpecular");
};

UCLASS(config = EditorPerProjectUserSettings, Transient, PrioritizeCategories = ("Mesh", "Feature", "Asset", "Material"), meta = (DisplayName = "Foliage Baker - Billboard"))
class FOLIAGEBAKERCARDS_API UFoliageBakerSingleBillboardSettings final : public UFoliageBakerCardsSettings
{
	GENERATED_BODY()

public:
	UFoliageBakerSingleBillboardSettings();

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Double Planes Parent Material Instance", ToolTip = "Editor Preferences provides the default. The current Billboard tool panel can override it for this session. Used exclusively by Double Planes Billboard. The generated mesh provides the per-plane atlas tile in UV0, local capture direction in UV1.xy, and plane selector 0 or 1 in UV2.x for view-angle Dither blending and dynamic plane spacing."))
	TSoftObjectPtr<UMaterialInstanceConstant> DoublePlanesMaterialInstanceTemplate;
};

UCLASS(config = EditorPerProjectUserSettings, Transient, PrioritizeCategories = ("Mesh", "Feature", "Asset", "Material"), meta = (DisplayName = "Foliage Baker - Cross Cards"))
class FOLIAGEBAKERCARDS_API UFoliageBakerCrossCardsSettings final : public UFoliageBakerCardsSettings
{
	GENERATED_BODY()

public:
	UFoliageBakerCrossCardsSettings();
};

UCLASS(config = EditorPerProjectUserSettings, Transient, PrioritizeCategories = ("Mesh", "Feature", "Asset", "Material"), meta = (DisplayName = "Foliage Baker - MultiBillboard"))
class FOLIAGEBAKERCARDS_API UFoliageBakerMultiBillboardSettings final : public UFoliageBakerCardsSettings
{
	GENERATED_BODY()

public:
	UFoliageBakerMultiBillboardSettings();
};
