#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureDefines.h"
#include "FoliageBakerMaterialResolver.h"
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

enum class EFoliageBakerMeshAssetOutputMode : uint8
{
	SeparateMeshAsset,
	AddToSourceMeshLOD,
	InsertIntoSourceMeshLOD,
	ReplaceSourceMeshLOD
};

struct FOLIAGEBAKERCORE_API FFoliageBakerTextureAssetParams
{
	FString OutputFolderName;
	FString AssetNamePrefix;
	FString AssetNameSuffix;
	int32 Width = 0;
	int32 Height = 0;
	TextureCompressionSettings CompressionSettings = TC_Default;
	TextureGroup LODGroup = TEXTUREGROUP_World;
	bool bSRGB = true;
	float AlphaCoverageThreshold = 0.0f;
	// Mip-0 atlas regions that must be filtered independently before being assembled into each lower mip.
	TArray<FIntRect> MipTileRects;
	FColor MipBackgroundColor = FColor(0, 0, 0, 0);
	bool bNormalizeMipNormals = false;
	FString EmptyPixelsError = TEXT("No texture pixels were generated.");
};

struct FOLIAGEBAKERCORE_API FFoliageBakerMaterialInstanceAssetParams
{
	FString OutputFolderName;
	FString AssetNamePrefix;
	FString AssetNameSuffix;
	EFoliageBakerExistingAssetPolicy ExistingAssetPolicy = EFoliageBakerExistingAssetPolicy::ReuseOrCreate;
	FName BaseColorOpacityTextureParameterName = NAME_None;
	FName NormalDepthTextureParameterName = NAME_None;
	FName MixTextureParameterName = NAME_None;
	TArray<UE::FoliageBaker::MaterialResolver::FMaterialScalarParameterValue> ScalarParameterValues;
	TOptional<bool> TwoSidedOverride;
	FString MissingTemplateError = TEXT("A parent Material Instance Constant must be configured in Editor Preferences.");
};

struct FOLIAGEBAKERCORE_API FFoliageBakerStaticMeshAssetParams
{
	FString AssetNameSuffix;
	EFoliageBakerExistingAssetPolicy ExistingAssetPolicy = EFoliageBakerExistingAssetPolicy::ReuseOrCreate;
	int32 DesiredUVChannelCount = 2;
	FName MaterialSlotName = FName(TEXT("BillboardProxy"));
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
	FName MaterialSlotName = FName(TEXT("BillboardProxy"));
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

	static UTexture2D* CreateTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FFoliageBakerTextureAssetParams& Params,
		const TArray<FColor>& Pixels,
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
