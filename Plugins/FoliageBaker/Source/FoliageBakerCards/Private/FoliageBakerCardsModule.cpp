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
	constexpr int32 MinOpacitySdfRangePixels = 1;
	constexpr int32 MaxOpacitySdfRangePixels = 64;

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
		Request.SingleCaptureAxis = ToCoreAxis(Settings.SingleCaptureAxis);
		Request.CrossCardPlaneCount = FMath::Clamp(
			Settings.CrossCardPlaneCount,
			MinCardPlaneCount,
			MaxCardPlaneCount);
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
		Request.OpacitySdfRangePixels = FMath::Clamp(
			Settings.OpacitySdfRangePixels,
			MinOpacitySdfRangePixels,
			MaxOpacitySdfRangePixels);
		Request.bTrimUnusedAtlasSpace = Settings.bTrimUnusedAtlasSpace;
		Request.bBakeBaseColorOpacity = Settings.bBakeBaseColorOpacity;
		Request.bBakeNormalDepth = Settings.bBakeNormalDepth;
		Request.bBakeMix = Settings.bBakeMix;
		Request.BaseColorOpacityTextureParameterName = Settings.BaseColorOpacityTextureParameterName;
		Request.NormalDepthTextureParameterName = Settings.NormalDepthTextureParameterName;
		Request.MixTextureParameterName = Settings.MixTextureParameterName;
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
		? LOCTEXT("BakeSingleBillboardButton", "Bake Single Billboard")
		: LOCTEXT("BakeCrossCardsButton", "Bake Cross Cards");
	ControllerArgs.BakeButtonTooltip = IsSingleBillboardMode(Mode)
		? LOCTEXT("BakeSingleBillboardTooltip", "Bake one Single Billboard asset for every queued Static Mesh.")
		: LOCTEXT("BakeCrossCardsTooltip", "Bake one Cross Cards asset for every queued Static Mesh.");
	ControllerArgs.RequirementsHint = LOCTEXT(
		"BakeRequirementsHint",
		"Configure the Parent Material Instance in Editor Preferences and queue at least one Static Mesh.");
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
	return FFoliageBakerFeatureTool::CanBakeFeature(
		EditorPreferences && !EditorPreferences->MaterialInstanceTemplate.IsNull(),
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
