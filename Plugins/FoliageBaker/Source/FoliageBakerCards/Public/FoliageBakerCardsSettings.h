#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "FoliageBakerCardsSettings.generated.h"

class UMaterialInstanceConstant;
class UStaticMesh;

UENUM()
enum class EFoliageBakerCardMode : uint8
{
	SingleBillboard UMETA(DisplayName = "Single Billboard"),
	CrossCards UMETA(DisplayName = "Cross Cards")
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

	UPROPERTY(EditAnywhere, Category = "Mesh", meta = (ToolTip = "Static Mesh assets baked from the selected Source LOD. Add assets directly or use Add Content Browser Selection."))
	TArray<TObjectPtr<UStaticMesh>> SourceStaticMeshes;

	UPROPERTY(config, EditAnywhere, Category = "Mesh", meta = (ClampMin = "0", ClampMax = "7", DisplayName = "Source LOD Index", ToolTip = "Source Static Mesh LOD used for geometry extraction, projected bounds, material baking, and trunk/leaf classification. Every queued mesh must contain this LOD."))
	int32 SourceLODIndex = 0;


	UPROPERTY(config)
	EFoliageBakerCardMode Mode = EFoliageBakerCardMode::SingleBillboard;

	UPROPERTY(config, EditAnywhere, Category = "Feature", meta = (DisplayName = "Billboard Mode", EditCondition = "Mode == EFoliageBakerCardMode::SingleBillboard", EditConditionHides, ToolTip = "Single Plane captures one view and emits one camera-facing plane. Double Planes captures the selected primary axis plus a second horizontal axis rotated 90 degrees around local +Z, then emits two parallel camera-facing planes for angle-based material blending."))
	EFoliageBakerBillboardMode BillboardMode = EFoliageBakerBillboardMode::SinglePlane;

	UPROPERTY(config, EditAnywhere, Category = "Feature", meta = (DisplayName = "Primary Capture Axis", EditCondition = "Mode == EFoliageBakerCardMode::SingleBillboard", EditConditionHides, ToolTip = "Camera is placed on the selected local axis and looks toward the source mesh. Double Planes derives its second capture direction by rotating this axis 90 degrees around local +Z. Generated planes pass through the source Static Mesh local origin (asset pivot)."))
	EFoliageBakerSingleCaptureAxis SingleCaptureAxis = EFoliageBakerSingleCaptureAxis::PositiveX;

	UPROPERTY(config, EditAnywhere, Category = "Feature", meta = (ClampMin = "2", ClampMax = "5", EditCondition = "Mode == EFoliageBakerCardMode::CrossCards", EditConditionHides, ToolTip = "Number of vertical planes distributed evenly over 180 degrees. Every plane is baked from both sides, and all planes intersect on the vertical axis through the source Static Mesh local origin (asset pivot)."))
	int32 CrossCardPlaneCount = 2;

