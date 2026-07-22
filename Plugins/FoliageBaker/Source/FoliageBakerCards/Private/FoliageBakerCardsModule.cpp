#include "FoliageBakerCardsModule.h"

#include "Engine/StaticMesh.h"
#include "FoliageBakerCardBaker.h"
#include "FoliageBakerCardsSettings.h"
#include "FoliageBakerFeatureTool.h"
#include "FoliageBakerMeshOutputDialog.h"
#include "Materials/MaterialInstanceConstant.h"

#define LOCTEXT_NAMESPACE "FFoliageBakerCardsModule"

namespace
{
	bool IsSingleBillboardMode(const EFoliageBakerCardMode Mode)
	{
		return Mode == EFoliageBakerCardMode::SingleBillboard;
	}

	bool IsMultiBillboardMode(const EFoliageBakerCardMode Mode)
	{
		return Mode == EFoliageBakerCardMode::MultiBillboard;
	}

	TSoftObjectPtr<UMaterialInstanceConstant> GetConfiguredMaterialTemplate(
		const UFoliageBakerCardsSettings& ToolSettings)
	{
		if (IsSingleBillboardMode(ToolSettings.Mode)
			&& ToolSettings.BillboardMode == EFoliageBakerBillboardMode::DoublePlanes)
		{
			const UFoliageBakerSingleBillboardSettings* BillboardSettings =
				Cast<UFoliageBakerSingleBillboardSettings>(&ToolSettings);
			return BillboardSettings
				? BillboardSettings->DoublePlanesMaterialInstanceTemplate
				: TSoftObjectPtr<UMaterialInstanceConstant>();
		}
		return ToolSettings.MaterialInstanceTemplate;
	}

	FFoliageBakerCardBakeRequest BuildRequest(
		UStaticMesh& StaticMesh,
		UMaterialInstanceConstant& MaterialTemplate,
		const UFoliageBakerCardsSettings& Settings)
	{
		FFoliageBakerCardBakeRequest Request;
		Request.SourceStaticMesh = &StaticMesh;
		Request.MaterialTemplate = &MaterialTemplate;
		Request.SourceLODIndex = Settings.SourceLODIndex;
		Request.Mode = Settings.Mode;
		Request.BillboardPlaneMode = Settings.BillboardMode;
		Request.SingleCaptureAxis = Settings.SingleCaptureAxis;
		Request.CrossCardPlaneCount = Settings.CrossCardPlaneCount;
		Request.CrossCardGeometryMode = Settings.CrossCardFaceMode;
		Request.TrunkMaterialKeywords = Settings.TrunkMaterialKeywords;
		Request.LeafMaterialKeywords = Settings.LeafMaterialKeywords;
		Request.MultiBillboardClusterCount = Settings.MultiBillboardClusterCount;
		Request.MultiBillboardsPerCluster = Settings.MultiBillboardsPerCluster;
		Request.bIncludeReducedTrunk = Settings.bIncludeReducedTrunk;
		Request.TrunkTrianglePercentage = Settings.TrunkTrianglePercentage;
		Request.TextureResolution = IsSingleBillboardMode(Settings.Mode)
			? Settings.SingleTextureResolution
			: IsMultiBillboardMode(Settings.Mode)
				? Settings.MultiBillboardTextureResolution
				: Settings.CrossTextureResolution;
		Request.AlphaCropGuardPixels = Settings.AlphaCropGuardPixels;
		Request.bPreserveAlphaMaskValues = Settings.bPreserveAlphaMaskValues;
		Request.MipMaskCoverageThreshold = Settings.MipMaskCoverageThreshold;
		Request.bTrimUnusedAtlasSpace = Settings.bTrimUnusedAtlasSpace;
		Request.bBakeBaseColorOpacity = Settings.bBakeBaseColorOpacity;
		Request.bBakeNormalDepth = Settings.bBakeNormalDepth;
		Request.bBakeMix = Settings.bBakeMix;
		Request.bBakeUpperHemisphereL1Visibility =
			IsSingleBillboardMode(Settings.Mode)
			&& Settings.bBakeUpperHemisphereL1Visibility;
		Request.UpperHemisphereL1TextureResolution =
			Settings.UpperHemisphereL1TextureResolution;
		Request.UpperHemisphereL1SampleCount =
			Settings.UpperHemisphereL1SampleCount;
		Request.UpperHemisphereL1ShadowMapResolution =
			Settings.UpperHemisphereL1ShadowMapResolution;
		Request.BaseColorOpacityTextureParameterName = Settings.BaseColorOpacityTextureParameterName;
		Request.NormalDepthTextureParameterName = Settings.NormalDepthTextureParameterName;
		Request.MixTextureParameterName = Settings.MixTextureParameterName;
		Request.UpperHemisphereL1VisibilityTextureParameterName =
			Settings.UpperHemisphereL1VisibilityTextureParameterName;
		Request.LeafRoughnessParameterName = Settings.LeafRoughnessParameterName;
		Request.LeafSpecularParameterName = Settings.LeafSpecularParameterName;
		Request.TrunkRoughnessParameterName = Settings.TrunkRoughnessParameterName;
		Request.TrunkSpecularParameterName = Settings.TrunkSpecularParameterName;
		Request.TextureOutputFolderName = Settings.TextureOutputFolderName;
		Request.MaterialOutputFolderName = Settings.MaterialOutputFolderName;
		Request.bPlaceGeneratedAssetsNearReplacedLODAssets =
			Settings.bPlaceGeneratedAssetsNearReplacedLODAssets;
		Request.TextureNamePrefix = Settings.TextureNamePrefix;
		Request.BaseColorOpacityTextureSuffix = Settings.BaseColorOpacityTextureSuffix;
		Request.NormalDepthTextureSuffix = Settings.NormalDepthTextureSuffix;
		Request.MixTextureSuffix = Settings.MixTextureSuffix;
		Request.UpperHemisphereL1VisibilityTextureSuffix =
			Settings.UpperHemisphereL1VisibilityTextureSuffix;
		Request.MaterialInstanceNamePrefix = Settings.MaterialInstanceNamePrefix;
		Request.MaterialInstanceNameSuffix = Settings.MaterialInstanceNameSuffix;
		return Request;
	}
}

