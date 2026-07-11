#include "FoliageBakerCardsModule.h"

#include "AssetRegistry/AssetData.h"
#include "AssetSelection.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailsViewArgs.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "FoliageBakerCardBaker.h"
#include "FoliageBakerCardsSettings.h"
#include "IDetailCustomization.h"
#include "IDetailsView.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/MessageDialog.h"
#include "Misc/ScopedSlowTask.h"
#include "PropertyEditorModule.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FFoliageBakerCardsModule"

namespace
{
	class FFoliageBakerCategoryOrderCustomization final : public IDetailCustomization
	{
	public:
		static TSharedRef<IDetailCustomization> MakeInstance()
		{
			return MakeShared<FFoliageBakerCategoryOrderCustomization>();
		}

		virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override
		{
			DetailBuilder.EditCategory(TEXT("Mesh")).SetSortOrder(0);
			DetailBuilder.EditCategory(TEXT("Feature")).SetSortOrder(1);
			DetailBuilder.EditCategory(TEXT("Asset")).SetSortOrder(2);
			DetailBuilder.EditCategory(TEXT("Material")).SetSortOrder(3);
		}
	};

	constexpr int32 MinCardPlaneCount = 2;
	constexpr int32 MaxCardPlaneCount = 5;
	constexpr int32 MinTextureResolution = 256;
	constexpr int32 MaxTextureResolution = 4096;
	constexpr int32 MinAlphaCropGuardPixels = 0;
	constexpr int32 MaxAlphaCropGuardPixels = 16;

	bool IsSingleBillboardMode(const EFoliageBakerCardMode Mode)
	{
		return Mode == EFoliageBakerCardMode::SingleBillboard;
	}

