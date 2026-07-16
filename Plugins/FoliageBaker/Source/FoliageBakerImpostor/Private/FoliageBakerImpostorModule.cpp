#include "FoliageBakerImpostorModule.h"

#include "AssetRegistry/AssetData.h"
#include "AssetSelection.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailsViewArgs.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "FoliageBakerImpostorBaker.h"
#include "FoliageBakerImpostorSettings.h"
#include "IDetailCustomization.h"
#include "IDetailsView.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/MessageDialog.h"
#include "Misc/ScopedSlowTask.h"
#include "PropertyEditorModule.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FFoliageBakerImpostorModule"

namespace
{
	class FFoliageBakerImpostorCategoryOrderCustomization final : public IDetailCustomization
	{
	public:
		static TSharedRef<IDetailCustomization> MakeInstance()
		{
			return MakeShared<FFoliageBakerImpostorCategoryOrderCustomization>();
		}

		virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override
		{
			DetailBuilder.EditCategory(TEXT("Mesh")).SetSortOrder(0);
			DetailBuilder.EditCategory(TEXT("Feature")).SetSortOrder(1);
			DetailBuilder.EditCategory(TEXT("Asset")).SetSortOrder(2);
			DetailBuilder.EditCategory(TEXT("Material")).SetSortOrder(3);
		}
	};

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
		for (const TObjectPtr<UStaticMesh>& StaticMesh : SourceStaticMeshes)
		{
			if (StaticMesh)
			{
				StaticMeshes.AddUnique(StaticMesh.Get());
			}
		}
		return StaticMeshes;
	}
}

void FFoliageBakerImpostorModule::ShutdownModule()
{
	DetailsView.Reset();
	ToolSettings.Reset();
}

void FFoliageBakerImpostorModule::EnsureToolSettings()
{
	if (!ToolSettings.IsValid())
	{
		ToolSettings.Reset(NewObject<UFoliageBakerImpostorSettings>(
			GetTransientPackage(),
			FName(TEXT("FoliageBakerImpostorSettings")),
			RF_Transactional));
	}
}

TSharedRef<SWidget> FFoliageBakerImpostorModule::CreateFeaturePanel()
{
	EnsureToolSettings();
	FPropertyEditorModule& PropertyEditorModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.bAllowSearch = true;
	DetailsViewArgs.bHideSelectionTip = true;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	DetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);
	DetailsView->RegisterInstancedCustomPropertyLayout(
		ToolSettings->GetClass(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FFoliageBakerImpostorCategoryOrderCustomization::MakeInstance));
	DetailsView->SetObject(ToolSettings.Get());

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
						.Text_Raw(this, &FFoliageBakerImpostorModule::GetSourceMeshCountText)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(8.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("AddSelectedMeshes", "Add Content Browser Selection"))
						.OnClicked_Raw(this, &FFoliageBakerImpostorModule::HandleAddSelectedMeshes)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("ClearMeshes", "Clear"))
						.OnClicked_Raw(this, &FFoliageBakerImpostorModule::HandleClearMeshes)
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
				.Text(LOCTEXT("BakeRequirementsHint", "Configure the Parent Material Instance in Editor Preferences and queue at least one Static Mesh."))
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
					.Text(LOCTEXT("BakeImpostorButton", "Bake Impostor"))
					.IsEnabled_Raw(this, &FFoliageBakerImpostorModule::CanBake)
					.OnClicked_Raw(this, &FFoliageBakerImpostorModule::HandleBake)
				]
			]
		];
}

void FFoliageBakerImpostorModule::AddContentBrowserSelectionToTool()
{
	EnsureToolSettings();
	const TArray<UStaticMesh*> SelectedStaticMeshes = GetSelectedStaticMeshes();
	const bool bHasNewStaticMesh = SelectedStaticMeshes.ContainsByPredicate([this](const UStaticMesh* StaticMesh)
	{
		return !ToolSettings->SourceStaticMeshes.Contains(StaticMesh);
	});
	if (!bHasNewStaticMesh)
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("AddImpostorSourceMeshesTransaction", "Add Foliage Baker Impostor Source Meshes"));
	ToolSettings->Modify();
	for (UStaticMesh* StaticMesh : SelectedStaticMeshes)
	{
		ToolSettings->SourceStaticMeshes.AddUnique(StaticMesh);
	}
	ToolSettings->PostEditChange();
	if (DetailsView.IsValid())
	{
		DetailsView->ForceRefresh();
	}
}

