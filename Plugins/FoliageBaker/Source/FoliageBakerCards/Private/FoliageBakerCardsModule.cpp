#include "FoliageBakerCardsModule.h"

#include "Engine/StaticMesh.h"
#include "FoliageBakerCardBaker.h"
#include "FoliageBakerCardsSettings.h"
#include "FoliageBakerFeatureTool.h"
#include "Materials/MaterialInstanceConstant.h"

#define LOCTEXT_NAMESPACE "FFoliageBakerCardsModule"

namespace
{
	constexpr int32 MinCardPlaneCount = 2;
	constexpr int32 MaxCardPlaneCount = 5;
	constexpr int32 MinTextureResolution = 256;
	constexpr int32 MaxTextureResolution = 4096;
	constexpr int32 MinAlphaCropGuardPixels = 2;
	constexpr int32 MaxAlphaCropGuardPixels = 16;

	bool IsSingleBillboardMode(const EFoliageBakerCardMode Mode)
	{
		return Mode == EFoliageBakerCardMode::SingleBillboard;
	}

	const UFoliageBakerCardsSettings* GetEditorPreferences(const EFoliageBakerCardMode Mode)
	{
		if (IsSingleBillboardMode(Mode))
		{
			return GetDefault<UFoliageBakerSingleBillboardSettings>();
		}
		return GetDefault<UFoliageBakerCrossCardsSettings>();
	}

	EFoliageBakerCardBakeMode ToCoreMode(const EFoliageBakerCardMode Mode)
	{
		return Mode == EFoliageBakerCardMode::CrossCards
			? EFoliageBakerCardBakeMode::CrossCards
			: EFoliageBakerCardBakeMode::SingleBillboard;
	}

	EFoliageBakerCaptureAxis ToCoreAxis(const EFoliageBakerSingleCaptureAxis Axis)
	{
		switch (Axis)
		{
		case EFoliageBakerSingleCaptureAxis::NegativeX: return EFoliageBakerCaptureAxis::NegativeX;
		case EFoliageBakerSingleCaptureAxis::PositiveY: return EFoliageBakerCaptureAxis::PositiveY;
		case EFoliageBakerSingleCaptureAxis::NegativeY: return EFoliageBakerCaptureAxis::NegativeY;
		case EFoliageBakerSingleCaptureAxis::PositiveX:
		default: return EFoliageBakerCaptureAxis::PositiveX;
		}
	}

	EFoliageBakerBillboardPlaneMode ToCoreBillboardPlaneMode(
		const EFoliageBakerBillboardMode Mode)
	{
		return Mode == EFoliageBakerBillboardMode::DoublePlanes
			? EFoliageBakerBillboardPlaneMode::DoublePlanes
			: EFoliageBakerBillboardPlaneMode::SinglePlane;
	}

	EFoliageBakerCrossCardGeometryMode ToCoreGeometryMode(const EFoliageBakerCrossCardFaceMode FaceMode)
	{
		return FaceMode == EFoliageBakerCrossCardFaceMode::SeparateOneSidedFaces
			? EFoliageBakerCrossCardGeometryMode::SeparateOneSidedFaces
			: EFoliageBakerCrossCardGeometryMode::TwoSidedTwoUVs;
	}

	TSoftObjectPtr<UMaterialInstanceConstant> GetConfiguredMaterialTemplate(
		const UFoliageBakerCardsSettings& ToolSettings,
		const UFoliageBakerCardsSettings* EditorPreferences)
	{
		if (!EditorPreferences)
		{
			return {};
		}
		if (IsSingleBillboardMode(ToolSettings.Mode)
			&& ToolSettings.BillboardMode == EFoliageBakerBillboardMode::DoublePlanes)
		{
			const UFoliageBakerSingleBillboardSettings* BillboardPreferences =
				Cast<UFoliageBakerSingleBillboardSettings>(EditorPreferences);
			return BillboardPreferences
				? BillboardPreferences->DoublePlanesMaterialInstanceTemplate
				: TSoftObjectPtr<UMaterialInstanceConstant>();
		}
		return EditorPreferences->MaterialInstanceTemplate;
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
		Request.Mode = ToCoreMode(Settings.Mode);
		Request.BillboardPlaneMode = ToCoreBillboardPlaneMode(Settings.BillboardMode);
		Request.SingleCaptureAxis = ToCoreAxis(Settings.SingleCaptureAxis);
		Request.CrossCardPlaneCount = FMath::Clamp(
			Settings.CrossCardPlaneCount,
			MinCardPlaneCount,
			MaxCardPlaneCount);
		Request.CrossCardGeometryMode = ToCoreGeometryMode(Settings.CrossCardFaceMode);
		Request.TrunkMaterialKeywords = Settings.TrunkMaterialKeywords;
		Request.TextureResolution = FMath::Clamp(
			IsSingleBillboardMode(Settings.Mode)
				? Settings.SingleTextureResolution
				: Settings.CrossTextureResolution,
			MinTextureResolution,
			MaxTextureResolution);
		Request.AlphaCropGuardPixels = FMath::Clamp(
			Settings.AlphaCropGuardPixels,
			MinAlphaCropGuardPixels,
			MaxAlphaCropGuardPixels);
		Request.bTrimUnusedAtlasSpace = Settings.bTrimUnusedAtlasSpace;
		Request.bBakeBaseColorOpacity = Settings.bBakeBaseColorOpacity;
		Request.bBakeNormalDepth = Settings.bBakeNormalDepth;
		Request.bBakeMix = Settings.bBakeMix;
		Request.BaseColorOpacityTextureParameterName = Settings.BaseColorOpacityTextureParameterName;
		Request.NormalDepthTextureParameterName = Settings.NormalDepthTextureParameterName;
		Request.MixTextureParameterName = Settings.MixTextureParameterName;
		Request.LeafRoughnessParameterName = Settings.LeafRoughnessParameterName;
		Request.LeafSpecularParameterName = Settings.LeafSpecularParameterName;
		Request.TrunkRoughnessParameterName = Settings.TrunkRoughnessParameterName;
		Request.TrunkSpecularParameterName = Settings.TrunkSpecularParameterName;
		Request.TextureOutputFolderName = Settings.TextureOutputFolderName;
		Request.MaterialOutputFolderName = Settings.MaterialOutputFolderName;
		Request.TextureNamePrefix = Settings.TextureNamePrefix;
		Request.BaseColorOpacityTextureSuffix = Settings.BaseColorOpacityTextureSuffix;
		Request.NormalDepthTextureSuffix = Settings.NormalDepthTextureSuffix;
		Request.MixTextureSuffix = Settings.MixTextureSuffix;
		Request.MaterialInstanceNamePrefix = Settings.MaterialInstanceNamePrefix;
		Request.MaterialInstanceNameSuffix = Settings.MaterialInstanceNameSuffix;
		return Request;
	}
}

