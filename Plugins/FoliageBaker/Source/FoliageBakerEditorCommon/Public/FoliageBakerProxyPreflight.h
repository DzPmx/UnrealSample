#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerAssetBuilder.h"
#include "FoliageBakerMeshOutputDialog.h"

class UStaticMesh;

enum class EFoliageBakerGeneratedAssetLocation : uint8
{
	Texture,
	Material
};

struct FOLIAGEBAKEREDITORCOMMON_API FFoliageBakerGeneratedAssetSpec
{
	FString DisplayName;
	FString ConfiguredOutputFolder;
	FString AssetNamePrefix;
	FString AssetNameSuffix;
	EFoliageBakerGeneratedAssetLocation Location =
		EFoliageBakerGeneratedAssetLocation::Texture;
};

struct FOLIAGEBAKEREDITORCOMMON_API FFoliageBakerProxyPreflightRequest
{
	FFoliageBakerSourceLODAssetParams SourceLODAssetParams;
	TArray<FFoliageBakerGeneratedAssetSpec> GeneratedAssets;
	FString SeparateMeshAssetSuffix;
	bool bPlaceGeneratedAssetsNearReplacedLODAssets = false;
};

struct FOLIAGEBAKEREDITORCOMMON_API FFoliageBakerProxyPreflightResult
{
	FFoliageBakerMeshOutputSelection MeshOutputSelection;
	FFoliageBakerSourceLODAssetParams SourceLODAssetParams;
	FFoliageBakerGeneratedAssetOutputFolders OutputFolders;
	FFoliageBakerExistingAssetDecision ExistingAssetDecision;
	FString SeparateMeshAssetSuffix;
};

enum class EFoliageBakerProxyPreflightStatus : uint8
{
	Succeeded,
	Failed,
	Cancelled
};

class FOLIAGEBAKEREDITORCOMMON_API FFoliageBakerProxyPreflight final
{
public:
	static EFoliageBakerProxyPreflightStatus Run(
		const UStaticMesh& SourceStaticMesh,
		const FFoliageBakerProxyPreflightRequest& Request,
		const FFoliageBakerMeshOutputSelector& MeshOutputSelector,
		FFoliageBakerProxyPreflightResult& OutResult,
		FString& OutError);
};
