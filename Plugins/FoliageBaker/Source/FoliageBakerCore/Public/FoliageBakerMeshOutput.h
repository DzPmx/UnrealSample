#pragma once

#include "CoreMinimal.h"

class UStaticMesh;

enum class EFoliageBakerMeshAssetOutputMode : uint8
{
	SeparateMeshAsset,
	AddToSourceMeshLOD,
	InsertIntoSourceMeshLOD,
	ReplaceSourceMeshLOD
};

struct FOLIAGEBAKERCORE_API FFoliageBakerMeshOutputSelection
{
	EFoliageBakerMeshAssetOutputMode OutputMode =
		EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset;
	int32 ReplaceLODIndex = INDEX_NONE;
	int32 InsertAfterLODIndex = INDEX_NONE;
};

DECLARE_DELEGATE_RetVal_TwoParams(
	TOptional<FFoliageBakerMeshOutputSelection>,
	FFoliageBakerMeshOutputSelector,
	const UStaticMesh&,
	int32);