	TArray<UStaticMesh*> GetSelectedStaticMeshes()
	{
		TArray<FAssetData> SelectedAssets;
		AssetSelectionUtils::GetSelectedAssets(SelectedAssets);

		TArray<UStaticMesh*> StaticMeshes;
		for (const FAssetData& AssetData : SelectedAssets)
		{
			if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(AssetData.GetAsset()))
			{
				StaticMeshes.AddUnique(StaticMesh);
			}
		}
		return StaticMeshes;
	}

	TArray<UStaticMesh*> GetUniqueValidStaticMeshes(const TArray<TObjectPtr<UStaticMesh>>& SourceStaticMeshes)
	{
		TArray<UStaticMesh*> StaticMeshes;
		StaticMeshes.Reserve(SourceStaticMeshes.Num());
		for (const TObjectPtr<UStaticMesh>& StaticMesh : SourceStaticMeshes)
		{
			if (StaticMesh)
			{
				StaticMeshes.AddUnique(StaticMesh.Get());
			}
		}
		return StaticMeshes;
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

	EFoliageBakerMeshOutputMode ToCoreOutputMode(const EFoliageBakerCardsMeshOutputMode Mode)
	{
		switch (Mode)
		{
		case EFoliageBakerCardsMeshOutputMode::AddToSourceMeshLOD:
			return EFoliageBakerMeshOutputMode::AddToSourceMeshLOD;
		case EFoliageBakerCardsMeshOutputMode::ReplaceSourceMeshLOD:
			return EFoliageBakerMeshOutputMode::ReplaceSourceMeshLOD;
		case EFoliageBakerCardsMeshOutputMode::SeparateMeshAsset:
		default:
			return EFoliageBakerMeshOutputMode::SeparateMeshAsset;
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
		Request.MeshOutputMode = ToCoreOutputMode(Settings.MeshOutputMode);
		Request.ReplaceSourceLODIndex = Settings.ReplaceSourceLODIndex;
		Request.TextureResolution = FMath::Clamp(
			IsSingleBillboardMode(Settings.Mode)
				? Settings.SingleTextureResolution
				: Settings.CrossTextureResolution,
			MinTextureResolution,
			MaxTextureResolution);
		Request.SourceMaterialBakeResolution = FMath::Clamp(
			Settings.SourceMaterialBakeResolution,
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

void FFoliageBakerCardsModule::StartupModule()
{
}

void FFoliageBakerCardsModule::ShutdownModule()
{
	SingleBillboardDetailsView.Reset();
	CrossCardsDetailsView.Reset();
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
			Settings.Reset(NewObject<UFoliageBakerSingleBillboardSettings>(
				GetTransientPackage(),
				FName(TEXT("FoliageBakerSingleBillboardSettings")),
				RF_Transactional));
		}
		else
		{
			Settings.Reset(NewObject<UFoliageBakerCrossCardsSettings>(
				GetTransientPackage(),
				FName(TEXT("FoliageBakerCrossCardsSettings")),
				RF_Transactional));
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

TSharedPtr<IDetailsView>& FFoliageBakerCardsModule::GetDetailsView(const EFoliageBakerCardMode Mode)
{
	return IsSingleBillboardMode(Mode)
		? SingleBillboardDetailsView
		: CrossCardsDetailsView;
}

TSharedRef<SWidget> FFoliageBakerCardsModule::CreateFeaturePanel(const EFoliageBakerCardMode Mode)
{
	EnsureToolSettings(Mode);
	UFoliageBakerCardsSettings* Settings = GetToolSettings(Mode);

	FPropertyEditorModule& PropertyEditorModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.bAllowSearch = true;
	DetailsViewArgs.bHideSelectionTip = true;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	TSharedPtr<IDetailsView>& DetailsView = GetDetailsView(Mode);
	DetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);
	DetailsView->RegisterInstancedCustomPropertyLayout(
		Settings->GetClass(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FFoliageBakerCategoryOrderCustomization::MakeInstance));
	DetailsView->SetObject(Settings);

	const FText BakeButtonText = IsSingleBillboardMode(Mode)
		? LOCTEXT("BakeSingleBillboardButton", "Bake Single Billboard")
		: LOCTEXT("BakeCrossCardsButton", "Bake Cross Cards");
	const FText BakeButtonTooltip = IsSingleBillboardMode(Mode)
		? LOCTEXT("BakeSingleBillboardTooltip", "Bake one Single Billboard asset for every queued Static Mesh.")
		: LOCTEXT("BakeCrossCardsTooltip", "Bake one Cross Cards asset for every queued Static Mesh.");

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.0f, 8.0f, 8.0f, 4.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(10.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SourceMeshesSectionTitle", "Source Static Meshes"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([this, Mode]() { return GetSourceMeshCountText(Mode); })
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(8.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("AddSelectedMeshes", "Add Content Browser Selection"))
						.ToolTipText(LOCTEXT("AddSelectedMeshesTooltip", "Add selected Static Mesh assets without removing meshes already queued."))
						.OnClicked_Lambda([this, Mode]() { return HandleAddSelectedMeshes(Mode); })
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("ClearMeshes", "Clear"))
						.ToolTipText(LOCTEXT("ClearMeshesTooltip", "Remove all queued Static Mesh assets from this feature."))
						.OnClicked_Lambda([this, Mode]() { return HandleClearMeshes(Mode); })
					]
				]
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(8.0f, 4.0f)
		[
			DetailsView.ToSharedRef()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SSeparator)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 12.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("BakeRequirementsHint", "Select a Material Instance Constant template and queue at least one Static Mesh."))
				.AutoWrapText(true)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SBox)
				.MinDesiredWidth(200.0f)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.HAlign(HAlign_Center)
					.Text(BakeButtonText)
					.ToolTipText(BakeButtonTooltip)
					.IsEnabled_Lambda([this, Mode]() { return CanBake(Mode); })
					.OnClicked_Lambda([this, Mode]() { return HandleBake(Mode); })
				]
			]
		];
}

void FFoliageBakerCardsModule::AddContentBrowserSelectionToTool(const EFoliageBakerCardMode Mode)
{
	EnsureToolSettings(Mode);
	UFoliageBakerCardsSettings* Settings = GetToolSettings(Mode);
	for (UStaticMesh* StaticMesh : GetSelectedStaticMeshes())
	{
		Settings->SourceStaticMeshes.AddUnique(StaticMesh);
	}
	if (TSharedPtr<IDetailsView>& DetailsView = GetDetailsView(Mode); DetailsView.IsValid())
	{
		DetailsView->ForceRefresh();
	}
}

