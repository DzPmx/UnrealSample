#include "FoliageBakerCardsSettings.h"

UFoliageBakerCardsSettings::UFoliageBakerCardsSettings()
{
	SourceStaticMeshes.Reset();
}

UFoliageBakerSingleBillboardSettings::UFoliageBakerSingleBillboardSettings()
{
	Mode = EFoliageBakerCardMode::SingleBillboard;
	bTrimUnusedAtlasSpace = true;
}

UFoliageBakerCrossCardsSettings::UFoliageBakerCrossCardsSettings()
{
	Mode = EFoliageBakerCardMode::CrossCards;
}

UFoliageBakerMultiBillboardSettings::UFoliageBakerMultiBillboardSettings()
{
	Mode = EFoliageBakerCardMode::MultiBillboard;
	bTrimUnusedAtlasSpace = true;
}
