#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerMeshOutput.h"

class UStaticMesh;

class FOLIAGEBAKEREDITORCOMMON_API FFoliageBakerMeshOutputDialog final
{
public:
	static TOptional<FFoliageBakerMeshOutputSelection> OpenAfterBake(
		const UStaticMesh& SourceStaticMesh,
		int32 SourceLODIndex);
};
