#include "FoliageBakerMaterialBaker.h"

#include "IMaterialBakingModule.h"
#include "Modules/ModuleManager.h"

bool FFoliageBakerMaterialBaker::BakeMaterials(
	const TArray<FMaterialData*>& MaterialSettings,
	const TArray<FMeshData*>& MeshSettings,
	TArray<FBakeOutput>& OutBakeOutputs,
	FString* OutError)
{
	OutBakeOutputs.Reset();
	if (OutError)
	{
		OutError->Reset();
	}

	if (MaterialSettings.IsEmpty() || MaterialSettings.Num() != MeshSettings.Num())
	{
		if (OutError)
		{
			*OutError = TEXT("Material and mesh bake request counts must be equal and non-zero.");
		}
		return false;
	}

	IMaterialBakingModule& MaterialBakingModule =
		FModuleManager::LoadModuleChecked<IMaterialBakingModule>(TEXT("MaterialBaking"));
	MaterialBakingModule.SetLinearBake(true);
	MaterialBakingModule.BakeMaterials(MaterialSettings, MeshSettings, OutBakeOutputs);

	if (OutBakeOutputs.IsEmpty())
	{
		if (OutError)
		{
			*OutError = TEXT("Unreal MaterialBaking returned no outputs.");
		}
		return false;
	}

	return true;
}
