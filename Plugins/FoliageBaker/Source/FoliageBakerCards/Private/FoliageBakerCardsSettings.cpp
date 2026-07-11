#include "FoliageBakerCardsSettings.h"

#include "Materials/MaterialInstanceConstant.h"

UFoliageBakerCardsSettings::UFoliageBakerCardsSettings()
{
	SourceStaticMeshes.Reset();
}

UFoliageBakerSingleBillboardSettings::UFoliageBakerSingleBillboardSettings()
{
	Mode = EFoliageBakerCardMode::SingleBillboard;
	bTrimUnusedAtlasSpace = true;
	MaterialInstanceTemplate = TSoftObjectPtr<UMaterialInstanceConstant>(
		FSoftObjectPath(TEXT("/FoliageBaker/Materials/MR_Foliage_Billboard.MR_Foliage_Billboard")));
}

UFoliageBakerCrossCardsSettings::UFoliageBakerCrossCardsSettings()
{
	Mode = EFoliageBakerCardMode::CrossCards;
	MaterialInstanceTemplate = TSoftObjectPtr<UMaterialInstanceConstant>(
		FSoftObjectPath(TEXT("/FoliageBaker/Materials/MR_Foliage_Cross.MR_Foliage_Cross")));
}
