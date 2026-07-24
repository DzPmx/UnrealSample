#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureDefines.h"
#include "FoliageBakerMaterialResolver.h"
#include "FoliageBakerMeshOutput.h"
#include "FoliageBakerPlaneCover.h"
#include "UObject/StrongObjectPtr.h"

struct FMeshDescription;
class UMaterialInstanceConstant;
class UMaterialInterface;
class UStaticMesh;
class UTexture2D;


class FOLIAGEBAKERCORE_API FFoliageBakerAssetTransaction final
{
public:
	FFoliageBakerAssetTransaction() = default;
	~FFoliageBakerAssetTransaction();
	FFoliageBakerAssetTransaction(const FFoliageBakerAssetTransaction&) = delete;
	FFoliageBakerAssetTransaction& operator=(const FFoliageBakerAssetTransaction&) = delete;
	FFoliageBakerAssetTransaction(FFoliageBakerAssetTransaction&&) = delete;
	FFoliageBakerAssetTransaction& operator=(FFoliageBakerAssetTransaction&&) = delete;


	void Track(UObject* Asset);


	bool Snapshot(UObject* Asset, FString& OutError);


	void SnapshotMetadata(UObject* Asset, FName Key);

	void Commit();
	void Rollback();

private:
	struct FObjectSnapshot
	{
		UObject* Original = nullptr;
		TStrongObjectPtr<UObject> Backup;
		bool bPackageWasDirty = false;
		EObjectFlags ObjectFlags = RF_NoFlags;
	};

	struct FMetadataSnapshot
	{
		UObject* Asset = nullptr;
		FName Key = NAME_None;
		FString Value;
		bool bHadValue = false;
	};

	TArray<UObject*> CreatedAssets;
	TArray<FObjectSnapshot> ObjectSnapshots;
	TArray<FMetadataSnapshot> MetadataSnapshots;
	bool bFinished = false;
};

enum class EFoliageBakerExistingAssetPolicy : uint8
{
	ReuseOrCreate,
	CreateUnique
};

struct FOLIAGEBAKERCORE_API FFoliageBakerGeneratedAssetOutputFolders
{
	// Long package paths such as /Game/Trees/Textures. Empty keeps the configured-folder behavior.
	FString TexturePackagePath;
	FString MaterialPackagePath;
};

enum class EFoliageBakerTextureMipMode : uint8
{
	Default,
	NormalizeXYZNormal,
	ImpostorOctaNormalMaskDepth
};

struct FOLIAGEBAKERCORE_API FFoliageBakerTextureAssetParams
{
	FString OutputFolderName;
	FString OutputPackagePathOverride;
	FString AssetNamePrefix;
	FString AssetNameSuffix;
	int32 Width = 0;
	int32 Height = 0;
	TextureCompressionSettings CompressionSettings = TC_Default;
	TextureGroup LODGroup = TEXTUREGROUP_World;
	bool bSRGB = true;
	float SemanticMaskMipCoverageThreshold = 0.0f;
	// Mip-0 atlas regions that must be filtered independently before being assembled into each lower mip.
	TArray<FIntRect> MipTileRects;
	FColor MipBackgroundColor = FColor(0, 0, 0, 0);
	EFoliageBakerTextureMipMode MipMode = EFoliageBakerTextureMipMode::Default;
	FString EmptyPixelsError = TEXT("No texture pixels were generated.");
};

struct FOLIAGEBAKERCORE_API FFoliageBakerPlaneAtlasTextureAssetParams
{
	FString OutputFolderName;
	FString OutputPackagePathOverride;
	FString AssetNamePrefix;
	FString AssetNameSuffix;
	FColor MipBackgroundColor = FColor(0, 0, 0, 0);
	TextureCompressionSettings CompressionSettings = TC_Default;
	TextureGroup LODGroup = TEXTUREGROUP_World;
	bool bSRGB = true;
	float SemanticMaskMipCoverageThreshold = 0.0f;
	FString EmptyPixelsError = TEXT("No atlas pixels were generated.");
};

struct FOLIAGEBAKERCORE_API FFoliageBakerMaterialInstanceAssetParams
{
	struct FTextureParameterValue
	{
		FName ParameterName = NAME_None;
		UTexture2D* Texture = nullptr;
	};

	struct FVectorParameterValue
	{
		FName ParameterName = NAME_None;
		FLinearColor Value = FLinearColor::Black;
	};

	struct FStaticSwitchParameterValue
	{
		FName ParameterName = NAME_None;
		bool bValue = false;
	};

