#include "FoliageBakerImpostorModule.h"

#include "Engine/StaticMesh.h"
#include "FoliageBakerFeatureTool.h"
#include "FoliageBakerImpostorBaker.h"
#include "FoliageBakerImpostorSettings.h"
#include "Materials/MaterialInstanceConstant.h"

#define LOCTEXT_NAMESPACE "FFoliageBakerImpostorModule"

void FFoliageBakerImpostorModule::ShutdownModule()
{
	FeatureController.Reset();
	ToolSettings.Reset();
}

void FFoliageBakerImpostorModule::EnsureToolSettings()
{
	if (!ToolSettings.IsValid())
	{
		FFoliageBakerFeatureTool::EnsureTransientSettings(
			ToolSettings,
			FName(TEXT("FoliageBakerImpostorSettings")));
	}
}

TSharedRef<SWidget> FFoliageBakerImpostorModule::CreateFeaturePanel()
{
	EnsureToolSettings();
	FFoliageBakerFeatureControllerArgs ControllerArgs;
	ControllerArgs.SettingsObject = ToolSettings.Get();
	ControllerArgs.SourceStaticMeshes = &ToolSettings->SourceStaticMeshes;
	ControllerArgs.BakeButtonText = LOCTEXT("BakeImpostorButton", "Bake Impostor");
	ControllerArgs.BakeButtonTooltip =
		LOCTEXT("BakeImpostorTooltip", "Bake one Impostor asset for every queued Static Mesh.");
	ControllerArgs.RequirementsHint = LOCTEXT(
		"BakeRequirementsHint",
		"Configure the Parent Material Instance in Editor Preferences and queue at least one Static Mesh.");
	ControllerArgs.AddMeshesTransactionText =
		LOCTEXT("AddImpostorSourceMeshesTransaction", "Add Foliage Baker Impostor Source Meshes");
	ControllerArgs.ClearMeshesTransactionText =
		LOCTEXT("ClearImpostorSourceMeshesTransaction", "Clear Foliage Baker Impostor Source Meshes");
	ControllerArgs.CanBake =
		FFoliageBakerFeaturePredicateDelegate::CreateRaw(
			this,
			&FFoliageBakerImpostorModule::CanBake);
	ControllerArgs.Bake =
		FFoliageBakerFeatureActionDelegate::CreateRaw(
			this,
			&FFoliageBakerImpostorModule::Bake);
	FeatureController = FFoliageBakerFeatureController::Create(ControllerArgs);
	return FeatureController->GetWidget();
}

bool FFoliageBakerImpostorModule::CanBake() const
{
	if (!ToolSettings.IsValid())
	{
		return false;
	}
	const UFoliageBakerImpostorSettings* EditorPreferences = GetDefault<UFoliageBakerImpostorSettings>();
	return FFoliageBakerFeatureTool::CanBakeFeature(
		EditorPreferences && !EditorPreferences->MaterialInstanceTemplate.IsNull(),
		ToolSettings->bBakeBaseColorSdf
			|| ToolSettings->bBakeNormalDepth
			|| ToolSettings->bBakeMix,
		ToolSettings->SourceStaticMeshes);
}

void FFoliageBakerImpostorModule::Bake()
{
	EnsureToolSettings();
	const UFoliageBakerImpostorSettings* EditorPreferences = GetDefault<UFoliageBakerImpostorSettings>();
	UMaterialInstanceConstant* MaterialTemplate = EditorPreferences
		? EditorPreferences->MaterialInstanceTemplate.LoadSynchronous()
		: nullptr;
	if (!MaterialTemplate)
	{
		FFoliageBakerFeatureTool::ShowMessage(LOCTEXT(
			"MissingTemplate",
			"Configure the Parent Material Instance in Editor Preferences before baking."));
		return;
	}

	const FFoliageBakerFeatureBatchResult BatchResult =
		FFoliageBakerFeatureTool::RunBakeBatch(
			ToolSettings->SourceStaticMeshes,
			LOCTEXT("BakeImpostorSlowTask", "Baking foliage Impostors..."),
			true,
			TEXT("\n"),
			FFoliageBakerBakeStaticMeshDelegate::CreateLambda(
				[this, MaterialTemplate](UStaticMesh& StaticMesh)
				{
					const FFoliageBakerImpostorBakeResult Result =
						FFoliageBakerImpostorBaker::Bake(
							StaticMesh,
							*MaterialTemplate,
							*ToolSettings);
					return FFoliageBakerFeatureTool::MakeBakeItemResult(Result);
				}));

	FFoliageBakerFeatureTool::SyncCreatedAssetsToContentBrowser(BatchResult.CreatedAssets);
	FFoliageBakerFeatureTool::ShowBatchSummary(
		BatchResult,
		LOCTEXT(
			"BakeImpostorSummary",
			"Foliage Baker completed {0} of {1} Impostor asset(s).\n\n{2}"));
}

IMPLEMENT_MODULE(FFoliageBakerImpostorModule, FoliageBakerImpostor)

#undef LOCTEXT_NAMESPACE
