#include "BillboardCloudsEditorSettings.h"

#include "Materials/MaterialInstanceConstant.h"

UBillboardCloudsEditorSettings::UBillboardCloudsEditorSettings()
{
	BillboardMaterialTemplate = TSoftObjectPtr<UMaterialInstanceConstant>(
		FSoftObjectPath(TEXT("/Game/Demo/BillboardClouds/MR_Foliage_BillboardClouds.MR_Foliage_BillboardClouds")));
}
