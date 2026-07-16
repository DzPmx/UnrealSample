#include "FoliageBakerFeatureTool.h"

#include "AssetRegistry/AssetData.h"
#include "AssetSelection.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailsViewArgs.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "IDetailCustomization.h"
#include "IDetailsView.h"
#include "Misc/MessageDialog.h"
#include "Misc/ScopedSlowTask.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FFoliageBakerFeatureTool"

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

	TArray<UStaticMesh*> GetSelectedStaticMeshes()
	{
		TArray<FAssetData> SelectedAssets;
		AssetSelectionUtils::GetSelectedAssets(SelectedAssets);

		TArray<UStaticMesh*> StaticMeshes;
		StaticMeshes.Reserve(SelectedAssets.Num());
		for (const FAssetData& AssetData : SelectedAssets)
		{
			if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(AssetData.GetAsset()))
			{
				StaticMeshes.AddUnique(StaticMesh);
			}
		}
		return StaticMeshes;
	}

	bool AddContentBrowserSelection(
		UObject& Settings,
		TArray<TObjectPtr<UStaticMesh>>& SourceStaticMeshes,
		const FText& TransactionText)
	{
		const TArray<UStaticMesh*> SelectedStaticMeshes = GetSelectedStaticMeshes();
		const bool bHasNewStaticMesh = SelectedStaticMeshes.ContainsByPredicate(
			[&SourceStaticMeshes](const UStaticMesh* StaticMesh)
			{
				return !SourceStaticMeshes.Contains(StaticMesh);
			});
		if (!bHasNewStaticMesh)
		{
			return false;
		}

		const FScopedTransaction Transaction(TransactionText);
		Settings.Modify();
		for (UStaticMesh* StaticMesh : SelectedStaticMeshes)
		{
			SourceStaticMeshes.AddUnique(StaticMesh);
		}
		Settings.PostEditChange();
		return true;
	}

	bool ClearSourceStaticMeshes(
		UObject& Settings,
		TArray<TObjectPtr<UStaticMesh>>& SourceStaticMeshes,
		const FText& TransactionText)
	{
		if (SourceStaticMeshes.IsEmpty())
		{
			return false;
		}

		const FScopedTransaction Transaction(TransactionText);
		Settings.Modify();
		SourceStaticMeshes.Reset();
		Settings.PostEditChange();
		return true;
	}

	TArray<UStaticMesh*> GetUniqueValidStaticMeshes(
		const TArray<TObjectPtr<UStaticMesh>>& SourceStaticMeshes)
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

	FText FormatQueuedStaticMeshCount(
		const TArray<TObjectPtr<UStaticMesh>>& SourceStaticMeshes)
	{
		return FText::Format(
			LOCTEXT("QueuedMeshCount", "{0} Static Mesh asset(s) queued"),
			FText::AsNumber(GetUniqueValidStaticMeshes(SourceStaticMeshes).Num()));
	}

	struct FFoliageBakerFeatureControllerState
	{
		TStrongObjectPtr<UObject> SettingsObject;
		TArray<TObjectPtr<UStaticMesh>>* SourceStaticMeshes = nullptr;
		TWeakPtr<IDetailsView> DetailsView;
		FText AddMeshesTransactionText;
		FText ClearMeshesTransactionText;
		FFoliageBakerFeaturePredicateDelegate CanBake;
		FFoliageBakerFeatureActionDelegate Bake;
	};
}

struct FFoliageBakerFeatureController::FImpl
{
	TSharedPtr<IDetailsView> DetailsView;
	TSharedPtr<SWidget> Widget;
	TSharedPtr<FFoliageBakerFeatureControllerState> State;
};

