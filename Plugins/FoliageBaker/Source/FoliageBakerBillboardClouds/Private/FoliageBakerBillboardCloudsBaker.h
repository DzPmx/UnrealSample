#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerMeshOutput.h"
#include "UObject/StrongObjectPtr.h"

class UFoliageBakerBillboardCloudsSettings;
class UMaterialInstanceConstant;
class UStaticMesh;

struct FFoliageBakerBillboardCloudsBakeResult
{
	bool bSucceeded = false;
	bool bCancelled = false;
	TArray<TStrongObjectPtr<UObject>> CreatedAssets;
	FString Report;
};

class FFoliageBakerBillboardCloudsBaker final
{
public:
	static bool HasAnyAtlasOutput(
		const UFoliageBakerBillboardCloudsSettings& Settings);

	static FFoliageBakerBillboardCloudsBakeResult Bake(
		UStaticMesh& StaticMesh,
		UMaterialInstanceConstant& MaterialTemplate,
		const UFoliageBakerBillboardCloudsSettings& Settings,
		const FFoliageBakerMeshOutputSelector& MeshOutputSelector);
};