FReply FFoliageBakerCardsModule::HandleAddSelectedMeshes(const EFoliageBakerCardMode Mode)
{
	AddContentBrowserSelectionToTool(Mode);
	return FReply::Handled();
}

FReply FFoliageBakerCardsModule::HandleClearMeshes(const EFoliageBakerCardMode Mode)
{
	EnsureToolSettings(Mode);
	GetToolSettings(Mode)->SourceStaticMeshes.Reset();
	if (TSharedPtr<IDetailsView>& DetailsView = GetDetailsView(Mode); DetailsView.IsValid())
	{
		DetailsView->ForceRefresh();
	}
	return FReply::Handled();
}

bool FFoliageBakerCardsModule::CanBake(const EFoliageBakerCardMode Mode) const
{
	const UFoliageBakerCardsSettings* Settings = GetToolSettings(Mode);
	if (!Settings || Settings->MaterialInstanceTemplate.IsNull())
	{
		return false;
	}
	return Settings->SourceStaticMeshes.ContainsByPredicate([](const TObjectPtr<UStaticMesh>& StaticMesh)
	{
		return StaticMesh != nullptr;
	});
}

FText FFoliageBakerCardsModule::GetSourceMeshCountText(const EFoliageBakerCardMode Mode) const
{
	int32 Count = 0;
	if (const UFoliageBakerCardsSettings* Settings = GetToolSettings(Mode))
	{
		for (const TObjectPtr<UStaticMesh>& StaticMesh : Settings->SourceStaticMeshes)
		{
			Count += StaticMesh ? 1 : 0;
		}
	}
	return FText::Format(LOCTEXT("QueuedMeshCount", "{0} Static Mesh asset(s) queued"), FText::AsNumber(Count));
}

FReply FFoliageBakerCardsModule::HandleBake(const EFoliageBakerCardMode Mode)
{
	EnsureToolSettings(Mode);
	UFoliageBakerCardsSettings* Settings = GetToolSettings(Mode);
	UMaterialInstanceConstant* MaterialTemplate = Settings->MaterialInstanceTemplate.LoadSynchronous();
	if (!MaterialTemplate)
	{
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("MissingTemplate", "Select a Material Instance Constant template before baking."));
		return FReply::Handled();
	}

	const TArray<UStaticMesh*> StaticMeshes = GetUniqueValidStaticMeshes(Settings->SourceStaticMeshes);

	FScopedSlowTask SlowTask(StaticMeshes.Num(), LOCTEXT("BakeCardsSlowTask", "Baking foliage cards..."));
	SlowTask.MakeDialog(true);
	TArray<UObject*> CreatedAssets;
	FString Report;
	int32 SuccessCount = 0;
	for (UStaticMesh* StaticMesh : StaticMeshes)
	{
		if (SlowTask.ShouldCancel())
		{
			break;
		}
		SlowTask.EnterProgressFrame(1.0f, FText::FromString(StaticMesh->GetName()));
		FFoliageBakerCardBakeResult Result = FFoliageBakerCardBaker::Bake(BuildRequest(*StaticMesh, *MaterialTemplate, *Settings));
		Report += Result.Report + TEXT("\n");
		if (Result.bSucceeded)
		{
			++SuccessCount;
			CreatedAssets.Append(Result.CreatedAssets);
		}
	}

	if (!CreatedAssets.IsEmpty() && GEditor)
	{
		GEditor->SyncBrowserToObjects(CreatedAssets);
	}
	FMessageDialog::Open(
		EAppMsgType::Ok,
		FText::Format(
			LOCTEXT("BakeCardsSummary", "Foliage Baker completed {0} of {1} asset(s).\n\n{2}"),
			FText::AsNumber(SuccessCount),
			FText::AsNumber(StaticMeshes.Num()),
			FText::FromString(Report)));
	return FReply::Handled();
}

IMPLEMENT_MODULE(FFoliageBakerCardsModule, FoliageBakerCards)

#undef LOCTEXT_NAMESPACE
