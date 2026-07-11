#pragma once

#include "CoreMinimal.h"
#include "MaterialBakingStructures.h"


class FOLIAGEBAKERCORE_API FFoliageBakerMaterialBaker final
{
public:
	static bool BakeMaterials(
		const TArray<FMaterialData*>& MaterialSettings,
		const TArray<FMeshData*>& MeshSettings,
		TArray<FBakeOutput>& OutBakeOutputs,
		FString* OutError = nullptr);
};