void FFoliageBakerCardsModule::ShutdownModule()
{
	SingleBillboardController.Reset();
	CrossCardsController.Reset();
	MultiBillboardController.Reset();
	SingleBillboardSettings.Reset();
	CrossCardsSettings.Reset();
	MultiBillboardSettings.Reset();
}

void FFoliageBakerCardsModule::EnsureToolSettings(const EFoliageBakerCardMode Mode)
{
	TStrongObjectPtr<UFoliageBakerCardsSettings>* Settings = IsSingleBillboardMode(Mode)
		? &SingleBillboardSettings
		: IsMultiBillboardMode(Mode)
			? &MultiBillboardSettings
			: &CrossCardsSettings;
	if (!Settings->IsValid())
	{
		if (IsSingleBillboardMode(Mode))
		{
			FFoliageBakerFeatureTool::EnsureTransientSettings<
				UFoliageBakerCardsSettings,
				UFoliageBakerSingleBillboardSettings>(
					*Settings,
					FName(TEXT("FoliageBakerSingleBillboardSettings")));
		}
		else if (IsMultiBillboardMode(Mode))
		{
			FFoliageBakerFeatureTool::EnsureTransientSettings<
				UFoliageBakerCardsSettings,
				UFoliageBakerMultiBillboardSettings>(
					*Settings,
					FName(TEXT("FoliageBakerMultiBillboardSettings")));
		}
		else
		{
			FFoliageBakerFeatureTool::EnsureTransientSettings<
				UFoliageBakerCardsSettings,
				UFoliageBakerCrossCardsSettings>(
					*Settings,
					FName(TEXT("FoliageBakerCrossCardsSettings")));
		}
	}
	(*Settings)->Mode = Mode;
}

UFoliageBakerCardsSettings* FFoliageBakerCardsModule::GetToolSettings(const EFoliageBakerCardMode Mode) const
{
	return IsSingleBillboardMode(Mode)
		? SingleBillboardSettings.Get()
		: IsMultiBillboardMode(Mode)
			? MultiBillboardSettings.Get()
			: CrossCardsSettings.Get();
}

TSharedPtr<FFoliageBakerFeatureController>& FFoliageBakerCardsModule::GetFeatureController(
	const EFoliageBakerCardMode Mode)
{
	return IsSingleBillboardMode(Mode)
		? SingleBillboardController
		: IsMultiBillboardMode(Mode)
			? MultiBillboardController
			: CrossCardsController;
}

