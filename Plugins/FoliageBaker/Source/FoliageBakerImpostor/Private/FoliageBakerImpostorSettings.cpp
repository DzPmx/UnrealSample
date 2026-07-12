#include "FoliageBakerImpostorSettings.h"

#include "Materials/MaterialInstanceConstant.h"

UFoliageBakerImpostorSettings::UFoliageBakerImpostorSettings()
{
	MaterialInstanceTemplate = TSoftObjectPtr<UMaterialInstanceConstant>(
		FSoftObjectPath(TEXT("/FoliageBaker/Materials/MR_Foliage_Impostor.MR_Foliage_Impostor")));
}
