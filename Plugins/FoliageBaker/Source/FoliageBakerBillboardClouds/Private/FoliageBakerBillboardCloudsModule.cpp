#include "FoliageBakerBillboardCloudsModule.h"

#include "FoliageBakerBillboardCloudsBaker.h"
#include "FoliageBakerBillboardCloudsSettings.h"
#include "FoliageBakerFeatureTool.h"
#include "FoliageBakerMeshOutputDialog.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceConstant.h"

#define LOCTEXT_NAMESPACE "FFoliageBakerBillboardCloudsModule"

void FFoliageBakerBillboardCloudsModule::ShutdownModule()
{
	FeatureController.Reset();
	ToolSettings.Reset();
}

void FFoliageBakerBillboardCloudsModule::EnsureToolSettings()
{
	if (!ToolSettings.IsValid())
	{
		FFoliageBakerFeatureTool::EnsureTransientSettings(ToolSettings);
	}
}

TSharedRef<SWidget> FFoliageBakerBillboardCloudsModule::CreateFeaturePanel()
{
	EnsureToolSettings();
	ToolSettings->SourceStaticMeshes.Reset();
	FFoliageBakerFeatureControllerArgs ControllerArgs;
	ControllerArgs.SettingsObject.Reset(ToolSettings.Get());
	ControllerArgs.GetSourceStaticMeshes =
		[Settings = ToolSettings]() -> TArray<TObjectPtr<UStaticMesh>>&
		{
			return Settings->SourceStaticMeshes;
		};
	ControllerArgs.BakeButtonText =
		LOCTEXT("UnifiedBakeBillboardCloudsButton", "Bake BillboardClouds");
	ControllerArgs.BakeButtonTooltip = LOCTEXT(
		"UnifiedBakeBillboardCloudsTooltip",
		"Bake BillboardClouds assets for every queued Static Mesh.");
	ControllerArgs.RequirementsHint = LOCTEXT(
		"UnifiedBakeRequirementsHint",
		"Select the Parent Material Instance, enable an atlas output, and queue at least one Static Mesh. Editor Preferences provides the initial default.");
	ControllerArgs.AddMeshesTransactionText = LOCTEXT(
		"AddBillboardCloudsSourceMeshesTransaction",
		"Add Foliage Baker BillboardClouds Source Meshes");
	ControllerArgs.ClearMeshesTransactionText = LOCTEXT(
		"ClearBillboardCloudsSourceMeshesTransaction",
		"Clear Foliage Baker BillboardClouds Source Meshes");
	ControllerArgs.bShowDetailsOptions = false;
	ControllerArgs.bShowPropertyMatrixButton = false;
	ControllerArgs.CanBake =
		FFoliageBakerFeaturePredicateDelegate::CreateRaw(
			this,
			&FFoliageBakerBillboardCloudsModule::CanBake);
	ControllerArgs.Bake =
		FFoliageBakerFeatureActionDelegate::CreateRaw(
			this,
			&FFoliageBakerBillboardCloudsModule::Bake);
	FeatureController = FFoliageBakerFeatureController::Create(ControllerArgs);
	return FeatureController->GetWidget();
}

bool FFoliageBakerBillboardCloudsModule::CanBake() const
{
	if (!ToolSettings.IsValid())
	{
		return false;
	}
	return FFoliageBakerFeatureTool::CanBakeFeature(
		FFoliageBakerFeatureTool::HasExistingAsset(
			ToolSettings->MaterialInstanceTemplate.ToSoftObjectPath()),
		FFoliageBakerBillboardCloudsBaker::HasAnyAtlasOutput(*ToolSettings),
		ToolSettings->SourceStaticMeshes);
}

void FFoliageBakerBillboardCloudsModule::Bake()
{
	EnsureToolSettings();
	const TStrongObjectPtr<UMaterialInstanceConstant> MaterialTemplate(
		ToolSettings->MaterialInstanceTemplate.LoadSynchronous());
	if (!MaterialTemplate)
	{
		FFoliageBakerFeatureTool::ShowMessage(
			LOCTEXT("MissingBillboardCloudsParentMaterial", "Select the Billboard Clouds Parent Material Instance in the current tool before baking."));
		return;
	}
	if (!FFoliageBakerBillboardCloudsBaker::HasAnyAtlasOutput(*ToolSettings))
	{
		FFoliageBakerFeatureTool::ShowMessage(
			LOCTEXT("NoBillboardCloudsAtlasOutputs", "Enable at least one Billboard Clouds atlas output before baking."));
		return;
	}
	if (!FFoliageBakerFeatureTool::HasAnyValidStaticMesh(
			ToolSettings->SourceStaticMeshes))
	{
		FFoliageBakerFeatureTool::ShowMessage(LOCTEXT(
			"NoStaticMeshSelectionForProxy",
			"Add one or more Static Mesh assets to the Billboard Clouds tool panel before clicking Bake."));
		return;
	}

	const FFoliageBakerFeatureBatchResult BatchResult =
		FFoliageBakerFeatureTool::RunBakeBatch(
			ToolSettings->SourceStaticMeshes,
			LOCTEXT(
				"CreatePlaneProxyMeshesSlowTask",
				"Creating Billboard Clouds plane proxy meshes..."),
			false,
			TEXT("\n\n"),
			FFoliageBakerBakeStaticMeshDelegate::CreateLambda(
				[MaterialTemplate, Settings = ToolSettings](UStaticMesh& StaticMesh)
				{
					return FFoliageBakerFeatureTool::MakeBakeItemResult(
						FFoliageBakerBillboardCloudsBaker::Bake(
							StaticMesh,
							*MaterialTemplate,
							*Settings,
							FFoliageBakerMeshOutputSelector::CreateStatic(
								&FFoliageBakerMeshOutputDialog::OpenForBake)));
				}));
	FFoliageBakerFeatureTool::SyncCreatedAssetsToContentBrowser(
		BatchResult.CreatedAssets);
	FFoliageBakerFeatureTool::ShowMessage(FText::FromString(BatchResult.Report));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFoliageBakerBillboardCloudsModule, FoliageBakerBillboardClouds)
