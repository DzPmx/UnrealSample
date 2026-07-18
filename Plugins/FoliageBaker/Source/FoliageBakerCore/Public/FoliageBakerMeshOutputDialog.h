#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerAssetBuilder.h"

class UStaticMesh;

struct FOLIAGEBAKERCORE_API FFoliageBakerMeshOutputSelection
{
	EFoliageBakerMeshAssetOutputMode OutputMode = EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset;
	int32 ReplaceLODIndex = INDEX_NONE;
	int32 InsertAfterLODIndex = INDEX_NONE;
};

class FOLIAGEBAKERCORE_API FFoliageBakerMeshOutputDialog final
{
public:
	static TOptional<FFoliageBakerMeshOutputSelection> OpenAfterBake(
		const UStaticMesh& SourceStaticMesh,
		int32 SourceLODIndex);
};