void FFoliageBakerCardsModule::ShutdownModule()
{
	SingleBillboardController.Reset();
	CrossCardsController.Reset();
	SingleBillboardSettings.Reset();
	CrossCardsSettings.Reset();
}

void FFoliageBakerCardsModule::EnsureToolSettings(const EFoliageBakerCardMode Mode)
{
	TStrongObjectPtr<UFoliageBakerCardsSettings>& Settings = IsSingleBillboardMode(Mode)
		? SingleBillboardSettings
		: CrossCardsSettings;
	if (!Settings.IsValid())
	{
		if (IsSingleBillboardMode(Mode))
		{
			FFoliageBakerFeatureTool::EnsureTransientSettings<
				UFoliageBakerCardsSettings,
				UFoliageBakerSingleBillboardSettings>(
					Settings,
					FName(TEXT("FoliageBakerSingleBillboardSettings")));
		}
		else
		{
			FFoliageBakerFeatureTool::EnsureTransientSettings<
				UFoliageBakerCardsSettings,
				UFoliageBakerCrossCardsSettings>(
					Settings,
					FName(TEXT("FoliageBakerCrossCardsSettings")));
		}
	}
	Settings->Mode = Mode;
}

UFoliageBakerCardsSettings* FFoliageBakerCardsModule::GetToolSettings(const EFoliageBakerCardMode Mode) const
{
	return IsSingleBillboardMode(Mode)
		? SingleBillboardSettings.Get()
		: CrossCardsSettings.Get();
}

TSharedPtr<FFoliageBakerFeatureController>& FFoliageBakerCardsModule::GetFeatureController(
	const EFoliageBakerCardMode Mode)
{
	return IsSingleBillboardMode(Mode)
		? SingleBillboardController
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
		: LOCTEXT("BakeCrossCardsButton", "Bake Cross Cards");
	ControllerArgs.BakeButtonTooltip = IsSingleBillboardMode(Mode)
		? LOCTEXT("BakeBillboardTooltip", "Bake the selected Single Plane or Double Planes Billboard mode for every queued Static Mesh.")
		: LOCTEXT("BakeCrossCardsTooltip", "Bake one Cross Cards asset for every queued Static Mesh.");
	ControllerArgs.RequirementsHint = LOCTEXT(
		"BakeRequirementsHint",
		"Configure the Parent Material Instance required by the selected mode in Editor Preferences and queue at least one Static Mesh.");
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
	const UFoliageBakerCardsSettings* EditorPreferences = GetEditorPreferences(Mode);
	if (!Settings)
	{
		return false;
	}
	const TSoftObjectPtr<UMaterialInstanceConstant> MaterialTemplate =
		GetConfiguredMaterialTemplate(*Settings, EditorPreferences);
	return FFoliageBakerFeatureTool::CanBakeFeature(
		!MaterialTemplate.IsNull(),
		Settings->bBakeBaseColorOpacity
			|| Settings->bBakeNormalDepth
			|| Settings->bBakeMix,
		Settings->SourceStaticMeshes);
}

void FFoliageBakerCardsModule::Bake(const EFoliageBakerCardMode Mode)
{
	EnsureToolSettings(Mode);
	UFoliageBakerCardsSettings* Settings = GetToolSettings(Mode);
	const UFoliageBakerCardsSettings* EditorPreferences = GetEditorPreferences(Mode);
	TSoftObjectPtr<UMaterialInstanceConstant> ConfiguredMaterialTemplate =
		GetConfiguredMaterialTemplate(*Settings, EditorPreferences);
	UMaterialInstanceConstant* MaterialTemplate = ConfiguredMaterialTemplate.LoadSynchronous();
	if (!MaterialTemplate)
	{
		FFoliageBakerFeatureTool::ShowMessage(LOCTEXT(
			"MissingTemplate",
			"Configure the Parent Material Instance required by the selected Billboard or Cross Cards mode in Editor Preferences before baking."));
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
							BuildRequest(StaticMesh, *MaterialTemplate, *Settings));
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