	FString OutputFolderName;
	FString OutputPackagePathOverride;
	FString AssetNamePrefix;
	FString AssetNameSuffix;
	EFoliageBakerExistingAssetPolicy ExistingAssetPolicy = EFoliageBakerExistingAssetPolicy::ReuseOrCreate;
	FName BaseColorOpacityTextureParameterName = NAME_None;
	FName NormalDepthTextureParameterName = NAME_None;
	FName MixTextureParameterName = NAME_None;
	TArray<FTextureParameterValue> AdditionalTextureParameterValues;
	TArray<UE::FoliageBaker::MaterialResolver::FMaterialScalarParameterValue> ScalarParameterValues;
	TArray<FVectorParameterValue> VectorParameterValues;
	TArray<FStaticSwitchParameterValue> StaticSwitchParameterValues;
	TArray<FName> OwnedTextureParameterNames;
	TArray<FName> OwnedScalarParameterNames;
	TOptional<bool> TwoSidedOverride;
	FString MissingTemplateError = TEXT("A parent Material Instance Constant must be provided.");
};

struct FOLIAGEBAKERCORE_API FFoliageBakerStaticMeshAssetParams
{
	FString AssetNameSuffix;
	EFoliageBakerExistingAssetPolicy ExistingAssetPolicy = EFoliageBakerExistingAssetPolicy::ReuseOrCreate;
	int32 DesiredUVChannelCount = 2;
	// Used as a legacy/fallback identifier; generated slots are named after the assigned material asset.
	FName MaterialSlotName = FName(TEXT("BillboardProxy"));
	// Optional non-proxy material slots already referenced by the MeshDescription.
	TArray<FFoliageBakerMeshMaterialSlot> AdditionalMaterialSlots;
	bool bRecomputeNormals = false;
	bool bRecomputeTangents = false;
	int32 BaseLODModel = INDEX_NONE;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerSourceLODAssetParams
{
	EFoliageBakerMeshAssetOutputMode OutputMode = EFoliageBakerMeshAssetOutputMode::AddToSourceMeshLOD;
	int32 RequestedReplaceLODIndex = INDEX_NONE;
	int32 RequestedInsertAfterLODIndex = INDEX_NONE;
	int32 SourceLODIndex = 0;
	int32 DesiredUVChannelCount = 2;
	// Used as a legacy/fallback identifier; generated slots are named after the assigned material asset.
	FName MaterialSlotName = FName(TEXT("BillboardProxy"));
	// Optional non-proxy material slots already referenced by the MeshDescription.
	TArray<FFoliageBakerMeshMaterialSlot> AdditionalMaterialSlots;
	bool bRecomputeNormals = false;
	bool bRecomputeTangents = false;
	int32 BaseLODModel = INDEX_NONE;

	FName RebuildLODMetadataKey = NAME_None;
};


class FOLIAGEBAKERCORE_API FFoliageBakerAssetBuilder final
{
public:
	static bool BuildGeneratedAssetBasePath(
		const UStaticMesh& SourceStaticMesh,
		const FString& ConfiguredOutputFolder,
		const FString& AssetNamePrefix,
		const FString& AssetNameSuffix,
		FString& OutBasePackageName,
		FString& OutBaseAssetName,
		FString& OutError);

	static bool BuildGeneratedAssetBasePath(
		const UStaticMesh& SourceStaticMesh,
		const FString& ConfiguredOutputFolder,
		const FString& OutputPackagePathOverride,
		const FString& AssetNamePrefix,
		const FString& AssetNameSuffix,
		FString& OutBasePackageName,
		FString& OutBaseAssetName,
		FString& OutError);

	// Resolves folders from the materials actually used by the selected source LOD.
	// Material output follows those materials; texture output prefers their nearest project texture folder.
	static FFoliageBakerGeneratedAssetOutputFolders ResolveSourceLODAssetOutputFolders(
		const UStaticMesh& SourceStaticMesh,
		int32 LODIndex);

	static UTexture2D* CreateTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FFoliageBakerTextureAssetParams& Params,
		const TArray<FColor>& Pixels,
		FString& OutError);

	static UTexture2D* CreatePlaneAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FFoliageBakerPlaneAtlasTextureAssetParams& Params,
		const TArray<FColor>& Pixels,
		int32 AtlasWidth,
		int32 AtlasHeight,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError);

	static UMaterialInstanceConstant* CreateMaterialInstanceAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FFoliageBakerMaterialInstanceAssetParams& Params,
		UMaterialInstanceConstant* TemplateMaterialInstance,
		UTexture2D* BaseColorOpacityTexture,
		UTexture2D* NormalDepthTexture,
		UTexture2D* MixTexture,
		FString& OutError);

	static UStaticMesh* CreateStaticMeshAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FFoliageBakerStaticMeshAssetParams& Params,
		const FMeshDescription& MeshDescription,
		UMaterialInterface* ProxyMaterial,
		FString& OutError);

	static bool ValidateSourceMeshOutputTarget(
		const UStaticMesh& SourceStaticMesh,
		const FFoliageBakerSourceLODAssetParams& Params,
		FString& OutError);

	static bool InstallMeshDescriptionAsSourceMeshLOD(
		UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FFoliageBakerSourceLODAssetParams& Params,
		const FMeshDescription& MeshDescription,
		UMaterialInterface* ProxyMaterial,
		int32& OutLODIndex,
		FString& OutError);
};
