#include "FoliageBakerBillboardCloudsModule.h"

#include "FoliageBakerBillboardCloudsBaker.h"
#include "FoliageBakerBillboardCloudsSettings.h"
#include "FoliageBakerFeatureTool.h"
#include "FoliageBakerMeshOutputDialog.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceConstant.h"

#define LOCTEXT_NAMESPACE "FFoliageBakerBillboardCloudsModule"

void FFoliageBakerBillboardCloudsModule::StartupModule()
{
	EnsureToolSettings();
}

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
	FFoliageBakerFeatureControllerArgs ControllerArgs;
	ControllerArgs.SettingsObject = ToolSettings.Get();
	ControllerArgs.SourceStaticMeshes = &ToolSettings->SourceStaticMeshes;
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
		!ToolSettings->BillboardMaterialTemplate.IsNull(),
		FFoliageBakerBillboardCloudsBaker::HasAnyAtlasOutput(*ToolSettings),
		ToolSettings->SourceStaticMeshes);
}

void FFoliageBakerBillboardCloudsModule::Bake()
{
	EnsureToolSettings();
	if (ToolSettings->BillboardMaterialTemplate.IsNull()
		|| !ToolSettings->BillboardMaterialTemplate.LoadSynchronous())
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
				[this](UStaticMesh& StaticMesh)
				{
					return FFoliageBakerFeatureTool::MakeBakeItemResult(
						FFoliageBakerBillboardCloudsBaker::Bake(
							StaticMesh,
							*ToolSettings,
							FFoliageBakerMeshOutputSelector::CreateStatic(
								&FFoliageBakerMeshOutputDialog::OpenAfterBake)));
				}));
	FFoliageBakerFeatureTool::SyncCreatedAssetsToContentBrowser(
		BatchResult.CreatedAssets);
	FFoliageBakerFeatureTool::ShowMessage(FText::FromString(BatchResult.Report));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFoliageBakerBillboardCloudsModule, FoliageBakerBillboardClouds)
