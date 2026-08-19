#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerMeshOutput.h"
#include "UObject/StrongObjectPtr.h"

class UMaterialInstanceConstant;
class UStaticMesh;
class UTexture2D;
class UFoliageBakerImpostorSettings;

struct FFoliageBakerImpostorBakeResult
{
	bool bSucceeded = false;
	bool bCancelled = false;
	TStrongObjectPtr<UStaticMesh> ProxyMesh;
	int32 SourceMeshLODIndex = INDEX_NONE;
	TStrongObjectPtr<UTexture2D> BaseColorSdfTexture;
	TStrongObjectPtr<UTexture2D> NormalMaskDepthTexture;
	TStrongObjectPtr<UTexture2D> MixTexture;
	TStrongObjectPtr<UMaterialInstanceConstant> MaterialInstance;
	TArray<TStrongObjectPtr<UObject>> CreatedAssets;
	FString Report;
};

class FFoliageBakerImpostorBaker final
{
public:
	static FFoliageBakerImpostorBakeResult Bake(
		UStaticMesh& SourceStaticMesh,
		UMaterialInstanceConstant& MaterialTemplate,
		const UFoliageBakerImpostorSettings& Settings,
		const FFoliageBakerMeshOutputSelector& MeshOutputSelector);
};
