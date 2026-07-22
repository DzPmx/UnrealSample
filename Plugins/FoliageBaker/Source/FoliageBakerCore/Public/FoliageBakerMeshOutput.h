#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
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

struct FOLIAGEBAKERCORE_API FFoliageBakerMeshMaterialSlot
{
	FName MaterialSlotName = NAME_None;
	UMaterialInterface* Material = nullptr;
};

DECLARE_DELEGATE_RetVal_TwoParams(
	TOptional<FFoliageBakerMeshOutputSelection>,
	FFoliageBakerMeshOutputSelector,
	const UStaticMesh&,
	int32);
