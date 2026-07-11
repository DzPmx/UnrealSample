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
enum class EFoliageBakerCardsMeshOutputMode : uint8
{
	SeparateMeshAsset UMETA(DisplayName = "Create Separate Mesh Asset"),
	AddToSourceMeshLOD UMETA(DisplayName = "Add To Source Mesh LODs"),
	ReplaceSourceMeshLOD UMETA(DisplayName = "Replace Source Mesh LOD")
};

UCLASS(config = EditorPerProjectUserSettings, defaultconfig, Transient, PrioritizeCategories = ("Mesh", "Feature", "Asset", "Material"), meta = (DisplayName = "Foliage Baker - Billboard & Cross Cards"))
class FOLIAGEBAKERCARDS_API UFoliageBakerCardsSettings : public UObject
{
	GENERATED_BODY()

public:
	UFoliageBakerCardsSettings();

	UPROPERTY(EditAnywhere, Category = "Mesh", meta = (ToolTip = "Static Mesh assets baked from the selected Source LOD. Add assets directly or use Add Content Browser Selection."))
	TArray<TObjectPtr<UStaticMesh>> SourceStaticMeshes;

	UPROPERTY(config, EditAnywhere, Category = "Mesh", meta = (ClampMin = "0", ClampMax = "7", DisplayName = "Source LOD Index", ToolTip = "Source Static Mesh LOD used for geometry extraction, projected bounds, material baking, and shared depth encoding. Every queued mesh must contain this LOD."))
	int32 SourceLODIndex = 0;


	UPROPERTY(config)
	EFoliageBakerCardMode Mode = EFoliageBakerCardMode::SingleBillboard;

	UPROPERTY(config, EditAnywhere, Category = "Mesh")
	EFoliageBakerCardsMeshOutputMode MeshOutputMode = EFoliageBakerCardsMeshOutputMode::SeparateMeshAsset;

	UPROPERTY(config, EditAnywhere, Category = "Mesh", meta = (ClampMin = "0", ClampMax = "7", DisplayName = "Output LOD Index To Replace", EditCondition = "MeshOutputMode == EFoliageBakerCardsMeshOutputMode::ReplaceSourceMeshLOD", EditConditionHides, ToolTip = "Existing destination LOD overwritten by the generated proxy. It must differ from Source LOD Index so the original input remains available for rebaking."))
	int32 ReplaceSourceLODIndex = 1;

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

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "0", ClampMax = "16", DisplayName = "Per-View Alpha Crop Guard", ToolTip = "Extra pixels retained around the automatically detected visible-alpha bounds. Per-view alpha cropping is always enabled for Single Billboard and Cross Cards."))
	int32 AlphaCropGuardPixels = 2;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Optimization", meta = (DisplayName = "Trim Unused Atlas Space", ToolTip = "After all Single Billboard or Cross Cards tiles are packed, remove completely unused outer atlas rows and columns. Output dimensions remain block-aligned and can become rectangular. Per-view alpha bounds are always cropped independently of this option."))
	bool bTrimUnusedAtlasSpace = false;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (ToolTip = "RGB stores base color. After source opacity clipping, A stores 0 for transparent background, 0.5 for trunk, and 1 for leaf using the BillboardClouds material-keyword classification rule."))
	bool bBakeBaseColorOpacity = true;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (ToolTip = "RGB stores object/local-space normal. A stores linear depth using one Min/Max range shared by all capture views: the globally nearest selected-LOD geometry point maps to 0 and the globally farthest maps to 1."))
	bool bBakeNormalDepth = true;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (ToolTip = "RGBA stores Occlusion, Roughness, Metallic, and Emission. The destination material texture parameter is configured in Material."))
	bool bBakeMix = false;

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString TextureOutputFolderName = TEXT("Textures");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MaterialOutputFolderName = TEXT("Materials");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString TextureNamePrefix = TEXT("T_");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString BaseColorOpacityTextureSuffix = TEXT("_DA");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString NormalDepthTextureSuffix = TEXT("_NR");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MixTextureSuffix = TEXT("_M");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MaterialInstanceNamePrefix = TEXT("MI_");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MaterialInstanceNameSuffix;

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (ToolTip = "Material Instance Constant template duplicated for every generated proxy. The baker assigns the generated textures to the configured texture parameter names without creating or editing a material graph."))
	TSoftObjectPtr<UMaterialInstanceConstant> MaterialInstanceTemplate;

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Base Color / Opacity Parameter", ToolTip = "Texture parameter receiving the generated BaseColor/Opacity texture when that output is enabled."))
	FName BaseColorOpacityTextureParameterName = TEXT("ColorOpacity");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Normal / Depth Parameter", ToolTip = "Texture parameter receiving object/local-space Normal RGB and the globally shared linear Depth mapping in A."))
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
