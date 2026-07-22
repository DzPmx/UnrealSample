#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerMeshOutput.h"

class UFoliageBakerBillboardCloudsSettings;
class UStaticMesh;

struct FFoliageBakerBillboardCloudsBakeResult
{
	bool bSucceeded = false;
	bool bCancelled = false;
	TArray<UObject*> CreatedAssets;
	FString Report;
};

class FFoliageBakerBillboardCloudsBaker final
{
public:
	static bool HasAnyAtlasOutput(
		const UFoliageBakerBillboardCloudsSettings& Settings);

	static FFoliageBakerBillboardCloudsBakeResult Bake(
		UStaticMesh& StaticMesh,
		const UFoliageBakerBillboardCloudsSettings& Settings,
		const FFoliageBakerMeshOutputSelector& MeshOutputSelector);
};