FReply FFoliageBakerImpostorModule::HandleAddSelectedMeshes()
{
	AddContentBrowserSelectionToTool();
	return FReply::Handled();
}

FReply FFoliageBakerImpostorModule::HandleClearMeshes()
{
	EnsureToolSettings();
	if (ToolSettings->SourceStaticMeshes.IsEmpty())
	{
		return FReply::Handled();
	}

	const FScopedTransaction Transaction(LOCTEXT("ClearImpostorSourceMeshesTransaction", "Clear Foliage Baker Impostor Source Meshes"));
	ToolSettings->Modify();
	ToolSettings->SourceStaticMeshes.Reset();
	ToolSettings->PostEditChange();
	if (DetailsView.IsValid())
	{
		DetailsView->ForceRefresh();
	}
	return FReply::Handled();
}

bool FFoliageBakerImpostorModule::CanBake() const
{
	const UFoliageBakerImpostorSettings* EditorPreferences = GetDefault<UFoliageBakerImpostorSettings>();
	return ToolSettings.IsValid()
		&& EditorPreferences
		&& !EditorPreferences->MaterialInstanceTemplate.IsNull()
		&& (ToolSettings->bBakeBaseColorSdf || ToolSettings->bBakeNormalDepth || ToolSettings->bBakeMix)
		&& ToolSettings->SourceStaticMeshes.ContainsByPredicate([](const TObjectPtr<UStaticMesh>& StaticMesh)
		{
			return StaticMesh != nullptr;
		});
}

FText FFoliageBakerImpostorModule::GetSourceMeshCountText() const
{
	int32 Count = 0;
	if (ToolSettings.IsValid())
	{
		for (const TObjectPtr<UStaticMesh>& StaticMesh : ToolSettings->SourceStaticMeshes)
		{
			Count += StaticMesh ? 1 : 0;
		}
	}
	return FText::Format(LOCTEXT("QueuedMeshCount", "{0} Static Mesh asset(s) queued"), FText::AsNumber(Count));
}

FReply FFoliageBakerImpostorModule::HandleBake()
{
	EnsureToolSettings();
	const UFoliageBakerImpostorSettings* EditorPreferences = GetDefault<UFoliageBakerImpostorSettings>();
	UMaterialInstanceConstant* MaterialTemplate = EditorPreferences
		? EditorPreferences->MaterialInstanceTemplate.LoadSynchronous()
		: nullptr;
	if (!MaterialTemplate)
	{
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("MissingTemplate", "Configure the Parent Material Instance in Editor Preferences before baking."));
		return FReply::Handled();
	}

	const TArray<UStaticMesh*> StaticMeshes = GetUniqueValidStaticMeshes(ToolSettings->SourceStaticMeshes);
	FScopedSlowTask SlowTask(StaticMeshes.Num(), LOCTEXT("BakeImpostorSlowTask", "Baking foliage Impostors..."));
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
		const FFoliageBakerImpostorBakeResult Result = FFoliageBakerImpostorBaker::Bake(*StaticMesh, *MaterialTemplate, *ToolSettings);
		Report += Result.Report + TEXT("\n");
		if (Result.bSucceeded)
		{
			++SuccessCount;
			CreatedAssets.Append(Result.CreatedAssets);
		}
		if (Result.bCancelled)
		{
			break;
		}
	}

	if (!CreatedAssets.IsEmpty() && GEditor)
	{
		GEditor->SyncBrowserToObjects(CreatedAssets);
	}
	FMessageDialog::Open(
		EAppMsgType::Ok,
		FText::Format(
			LOCTEXT("BakeImpostorSummary", "Foliage Baker completed {0} of {1} Impostor asset(s).\n\n{2}"),
			FText::AsNumber(SuccessCount),
			FText::AsNumber(StaticMeshes.Num()),
			FText::FromString(Report)));
	return FReply::Handled();
}

IMPLEMENT_MODULE(FFoliageBakerImpostorModule, FoliageBakerImpostor)

#undef LOCTEXT_NAMESPACE
