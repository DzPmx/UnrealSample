#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "FoliageBakerImpostorSettings.generated.h"

class UMaterialInstanceConstant;
class UStaticMesh;

UENUM()
enum class EFoliageBakerImpostorCoverage : uint8
{
	UpperHemisphere UMETA(DisplayName = "Upper Hemisphere"),
	FullSphere UMETA(DisplayName = "Full Sphere")
};

UCLASS(config = EditorPerProjectUserSettings, Transient, PrioritizeCategories = ("Mesh", "Feature", "Asset", "Material"), meta = (DisplayName = "Foliage Baker - Impostor"))
class FOLIAGEBAKERIMPOSTOR_API UFoliageBakerImpostorSettings final : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(Transient, EditAnywhere, Category = "Mesh", meta = (ToolTip = "Static Mesh assets baked from the selected Source LOD. Add assets directly or use Add Content Browser Selection."))
	TArray<TObjectPtr<UStaticMesh>> SourceStaticMeshes;

	UPROPERTY(config, EditAnywhere, Category = "Mesh", meta = (ClampMin = "0", ClampMax = "7", DisplayName = "Source LOD Index", ToolTip = "Source Static Mesh LOD used for geometry extraction, material baking, shared bounds, and depth encoding."))
	int32 SourceLODIndex = 0;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Sampling")
	EFoliageBakerImpostorCoverage Coverage = EFoliageBakerImpostorCoverage::UpperHemisphere;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Sampling", meta = (ClampMin = "3", ClampMax = "8", DisplayName = "Frame Grid Size", ToolTip = "Number of rows and columns in the octahedral direction grid. The baker captures Frame Grid Size squared views."))
	int32 FrameGridSize = 4;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Classification", meta = (DisplayName = "Trunk Material Keywords", ToolTip = "Material or parent material names containing any keyword are classified as trunk. All other materials are classified as leaf."))
	TArray<FString> TrunkMaterialKeywords = { TEXT("Trunk") };

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "256", ClampMax = "4096", DisplayName = "Maximum Atlas Resolution", ToolTip = "Maximum dimension of the generated square atlas. Square tiles are arranged in the configured octahedral frame grid."))
	int32 TextureResolution = 2048;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture", meta = (ClampMin = "1", ClampMax = "64", DisplayName = "Opacity SDF Range", Suffix = "px", ToolTip = "Pixel distance from the 0.5 contour to fully inside or outside in BaseColor Alpha. It does not add padding or change the fixed view grid."))
	int32 OpacitySdfRangePixels = 16;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (DisplayName = "Bake Base Color / SDF", ToolTip = "RGB stores BaseColor. A stores a whole-vegetation SDF: outside 0, contour 0.5, inside 1."))
	bool bBakeBaseColorSdf = true;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (DisplayName = "Bake Normal / Mask / Depth", ToolTip = "RG stores octahedral object/local-space Normal. B stores trunk 0.5 or leaf 1. A stores shared-range linear depth: near 1, far 0, uncovered 0.5."))
	bool bBakeNormalDepth = true;

	UPROPERTY(config, EditAnywhere, Category = "Feature|Texture|Outputs", meta = (ToolTip = "RGBA stores Occlusion, Roughness, Metallic, and Emission."))
	bool bBakeMix = false;

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString TextureOutputFolderName = TEXT("Textures");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MaterialOutputFolderName = TEXT("Materials");

	UPROPERTY(config, EditAnywhere, Category = "Asset", meta = (DisplayName = "Place Assets Near Replaced LOD Assets", ToolTip = "When enabled and Replace LOD is selected, creates the generated material near the materials used by the target LOD and creates generated textures in the nearest referenced texture folder. Falls back to the configured output folders when no suitable source folder can be resolved."))
	bool bPlaceGeneratedAssetsNearReplacedLODAssets = true;

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString TextureNamePrefix = TEXT("T_");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString BaseColorSdfTextureSuffix = TEXT("_Impostor_DA");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString NormalDepthTextureSuffix = TEXT("_Impostor_NR");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MixTextureSuffix = TEXT("_Impostor_M");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MaterialInstanceNamePrefix = TEXT("MI_");

	UPROPERTY(config, EditAnywhere, Category = "Asset")
	FString MaterialInstanceNameSuffix = TEXT("_Impostor");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Parent Material Instance", ToolTip = "Editor Preferences provides the default. The current tool panel can override it for this session. Generated proxy materials are new child Material Instance Constants whose Parent is this instance."))
	TSoftObjectPtr<UMaterialInstanceConstant> MaterialInstanceTemplate;

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Base Color / SDF Parameter"))
	FName BaseColorSdfTextureParameterName = TEXT("ColorOpacity");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Normal / Mask / Depth Parameter"))
	FName NormalDepthTextureParameterName = TEXT("NormalMask");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Packed Masks Parameter"))
	FName MixTextureParameterName = TEXT("PackedMasks_1");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (EditCondition = "!bBakeMix", DisplayName = "Leaf Roughness Parameter", ToolTip = "Scalar parameter receiving the average baked Roughness of final visible leaf pixels when Mix output is disabled and valid leaf pixels exist."))
	FName LeafRoughnessParameterName = TEXT("LeafRoughness");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (EditCondition = "!bBakeMix", DisplayName = "Leaf Specular Parameter", ToolTip = "Scalar parameter receiving the average baked Specular of final visible leaf pixels when Mix output is disabled and valid leaf pixels exist."))
	FName LeafSpecularParameterName = TEXT("LeafSpecular");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (EditCondition = "!bBakeMix", DisplayName = "Trunk Roughness Parameter", ToolTip = "Scalar parameter receiving the average baked Roughness of final visible trunk pixels when Mix output is disabled and valid trunk pixels exist."))
	FName TrunkRoughnessParameterName = TEXT("TrunkRoughness");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (EditCondition = "!bBakeMix", DisplayName = "Trunk Specular Parameter", ToolTip = "Scalar parameter receiving the average baked Specular of final visible trunk pixels when Mix output is disabled and valid trunk pixels exist."))
	FName TrunkSpecularParameterName = TEXT("TrunkSpecular");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Frames XY Parameter", ToolTip = "Scalar parameter receiving the shared row and column count used by the square octahedral atlas."))
	FName FramesParameterName = TEXT("FramesXY");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Default Mesh Size Parameter", ToolTip = "Scalar parameter receiving the shared square capture and proxy diameter."))
	FName DefaultMeshSizeParameterName = TEXT("Default Mesh Size");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Pivot Offset Parameter", ToolTip = "Vector parameter receiving the selected-LOD local bounds center relative to the source asset pivot."))
	FName PivotOffsetParameterName = TEXT("Pivot Offset");

	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (DisplayName = "Upper Hemisphere Switch Parameter", ToolTip = "Static switch enabled for Upper Hemisphere capture and disabled for Full Sphere capture."))
	FName UpperHemisphereStaticSwitchParameterName = TEXT("UpperHemisphereOnlyImpostor");
};
