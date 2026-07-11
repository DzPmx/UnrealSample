#include "FoliageBakerBillboardCloudsSettings.h"

#include "Materials/MaterialInstanceConstant.h"

UFoliageBakerBillboardCloudsSettings::UFoliageBakerBillboardCloudsSettings()
{
	BillboardMaterialTemplate = TSoftObjectPtr<UMaterialInstanceConstant>(
		FSoftObjectPath(TEXT("/FoliageBaker/Materials/MR_Foliage_BillboardClouds.MR_Foliage_BillboardClouds")));
}