TSharedRef<FFoliageBakerFeatureController> FFoliageBakerFeatureController::Create(
	const FFoliageBakerFeatureControllerArgs& Args)
{
	check(Args.SettingsObject);
	check(Args.SourceStaticMeshes);

	TUniquePtr<FImpl> Impl = MakeUnique<FImpl>();
	Impl->State = MakeShared<FFoliageBakerFeatureControllerState>();
	Impl->State->SettingsObject.Reset(Args.SettingsObject);
	Impl->State->SourceStaticMeshes = Args.SourceStaticMeshes;
	Impl->State->AddMeshesTransactionText = Args.AddMeshesTransactionText;
	Impl->State->ClearMeshesTransactionText = Args.ClearMeshesTransactionText;
	Impl->State->CanBake = Args.CanBake;
	Impl->State->Bake = Args.Bake;
	FPropertyEditorModule& PropertyEditorModule =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.bAllowSearch = true;
	DetailsViewArgs.bHideSelectionTip = true;
	DetailsViewArgs.bLockable = false;
	DetailsViewArgs.bShowOptions = Args.bShowDetailsOptions;
	DetailsViewArgs.bShowPropertyMatrixButton = Args.bShowPropertyMatrixButton;
	DetailsViewArgs.bUpdatesFromSelection = false;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	Impl->DetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);
	Impl->DetailsView->RegisterInstancedCustomPropertyLayout(
		Args.SettingsObject->GetClass(),
		FOnGetDetailCustomizationInstance::CreateStatic(
			&FFoliageBakerCategoryOrderCustomization::MakeInstance));
	Impl->DetailsView->SetObject(Args.SettingsObject);
	Impl->State->DetailsView = Impl->DetailsView;

	const TWeakPtr<FFoliageBakerFeatureControllerState> WeakState = Impl->State;

	Impl->Widget = SNew(SVerticalBox)
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
						.Text_Lambda([WeakState]()
						{
							const TSharedPtr<FFoliageBakerFeatureControllerState> State =
								WeakState.Pin();
							if (!State
								|| !State->SettingsObject.IsValid()
								|| !State->SourceStaticMeshes)
							{
								return FText::GetEmpty();
							}
							return FormatQueuedStaticMeshCount(
								*State->SourceStaticMeshes);
						})
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(8.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("AddSelectedMeshes", "Add Content Browser Selection"))
						.ToolTipText(LOCTEXT(
							"AddSelectedMeshesTooltip",
							"Add selected Static Mesh assets without removing meshes already queued."))
						.OnClicked_Lambda([WeakState]()
						{
							const TSharedPtr<FFoliageBakerFeatureControllerState> State =
								WeakState.Pin();
							if (State
								&& State->SettingsObject.IsValid()
								&& State->SourceStaticMeshes
								&& AddContentBrowserSelection(
									*State->SettingsObject.Get(),
									*State->SourceStaticMeshes,
									State->AddMeshesTransactionText))
							{
								if (const TSharedPtr<IDetailsView> DetailsView =
									State->DetailsView.Pin())
								{
									DetailsView->ForceRefresh();
								}
							}
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("ClearMeshes", "Clear"))
						.ToolTipText(LOCTEXT(
							"ClearMeshesTooltip",
							"Remove all queued Static Mesh assets from this feature."))
						.OnClicked_Lambda([WeakState]()
						{
							const TSharedPtr<FFoliageBakerFeatureControllerState> State =
								WeakState.Pin();
							if (State
								&& State->SettingsObject.IsValid()
								&& State->SourceStaticMeshes
								&& ClearSourceStaticMeshes(
									*State->SettingsObject.Get(),
									*State->SourceStaticMeshes,
									State->ClearMeshesTransactionText))
							{
								if (const TSharedPtr<IDetailsView> DetailsView =
									State->DetailsView.Pin())
								{
									DetailsView->ForceRefresh();
								}
							}
							return FReply::Handled();
						})
					]
				]
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(8.0f, 4.0f)
		[
			Impl->DetailsView.ToSharedRef()
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
					.Text(Args.RequirementsHint)
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
						.Text(Args.BakeButtonText)
						.ToolTipText(Args.BakeButtonTooltip)
						.IsEnabled_Lambda([WeakState]()
						{
							const TSharedPtr<FFoliageBakerFeatureControllerState> State =
								WeakState.Pin();
							return State
								&& State->CanBake.IsBound()
								&& State->CanBake.Execute();
						})
						.OnClicked_Lambda([WeakState]()
						{
							if (const TSharedPtr<FFoliageBakerFeatureControllerState> State =
								WeakState.Pin())
							{
								State->Bake.ExecuteIfBound();
							}
							return FReply::Handled();
						})
				]
			]
		];

	TSharedRef<FFoliageBakerFeatureController> Controller =
		MakeShareable(new FFoliageBakerFeatureController(MoveTemp(Impl)));
	return Controller;
}