TSharedRef<SWidget> FFoliageBakerCardsModule::CreateFeaturePanel(const EFoliageBakerCardMode Mode)
{
	EnsureToolSettings(Mode);
	UFoliageBakerCardsSettings* Settings = GetToolSettings(Mode);

	FFoliageBakerFeatureControllerArgs ControllerArgs;
	ControllerArgs.SettingsObject = Settings;
	ControllerArgs.SourceStaticMeshes = &Settings->SourceStaticMeshes;
	ControllerArgs.BakeButtonText = IsSingleBillboardMode(Mode)
		? LOCTEXT("BakeBillboardButton", "Bake Billboard")
		: IsMultiBillboardMode(Mode)
			? LOCTEXT("BakeMultiBillboardButton", "Bake MultiBillboard")
			: LOCTEXT("BakeCrossCardsButton", "Bake Cross Cards");
	ControllerArgs.BakeButtonTooltip = IsSingleBillboardMode(Mode)
		? LOCTEXT("BakeBillboardTooltip", "Bake the selected Single Plane or Double Planes Billboard mode for every queued Static Mesh.")
		: IsMultiBillboardMode(Mode)
			? LOCTEXT("BakeMultiBillboardTooltip", "Bake clustered leaf Billboards and, when enabled, retain and simplify the non-leaf trunk and branch geometry with its original materials.")
			: LOCTEXT("BakeCrossCardsTooltip", "Bake one Cross Cards asset for every queued Static Mesh.");
	ControllerArgs.RequirementsHint = LOCTEXT(
		"BakeRequirementsHint",
		"Select the Parent Material Instance required by the current mode and queue at least one Static Mesh. Editor Preferences provides the initial default.");
	ControllerArgs.AddMeshesTransactionText =
		LOCTEXT("AddCardsSourceMeshesTransaction", "Add Foliage Baker Card Source Meshes");
	ControllerArgs.ClearMeshesTransactionText =
		LOCTEXT("ClearCardsSourceMeshesTransaction", "Clear Foliage Baker Card Source Meshes");
	ControllerArgs.CanBake =
		FFoliageBakerFeaturePredicateDelegate::CreateLambda(
			[this, Mode]() { return CanBake(Mode); });
	ControllerArgs.Bake =
		FFoliageBakerFeatureActionDelegate::CreateLambda(
			[this, Mode]() { Bake(Mode); });

	TSharedPtr<FFoliageBakerFeatureController>& Controller = GetFeatureController(Mode);
	Controller = FFoliageBakerFeatureController::Create(ControllerArgs);
	return Controller->GetWidget();
}

bool FFoliageBakerCardsModule::CanBake(const EFoliageBakerCardMode Mode) const
{
	const UFoliageBakerCardsSettings* Settings = GetToolSettings(Mode);
	if (!Settings)
	{
		return false;
	}
	const TSoftObjectPtr<UMaterialInstanceConstant> MaterialTemplate =
		GetConfiguredMaterialTemplate(*Settings);
	const bool bHasLeafKeyword = !IsMultiBillboardMode(Mode)
		|| Settings->LeafMaterialKeywords.ContainsByPredicate([](const FString& Keyword)
		{
			return !Keyword.TrimStartAndEnd().IsEmpty();
		});
	return FFoliageBakerFeatureTool::CanBakeFeature(
		!MaterialTemplate.IsNull() && bHasLeafKeyword,
		Settings->bBakeBaseColorOpacity
			|| Settings->bBakeNormalDepth
			|| Settings->bBakeMix
			|| (IsSingleBillboardMode(Mode)
				&& Settings->bBakeUpperHemisphereL1Visibility),
		Settings->SourceStaticMeshes);
}

void FFoliageBakerCardsModule::Bake(const EFoliageBakerCardMode Mode)
{
	EnsureToolSettings(Mode);
	UFoliageBakerCardsSettings* Settings = GetToolSettings(Mode);
	TSoftObjectPtr<UMaterialInstanceConstant> ConfiguredMaterialTemplate =
		GetConfiguredMaterialTemplate(*Settings);
	UMaterialInstanceConstant* MaterialTemplate = ConfiguredMaterialTemplate.LoadSynchronous();
	if (!MaterialTemplate)
	{
		FFoliageBakerFeatureTool::ShowMessage(LOCTEXT(
			"MissingTemplate",
			"Select the Parent Material Instance required by the current Billboard, Cross Cards, or MultiBillboard mode before baking."));
		return;
	}

	const FFoliageBakerFeatureBatchResult BatchResult =
		FFoliageBakerFeatureTool::RunBakeBatch(
			Settings->SourceStaticMeshes,
			LOCTEXT("BakeCardsSlowTask", "Baking foliage cards..."),
			true,
			TEXT("\n"),
			FFoliageBakerBakeStaticMeshDelegate::CreateLambda(
				[Settings, MaterialTemplate](UStaticMesh& StaticMesh)
				{
					const FFoliageBakerCardBakeResult Result =
						FFoliageBakerCardBaker::Bake(
							BuildRequest(StaticMesh, *MaterialTemplate, *Settings),
							FFoliageBakerMeshOutputSelector::CreateStatic(
								&FFoliageBakerMeshOutputDialog::OpenAfterBake));
					return FFoliageBakerFeatureTool::MakeBakeItemResult(Result);
				}));

	FFoliageBakerFeatureTool::SyncCreatedAssetsToContentBrowser(BatchResult.CreatedAssets);
	FFoliageBakerFeatureTool::ShowBatchSummary(
		BatchResult,
		LOCTEXT(
			"BakeCardsSummary",
			"Foliage Baker completed {0} of {1} asset(s).\n\n{2}"));
}

IMPLEMENT_MODULE(FFoliageBakerCardsModule, FoliageBakerCards)

#undef LOCTEXT_NAMESPACE
