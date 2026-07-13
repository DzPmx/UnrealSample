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

UCLASS(config = EditorPerProjectUserSettings, defaultconfig, Transient, PrioritizeCategories = ("Mesh", "Feature", "Asset", "Material"), meta = (DisplayName = "Foliage Baker - Billboard & Cross Cards"))
class FOLIAGEBAKERCARDS_API UFoliageBakerCardsSettings : public UObject
{
	GENERATED_BODY()

public:
	UFoliageBakerCardsSettings();

	UPROPERTY(EditAnywhere, Category = "Mesh", meta = (ToolTip = "Static Mesh assets baked from the selected Source LOD. Add assets directly or use Add Content Browser Selection."))
	TArray<TObjectPtr<UStaticMesh>> SourceStaticMeshes;

	UPROPERTY(config, EditAnywhere, Category = "Mesh", meta = (ClampMin = "0", ClampMax = "7", DisplayName = "Source LOD Index", ToolTip = "Source Static Mesh LOD used for geometry extraction, projected bounds, material baking, SDF generation, and trunk/leaf classification. Every queued mesh must contain this LOD."))
	int32 SourceLODIndex = 0;


	UPROPERTY(config)
	EFoliageBakerCardMode Mode = EFoliageBakerCardMode::SingleBillboard;

	UPROPERTY(config, EditAnywhere, Category = "Feature", meta = (EditCondition = "Mode == EFoliageBakerCardMode::SingleBillboard", EditConditionHides, ToolTip = "Camera is placed on the selected axis and looks toward the source mesh. The generated billboard plane passes through the source Static Mesh local origin (asset pivot)."))
	EFoliageBakerSingleCaptureAxis SingleCaptureAxis = EFoliageBakerSingleCaptureAxis::PositiveX;

	UPROPERTY(config, EditAnywhere, Category = "Feature", meta = (ClampMin = "2", ClampMax = "5", EditCondition = "Mode == EFoliageBakerCardMode::CrossCards", EditConditionHides, ToolTip = "Number of vertical planes distributed evenly over 180 degrees. Every plane is baked from both sides, and all planes intersect on the vertical axis through the source Static Mesh local origin (asset pivot)."))
	int32 CrossCardPlaneCount = 2;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Trunk Leaf Mask", meta = (DisplayName = "Trunk Material Keywords", ToolTip = "Same classification rule as BillboardClouds: source material instance or parent material names containing any keyword are trunk; all other visible source triangles are leaf. Empty means all visible pixels are leaf."))
	TArray<FString> TrunkMaterialKeywords = { TEXT("Trunk") };

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "256", ClampMax = "4096", EditCondition = "Mode == EFoliageBakerCardMode::SingleBillboard", EditConditionHides))
	int32 SingleTextureResolution = 1024;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "256", ClampMax = "4096", EditCondition = "Mode == EFoliageBakerCardMode::CrossCards", EditConditionHides))
	int32 CrossTextureResolution = 2048;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "256", ClampMax = "4096", ToolTip = "Resolution used to evaluate source material data before the final card atlas is assembled."))
	int32 SourceMaterialBakeResolution = 2048;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "2", ClampMax = "16", DisplayName = "Per-View Alpha Crop Guard", ToolTip = "Extra pixels retained around the automatically detected visible-alpha bounds. Per-view alpha cropping is always enabled for Single Billboard and Cross Cards."))
	int32 AlphaCropGuardPixels = 2;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "1", ClampMax = "64", DisplayName = "Opacity SDF Range", Suffix = "px", ToolTip = "Pixel distance from the 0.5 contour to fully inside or outside in the BaseColor Alpha Union SDF. It does not add padding or expand the cropped tile."))
	int32 OpacitySdfRangePixels = 16;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Optimization", meta = (DisplayName = "Trim Unused Atlas Space", ToolTip = "After all Single Billboard or Cross Cards tiles are packed, remove completely unused outer atlas rows and columns. Output dimensions remain block-aligned and can become rectangular. Per-view alpha bounds are always cropped independently of this option."))
	bool bTrimUnusedAtlasSpace = false;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (DisplayName = "Bake Base Color / SDF", ToolTip = "RGB stores base color. A stores one continuous whole-vegetation Union SDF: outside 0, contour 0.5, inside 1."))
	bool bBakeBaseColorOpacity = true;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (DisplayName = "Bake Normal / Trunk Leaf Mask", ToolTip = "RGB stores object/local-space normal. A stores the visible trunk/leaf classification after source opacity clipping: background 0, trunk 0.5, leaf 1."))
	bool bBakeNormalDepth = true;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (ToolTip = "RGBA stores Occlusion, Roughness, Metallic, and Emission. The destination material texture parameter is configured in Material."))
	bool bBakeMix = false;

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString TextureOutputFolderName = TEXT("Textures");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MaterialOutputFolderName = TEXT("Materials");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString TextureNamePrefix = TEXT("T_");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (DisplayName = "Base Color / SDF Texture Suffix"))
	FString BaseColorOpacityTextureSuffix = TEXT("_DA");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (DisplayName = "Normal / Mask Texture Suffix"))
	FString NormalDepthTextureSuffix = TEXT("_NR");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MixTextureSuffix = TEXT("_M");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MaterialInstanceNamePrefix = TEXT("MI_");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MaterialInstanceNameSuffix;

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (ToolTip = "Material Instance Constant template duplicated for every generated proxy. The baker assigns the generated textures to the configured texture parameter names without creating or editing a material graph."))
	TSoftObjectPtr<UMaterialInstanceConstant> MaterialInstanceTemplate;

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Base Color / SDF Parameter", ToolTip = "Texture parameter receiving BaseColor RGB and the whole-vegetation Union SDF in A."))
	FName BaseColorOpacityTextureParameterName = TEXT("ColorOpacity");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Normal / Trunk Leaf Mask Parameter", ToolTip = "Texture parameter receiving object/local-space Normal RGB and the trunk/leaf classification in A."))
	FName NormalDepthTextureParameterName = TEXT("NormalMask");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Mix Parameter", ToolTip = "Texture parameter receiving the generated Occlusion/Roughness/Metallic/Emission texture when that output is enabled."))
	FName MixTextureParameterName = TEXT("Mix");
};

UCLASS(config = EditorPerProjectUserSettings, defaultconfig, Transient, PrioritizeCategories = ("Mesh", "Feature", "Asset", "Material"), meta = (DisplayName = "Foliage Baker - Single Billboard"))
class FOLIAGEBAKERCARDS_API UFoliageBakerSingleBillboardSettings final : public UFoliageBakerCardsSettings
{
	GENERATED_BODY()

public:
	UFoliageBakerSingleBillboardSettings();
};

UCLASS(config = EditorPerProjectUserSettings, defaultconfig, Transient, PrioritizeCategories = ("Mesh", "Feature", "Asset", "Material"), meta = (DisplayName = "Foliage Baker - Cross Cards"))
class FOLIAGEBAKERCARDS_API UFoliageBakerCrossCardsSettings final : public UFoliageBakerCardsSettings
{
	GENERATED_BODY()

public:
	UFoliageBakerCrossCardsSettings();
};
