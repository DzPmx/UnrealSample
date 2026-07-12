#pragma once

#include "CoreMinimal.h"

class UMaterialInstanceConstant;
class UStaticMesh;
class UTexture2D;
class UFoliageBakerImpostorSettings;

struct FFoliageBakerImpostorBakeResult
{
	bool bSucceeded = false;
	UStaticMesh* ProxyMesh = nullptr;
	int32 SourceMeshLODIndex = INDEX_NONE;
	UTexture2D* BaseColorSdfTexture = nullptr;
	UTexture2D* NormalDepthTexture = nullptr;
	UTexture2D* MixTexture = nullptr;
	UMaterialInstanceConstant* MaterialInstance = nullptr;
	TArray<UObject*> CreatedAssets;
	FString Report;
};

class FFoliageBakerImpostorBaker final
{
public:
	static FFoliageBakerImpostorBakeResult Bake(
		UStaticMesh& SourceStaticMesh,
		UMaterialInstanceConstant& MaterialTemplate,
		const UFoliageBakerImpostorSettings& Settings);
};