FFoliageBakerFeatureController::FFoliageBakerFeatureController(TUniquePtr<FImpl>&& InImpl)
	: Impl(MoveTemp(InImpl))
{
}

FFoliageBakerFeatureController::~FFoliageBakerFeatureController() = default;

TSharedRef<SWidget> FFoliageBakerFeatureController::GetWidget() const
{
	check(Impl && Impl->Widget.IsValid());
	return Impl->Widget.ToSharedRef();
}

bool FFoliageBakerFeatureTool::HasAnyValidStaticMesh(
	const TArray<TObjectPtr<UStaticMesh>>& SourceStaticMeshes)
{
	return SourceStaticMeshes.ContainsByPredicate(
		[](const TObjectPtr<UStaticMesh>& StaticMesh)
		{
			return StaticMesh != nullptr;
		});
}

bool FFoliageBakerFeatureTool::CanBakeFeature(
	const bool bHasMaterialTemplate,
	const bool bHasEnabledOutput,
	const TArray<TObjectPtr<UStaticMesh>>& SourceStaticMeshes)
{
	return bHasMaterialTemplate
		&& bHasEnabledOutput
		&& HasAnyValidStaticMesh(SourceStaticMeshes);
}

FFoliageBakerFeatureBatchResult FFoliageBakerFeatureTool::RunBakeBatch(
	const TArray<TObjectPtr<UStaticMesh>>& SourceStaticMeshes,
	const FText& SlowTaskText,
	const bool bAllowCancel,
	const FString& ReportSeparator,
	const FFoliageBakerBakeStaticMeshDelegate& BakeStaticMesh)
{
	FFoliageBakerFeatureBatchResult BatchResult;
	const TArray<UStaticMesh*> StaticMeshes =
		GetUniqueValidStaticMeshes(SourceStaticMeshes);
	BatchResult.TotalCount = StaticMeshes.Num();
	if (!BakeStaticMesh.IsBound())
	{
		return BatchResult;
	}

	FScopedSlowTask SlowTask(StaticMeshes.Num(), SlowTaskText);
	SlowTask.MakeDialog(bAllowCancel);
	for (UStaticMesh* StaticMesh : StaticMeshes)
	{
		if (!StaticMesh)
		{
			continue;
		}
		if (bAllowCancel && SlowTask.ShouldCancel())
		{
			break;
		}

		SlowTask.EnterProgressFrame(1.0f, FText::FromString(StaticMesh->GetName()));
		const FFoliageBakerFeatureBakeItemResult ItemResult = BakeStaticMesh.Execute(*StaticMesh);
		BatchResult.Report += ItemResult.Report + ReportSeparator;
		if (ItemResult.bSucceeded)
		{
			++BatchResult.SuccessCount;
			BatchResult.CreatedAssets.Append(ItemResult.CreatedAssets);
		}
		if (ItemResult.bCancelled)
		{
			break;
		}
	}
	return BatchResult;
}

void FFoliageBakerFeatureTool::SyncCreatedAssetsToContentBrowser(
	const TArray<UObject*>& CreatedAssets)
{
	if (!CreatedAssets.IsEmpty() && GEditor)
	{
		GEditor->SyncBrowserToObjects(CreatedAssets);
	}
}

void FFoliageBakerFeatureTool::ShowMessage(const FText& Message)
{
	FMessageDialog::Open(EAppMsgType::Ok, Message);
}

void FFoliageBakerFeatureTool::ShowBatchSummary(
	const FFoliageBakerFeatureBatchResult& BatchResult,
	const FText& SummaryFormat)
{
	ShowMessage(FText::Format(
		SummaryFormat,
		FText::AsNumber(BatchResult.SuccessCount),
		FText::AsNumber(BatchResult.TotalCount),
		FText::FromString(BatchResult.Report)));
}

#undef LOCTEXT_NAMESPACE
