#include "FoliageBakerCardsSettings.h"

#include "Materials/MaterialInstanceConstant.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	const FSoftObjectPath DeletedBillboardParentPath(
		TEXT("/FoliageBaker/Materials/MR_Foliage_Billboard.MR_Foliage_Billboard"));
	const FSoftObjectPath BillboardParentPath(
		TEXT("/FoliageBaker/Materials/MR_Foliage_SingleBillboard.MR_Foliage_SingleBillboard"));
}

UFoliageBakerCardsSettings::UFoliageBakerCardsSettings()
{
	SourceStaticMeshes.Reset();
}

void UFoliageBakerCardsSettings::PostInitProperties()
{
	Super::PostInitProperties();
	RedirectDeletedParentMaterialTemplate();
}

void UFoliageBakerCardsSettings::PostReloadConfig(FProperty* PropertyThatWasLoaded)
{
	Super::PostReloadConfig(PropertyThatWasLoaded);
	RedirectDeletedParentMaterialTemplate();
}

void UFoliageBakerCardsSettings::RedirectDeletedParentMaterialTemplate()
{
	if (MaterialInstanceTemplate.ToSoftObjectPath() == DeletedBillboardParentPath)
	{
		MaterialInstanceTemplate =
			TSoftObjectPtr<UMaterialInstanceConstant>(BillboardParentPath);
	}
}

UFoliageBakerBillboardSettings::UFoliageBakerBillboardSettings()
{
	Mode = EFoliageBakerCardMode::Billboard;
	MaterialInstanceTemplate =
		TSoftObjectPtr<UMaterialInstanceConstant>(BillboardParentPath);
}

UFoliageBakerCrossCardsSettings::UFoliageBakerCrossCardsSettings()
{
	Mode = EFoliageBakerCardMode::CrossCards;
}

UFoliageBakerMultiBillboardSettings::UFoliageBakerMultiBillboardSettings()
{
	Mode = EFoliageBakerCardMode::MultiBillboard;
	MaterialInstanceTemplate =
		TSoftObjectPtr<UMaterialInstanceConstant>(BillboardParentPath);
}
