#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerAssetBuilder.h"
#include "FoliageBakerMeshOutput.h"

class UStaticMesh;

struct FOLIAGEBAKEREDITORCOMMON_API FFoliageBakerExistingAssetDecision
{
	EFoliageBakerExistingAssetPolicy ExistingAssetPolicy =
		EFoliageBakerExistingAssetPolicy::CreateOnly;
	int32 AssetNameVersion = 0;
};

class FOLIAGEBAKEREDITORCOMMON_API FFoliageBakerMeshOutputDialog final
{
public:
	static TOptional<FFoliageBakerMeshOutputSelection> OpenAfterBake(
		const UStaticMesh& SourceStaticMesh,
		int32 SourceLODIndex);
};

class FOLIAGEBAKEREDITORCOMMON_API FFoliageBakerExistingAssetDialog final
{
public:
	// Returns CreateOnly without opening a window when none of the requested
	// object paths exists. Cancel returns an unset optional with an empty error.
	static TOptional<FFoliageBakerExistingAssetDecision> OpenIfNeeded(
		const TArray<FFoliageBakerGeneratedAssetPath>& GeneratedAssets,
		FString& OutError);
};