	UPROPERTY(config, EditAnywhere, Category = "Feature", meta = (DisplayName = "Cross Card Face Mode", EditCondition = "Mode == EFoliageBakerCardMode::CrossCards", EditConditionHides, ToolTip = "Two-Sided keeps one quad per direction and stores front/back atlas tiles in UV0/UV1. Separate One-Sided Faces emits an oppositely wound quad for each side, stores that side's tile in UV0, and forces the generated material instance to be one-sided."))
	EFoliageBakerCrossCardFaceMode CrossCardFaceMode = EFoliageBakerCrossCardFaceMode::TwoSidedTwoUVs;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Trunk Leaf Mask", meta = (DisplayName = "Trunk Material Keywords", ToolTip = "Same classification rule as BillboardClouds: source material instance or parent material names containing any keyword are trunk; all other visible source triangles are leaf. Empty means all visible pixels are leaf."))
	TArray<FString> TrunkMaterialKeywords = { TEXT("Trunk") };

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "256", ClampMax = "4096", DisplayName = "Texture Resolution", EditCondition = "Mode == EFoliageBakerCardMode::SingleBillboard", EditConditionHides, ToolTip = "Maximum atlas resolution used by Single Plane or Double Planes Billboard."))
	int32 SingleTextureResolution = 1024;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "256", ClampMax = "4096", EditCondition = "Mode == EFoliageBakerCardMode::CrossCards", EditConditionHides))
	int32 CrossTextureResolution = 2048;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "2", ClampMax = "16", DisplayName = "Per-View Alpha Crop Guard", ToolTip = "Extra pixels retained around the automatically detected visible-alpha bounds. Per-view alpha cropping is always enabled for Billboard and Cross Cards."))
	int32 AlphaCropGuardPixels = 2;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Optimization", meta = (DisplayName = "Trim Unused Atlas Space", ToolTip = "After all Billboard or Cross Cards tiles are packed, remove completely unused outer atlas rows and columns. Output dimensions remain block-aligned and can become rectangular. Per-view alpha bounds are always cropped independently of this option."))
	bool bTrimUnusedAtlasSpace = false;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (DisplayName = "Bake Base Color / Opacity", ToolTip = "RGB stores base color. A stores visible source classification: background 0, trunk 0.5, leaf 1."))
	bool bBakeBaseColorOpacity = true;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (DisplayName = "Bake Normal / Trunk Leaf Mask", ToolTip = "RGB stores object/local-space normal for Single Plane and Cross Cards. Double Planes re-expresses each view's normal in its capture Facing/Right/Up frame so both views use the same billboard decoder. A stores the visible trunk/leaf classification after source opacity clipping: background 0, trunk 0.5, leaf 1."))
	bool bBakeNormalDepth = true;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (ToolTip = "RGBA stores Occlusion, Roughness, Metallic, and Emission. The destination material texture parameter is configured in Material."))
	bool bBakeMix = false;

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString TextureOutputFolderName = TEXT("Textures");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MaterialOutputFolderName = TEXT("Materials");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString TextureNamePrefix = TEXT("T_");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (DisplayName = "Base Color / Opacity Texture Suffix"))
	FString BaseColorOpacityTextureSuffix = TEXT("_DA");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (DisplayName = "Normal / Mask Texture Suffix"))
	FString NormalDepthTextureSuffix = TEXT("_NR");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MixTextureSuffix = TEXT("_M");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MaterialInstanceNamePrefix = TEXT("MI_");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MaterialInstanceNameSuffix;

	UPROPERTY(config, EditDefaultsOnly, Category = "Material", meta = (DisplayName = "Standard Parent Material Instance", ToolTip = "Configured only in Editor Preferences. Used by Single Plane Billboard or Cross Cards. Double Planes Billboard uses its dedicated Parent Material Instance slot. Generated proxy materials are new child Material Instance Constants."))
	TSoftObjectPtr<UMaterialInstanceConstant> MaterialInstanceTemplate;

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Base Color / Opacity Parameter", ToolTip = "Texture parameter receiving BaseColor RGB and trunk/leaf opacity classification in A."))
	FName BaseColorOpacityTextureParameterName = TEXT("ColorOpacity");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Normal / Trunk Leaf Mask Parameter", ToolTip = "Texture parameter receiving Normal RGB and the trunk/leaf classification in A. Double Planes Normal RGB uses the shared capture-frame convention required by its billboard decoder."))
	FName NormalDepthTextureParameterName = TEXT("NormalMask");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Mix Parameter", ToolTip = "Texture parameter receiving the generated Occlusion/Roughness/Metallic/Emission texture when that output is enabled."))
	FName MixTextureParameterName = TEXT("Mix");

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

	UPROPERTY(config, EditDefaultsOnly, Category = "Material", meta = (DisplayName = "Double Planes Parent Material Instance", ToolTip = "Configured only in Editor Preferences. Used exclusively by Double Planes Billboard. The generated mesh provides the per-plane atlas tile in UV0, local capture direction in UV1.xy, and plane selector 0 or 1 in UV2.x for view-angle Dither blending and dynamic plane spacing."))
	TSoftObjectPtr<UMaterialInstanceConstant> DoublePlanesMaterialInstanceTemplate;
};

UCLASS(config = EditorPerProjectUserSettings, Transient, PrioritizeCategories = ("Mesh", "Feature", "Asset", "Material"), meta = (DisplayName = "Foliage Baker - Cross Cards"))
class FOLIAGEBAKERCARDS_API UFoliageBakerCrossCardsSettings final : public UFoliageBakerCardsSettings
{
	GENERATED_BODY()

public:
	UFoliageBakerCrossCardsSettings();
};
