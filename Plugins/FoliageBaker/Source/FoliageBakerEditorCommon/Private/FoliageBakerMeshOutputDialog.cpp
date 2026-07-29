#include "FoliageBakerMeshOutputDialog.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Styling/CoreStyle.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	bool DoesGeneratedAssetExist(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty())
		{
			return false;
		}

		if (FindObject<UObject>(nullptr, *ObjectPath))
		{
			return true;
		}

		const FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
				TEXT("AssetRegistry"));
		if (AssetRegistryModule.Get().GetAssetByObjectPath(
				FName(*ObjectPath)).IsValid())
		{
			return true;
		}

		const FString PackageName =
			FPackageName::ObjectPathToPackageName(ObjectPath);
		return !PackageName.IsEmpty()
			&& FPackageName::DoesPackageExist(PackageName);
	}

	class SFoliageBakerExistingAssetDialog final : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SFoliageBakerExistingAssetDialog)
		{
		}
			SLATE_ARGUMENT(
				TArray<FFoliageBakerGeneratedAssetPath>,
				ConflictingAssets)
			SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			ConflictingAssets = InArgs._ConflictingAssets;
			ParentWindow = InArgs._ParentWindow;

			TSharedRef<SVerticalBox> AssetList = SNew(SVerticalBox);
			for (const FFoliageBakerGeneratedAssetPath& Asset :
				ConflictingAssets)
			{
				const FString ObjectPath = Asset.BuildObjectPath();
				AssetList->AddSlot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
							.Text(FText::FromString(
								Asset.DisplayName.IsEmpty()
									? ObjectPath
									: Asset.DisplayName))
							.Font(FCoreStyle::GetDefaultFontStyle(
								TEXT("Bold"),
								10))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
							.Text(FText::FromString(ObjectPath))
							.AutoWrapText(true)
					]
				];
			}

			ChildSlot
			[
				SNew(SBorder)
					.Padding(20.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(STextBlock)
								.Text(FText::FromString(
									TEXT("Generated assets already exist")))
								.Font(FCoreStyle::GetDefaultFontStyle(
									TEXT("Bold"),
									14))
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 6.0f, 0.0f, 14.0f)
						[
							SNew(STextBlock)
								.Text(FText::FromString(TEXT(
									"Choose whether to update the existing assets in place or create a new uniquely named set.")))
								.AutoWrapText(true)
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(SBox)
								.MinDesiredWidth(560.0f)
								.MaxDesiredHeight(260.0f)
								[
									SNew(SScrollBox)
									+ SScrollBox::Slot()
									[
										AssetList
									]
								]
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 10.0f)
						[
							SNew(SSeparator)
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.HAlign(HAlign_Right)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(SButton)
									.ContentPadding(FMargin(14.0f, 5.0f))
									.Text(FText::FromString(
										TEXT("Update Existing")))
									.ToolTipText(FText::FromString(TEXT(
										"Update the listed assets in place. Other objects that reference them will see the new content.")))
									.OnClicked(
										this,
										&SFoliageBakerExistingAssetDialog::
											UpdateExisting)
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(8.0f, 0.0f, 0.0f, 0.0f)
							[
								SNew(SButton)
									.ContentPadding(FMargin(14.0f, 5.0f))
									.Text(FText::FromString(TEXT("Create New")))
									.ToolTipText(FText::FromString(TEXT(
										"Keep the existing assets unchanged and create uniquely named assets for this bake.")))
									.OnClicked(
										this,
										&SFoliageBakerExistingAssetDialog::
											CreateNew)
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(8.0f, 0.0f, 0.0f, 0.0f)
							[
								SNew(SButton)
									.ContentPadding(FMargin(14.0f, 5.0f))
									.Text(FText::FromString(TEXT("Cancel")))
									.OnClicked(
										this,
										&SFoliageBakerExistingAssetDialog::
											Cancel)
							]
						]
					]
			];
		}

		TOptional<EFoliageBakerExistingAssetPolicy> GetPolicy() const
		{
			return Policy;
		}

	private:
		FReply UpdateExisting()
		{
			return CloseWithPolicy(
				EFoliageBakerExistingAssetPolicy::ReuseOrCreate);
		}

		FReply CreateNew()
		{
			return CloseWithPolicy(
				EFoliageBakerExistingAssetPolicy::CreateUnique);
		}

		FReply Cancel()
		{
			if (ParentWindow.IsValid())
			{
				ParentWindow.Pin()->RequestDestroyWindow();
			}
			return FReply::Handled();
		}

		FReply CloseWithPolicy(
			const EFoliageBakerExistingAssetPolicy InPolicy)
		{
			Policy = InPolicy;
			return Cancel();
		}

		TArray<FFoliageBakerGeneratedAssetPath> ConflictingAssets;
		TWeakPtr<SWindow> ParentWindow;
		TOptional<EFoliageBakerExistingAssetPolicy> Policy;
	};

	int32 FindAvailableAssetNameVersion(
		const TArray<FFoliageBakerGeneratedAssetPath>& GeneratedAssets)
	{
		for (int32 AssetNameVersion = 1;
			AssetNameVersion < MAX_int32;
			++AssetNameVersion)
		{
			bool bVersionIsAvailable = true;
			for (const FFoliageBakerGeneratedAssetPath& Asset :
				GeneratedAssets)
			{
				if (DoesGeneratedAssetExist(
					Asset.BuildObjectPath(AssetNameVersion)))
				{
					bVersionIsAvailable = false;
					break;
				}
			}
			if (bVersionIsAvailable)
			{
				return AssetNameVersion;
			}
		}
		return INDEX_NONE;
	}

	class SFoliageBakerMeshOutputDialog final : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SFoliageBakerMeshOutputDialog)
		{
		}
			SLATE_ARGUMENT(const UStaticMesh*, SourceStaticMesh)
			SLATE_ARGUMENT(int32, SourceLODIndex)
			SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			SourceStaticMesh = InArgs._SourceStaticMesh;
			SourceLODIndex = InArgs._SourceLODIndex;
			ParentWindow = InArgs._ParentWindow;
			const int32 LastLODIndex = GetLastLODIndex();
			ReplaceLODIndex = LastLODIndex > 0 ? LastLODIndex : 0;
			InsertAfterLODIndex = FMath::Clamp(SourceLODIndex, 0, FMath::Max(LastLODIndex, 0));

			ChildSlot
			[
				SNew(SBorder)
				.Padding(20.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(FText::FromString(SourceStaticMesh
							? FString::Printf(TEXT("Bake completed for %s"), *SourceStaticMesh->GetName())
							: FString(TEXT("Bake completed"))))
						.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 14))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 14.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(FString::Printf(
							TEXT("Source LOD: %d    Existing LODs: %d\nChoose where the generated proxy mesh should be written."),
							SourceLODIndex,
							GetLODCount())))
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SButton)
						.ContentPadding(FMargin(12.0f, 8.0f))
						.OnClicked(this, &SFoliageBakerMeshOutputDialog::ChooseSeparateMesh)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot()
							.AutoHeight()
							[
								SNew(STextBlock).Text(FText::FromString(TEXT("Create Separate Mesh Asset")))
							]
							+ SVerticalBox::Slot()
							.AutoHeight()
							[
								SNew(STextBlock).Text(FText::FromString(TEXT("Reuse or create the feature-specific proxy mesh asset.")))
							]
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.ContentPadding(FMargin(12.0f, 8.0f))
						.OnClicked(this, &SFoliageBakerMeshOutputDialog::ChooseAddLOD)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot()
							.AutoHeight()
							[
								SNew(STextBlock).Text(FText::FromString(TEXT("Add To Source Mesh LODs")))
							]
							+ SVerticalBox::Slot()
							.AutoHeight()
							[
								SNew(STextBlock).Text(FText::FromString(TEXT("Reuse the previously generated feature LOD or append a new LOD.")))
							]
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						[
							SNew(SButton)
							.ContentPadding(FMargin(12.0f, 8.0f))
							.IsEnabled(this, &SFoliageBakerMeshOutputDialog::CanInsertAfterSelectedLOD)
							.ToolTipText(this, &SFoliageBakerMeshOutputDialog::GetInsertLODToolTip)
							.OnClicked(this, &SFoliageBakerMeshOutputDialog::ChooseInsertLOD)
							[
								SNew(STextBlock).Text(FText::FromString(TEXT("Insert After LOD")))
							]
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(8.0f, 0.0f, 0.0f, 0.0f)
						.VAlign(VAlign_Center)
						[
							SNew(SBox)
							.WidthOverride(90.0f)
							[
								SNew(SNumericEntryBox<int32>)
								.MinValue(FMath::Clamp(SourceLODIndex, 0, FMath::Max(GetLastLODIndex(), 0)))
								.MaxValue(FMath::Max(GetLastLODIndex(), 0))
								.MinSliderValue(FMath::Clamp(SourceLODIndex, 0, FMath::Max(GetLastLODIndex(), 0)))
								.MaxSliderValue(FMath::Max(GetLastLODIndex(), 0))
								.Value(this, &SFoliageBakerMeshOutputDialog::GetInsertAfterLODIndex)
								.OnValueChanged(this, &SFoliageBakerMeshOutputDialog::SetInsertAfterLODIndex)
							]
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						[
							SNew(SButton)
							.ContentPadding(FMargin(12.0f, 8.0f))
							.IsEnabled(this, &SFoliageBakerMeshOutputDialog::CanReplaceSelectedLOD)
							.OnClicked(this, &SFoliageBakerMeshOutputDialog::ChooseReplaceLOD)
							[
								SNew(STextBlock).Text(FText::FromString(TEXT("Replace LOD")))
							]
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(8.0f, 0.0f, 0.0f, 0.0f)
						.VAlign(VAlign_Center)
						[
							SNew(SBox)
							.WidthOverride(90.0f)
							[
								SNew(SNumericEntryBox<int32>)
								.MinValue(FMath::Clamp(SourceLODIndex + 1, 0, FMath::Max(GetLastLODIndex(), 0)))
								.MaxValue(FMath::Max(GetLastLODIndex(), 0))
								.MinSliderValue(FMath::Clamp(SourceLODIndex + 1, 0, FMath::Max(GetLastLODIndex(), 0)))
								.MaxSliderValue(FMath::Max(GetLastLODIndex(), 0))
								.Value(this, &SFoliageBakerMeshOutputDialog::GetReplaceLODIndex)
								.OnValueChanged(this, &SFoliageBakerMeshOutputDialog::SetReplaceLODIndex)
							]
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.ContentPadding(FMargin(12.0f, 8.0f))
						.IsEnabled(this, &SFoliageBakerMeshOutputDialog::CanReplaceLastLOD)
						.ToolTipText(this, &SFoliageBakerMeshOutputDialog::GetReplaceLastLODToolTip)
						.OnClicked(this, &SFoliageBakerMeshOutputDialog::ChooseReplaceLastLOD)
						[
							SNew(STextBlock)
							.Text(this, &SFoliageBakerMeshOutputDialog::GetReplaceLastLODText)
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 14.0f, 0.0f, 10.0f)
					[
						SNew(SSeparator)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.HAlign(HAlign_Right)
					[
						SNew(SButton)
						.ContentPadding(FMargin(18.0f, 5.0f))
						.Text(FText::FromString(TEXT("Cancel")))
						.OnClicked(this, &SFoliageBakerMeshOutputDialog::Cancel)
					]
				]
			];
		}

		TOptional<FFoliageBakerMeshOutputSelection> GetSelection() const
		{
			return Selection;
		}

	private:
		int32 GetLODCount() const
		{
			return SourceStaticMesh ? SourceStaticMesh->GetNumSourceModels() : 0;
		}

		int32 GetLastLODIndex() const
		{
			return GetLODCount() - 1;
		}

		bool CanReplaceLOD(const int32 LODIndex) const
		{
			return SourceStaticMesh
				&& SourceStaticMesh->IsSourceModelValid(LODIndex)
				&& LODIndex > SourceLODIndex;
		}

		bool CanReplaceSelectedLOD() const
		{
			return CanReplaceLOD(ReplaceLODIndex);
		}

		bool CanInsertAfterSelectedLOD() const
		{
			return SourceStaticMesh
				&& GetLODCount() < MAX_STATIC_MESH_LODS
				&& SourceStaticMesh->IsSourceModelValid(InsertAfterLODIndex)
				&& InsertAfterLODIndex >= SourceLODIndex;
		}

		bool CanReplaceLastLOD() const
		{
			const int32 LastLODIndex = GetLastLODIndex();
			return LastLODIndex > 0 && CanReplaceLOD(LastLODIndex);
		}

		TOptional<int32> GetReplaceLODIndex() const
		{
			return ReplaceLODIndex;
		}

		void SetReplaceLODIndex(const int32 Value)
		{
			ReplaceLODIndex = Value;
		}

		TOptional<int32> GetInsertAfterLODIndex() const
		{
			return InsertAfterLODIndex;
		}

		void SetInsertAfterLODIndex(const int32 Value)
		{
			InsertAfterLODIndex = Value;
		}

		FText GetInsertLODToolTip() const
		{
			if (GetLODCount() >= MAX_STATIC_MESH_LODS)
			{
				return FText::FromString(FString::Printf(
					TEXT("Unavailable because Static Meshes support at most %d LODs."),
					MAX_STATIC_MESH_LODS));
			}
			if (InsertAfterLODIndex < SourceLODIndex)
			{
				return FText::FromString(TEXT("Choose the selected source LOD or a later LOD so the source LOD is not renumbered."));
			}
			return FText::FromString(FString::Printf(
				TEXT("Insert the generated proxy as LOD%d and shift the existing LOD%d and every later LOD back by one."),
				InsertAfterLODIndex + 1,
				InsertAfterLODIndex + 1));
		}

		FText GetReplaceLastLODText() const
		{
			const int32 LastLODIndex = GetLastLODIndex();
			return LastLODIndex >= 0
				? FText::FromString(FString::Printf(TEXT("Replace Last LOD (LOD%d)"), LastLODIndex))
				: FText::FromString(TEXT("Replace Last LOD"));
		}

		FText GetReplaceLastLODToolTip() const
		{
			const int32 LastLODIndex = GetLastLODIndex();
			if (LastLODIndex <= 0)
			{
				return FText::FromString(TEXT("Unavailable because the source mesh only has LOD0."));
			}
			if (LastLODIndex == SourceLODIndex)
			{
				return FText::FromString(TEXT("Unavailable because the last LOD is the selected source LOD."));
			}
			return FText::FromString(FString::Printf(TEXT("Replace the existing last source LOD, LOD%d."), LastLODIndex));
		}

		FReply ChooseSeparateMesh()
		{
			return Confirm(EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset, INDEX_NONE, INDEX_NONE);
		}

		FReply ChooseAddLOD()
		{
			return Confirm(EFoliageBakerMeshAssetOutputMode::AddToSourceMeshLOD, INDEX_NONE, INDEX_NONE);
		}

		FReply ChooseInsertLOD()
		{
			return Confirm(
				EFoliageBakerMeshAssetOutputMode::InsertIntoSourceMeshLOD,
				INDEX_NONE,
				InsertAfterLODIndex);
		}

		FReply ChooseReplaceLOD()
		{
			return Confirm(EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD, ReplaceLODIndex, INDEX_NONE);
		}

		FReply ChooseReplaceLastLOD()
		{
			return Confirm(EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD, GetLastLODIndex(), INDEX_NONE);
		}

		FReply Cancel()
		{
			if (ParentWindow.IsValid())
			{
				ParentWindow.Pin()->RequestDestroyWindow();
			}
			return FReply::Handled();
		}

		FReply Confirm(
			const EFoliageBakerMeshAssetOutputMode OutputMode,
			const int32 ReplaceIndex,
			const int32 InsertAfterIndex)
		{
			FFoliageBakerMeshOutputSelection ResolvedSelection;
			ResolvedSelection.OutputMode = OutputMode;
			ResolvedSelection.ReplaceLODIndex = ReplaceIndex;
			ResolvedSelection.InsertAfterLODIndex = InsertAfterIndex;
			Selection = ResolvedSelection;
			return Cancel();
		}

		const UStaticMesh* SourceStaticMesh = nullptr;
		int32 SourceLODIndex = 0;
		int32 ReplaceLODIndex = 0;
		int32 InsertAfterLODIndex = 0;
		TWeakPtr<SWindow> ParentWindow;
		TOptional<FFoliageBakerMeshOutputSelection> Selection;
	};
}

TOptional<FFoliageBakerMeshOutputSelection> FFoliageBakerMeshOutputDialog::OpenAfterBake(
	const UStaticMesh& SourceStaticMesh,
	const int32 SourceLODIndex)
{
	if (!FSlateApplication::IsInitialized())
	{
		return TOptional<FFoliageBakerMeshOutputSelection>();
	}

	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::FromString(TEXT("Foliage Baker - Mesh Output")))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMaximize(false)
		.SupportsMinimize(false);
	const TSharedRef<SFoliageBakerMeshOutputDialog> Dialog = SNew(SFoliageBakerMeshOutputDialog)
		.SourceStaticMesh(&SourceStaticMesh)
		.SourceLODIndex(SourceLODIndex)
		.ParentWindow(Window);
	Window->SetContent(Dialog);
	FSlateApplication::Get().AddModalWindow(
		Window,
		FSlateApplication::Get().GetActiveTopLevelWindow(),
		false);
	return Dialog->GetSelection();
}

TOptional<FFoliageBakerExistingAssetDecision>
FFoliageBakerExistingAssetDialog::OpenIfNeeded(
	const TArray<FFoliageBakerGeneratedAssetPath>& GeneratedAssets,
	FString& OutError)
{
	OutError.Reset();
	if (GeneratedAssets.IsEmpty())
	{
		OutError =
			TEXT("No generated assets were included in the output plan.");
		return TOptional<FFoliageBakerExistingAssetDecision>();
	}

	TArray<FFoliageBakerGeneratedAssetPath> ConflictingAssets;
	TMap<FName, FString> PlannedAssetLabels;
	ConflictingAssets.Reserve(GeneratedAssets.Num());
	for (const FFoliageBakerGeneratedAssetPath& Asset : GeneratedAssets)
	{
		const FString ObjectPath = Asset.BuildObjectPath();
		if (ObjectPath.IsEmpty())
		{
			OutError =
				TEXT("The generated asset plan contains an empty object path.");
			return TOptional<FFoliageBakerExistingAssetDecision>();
		}

		const FString DisplayLabel = Asset.DisplayName.IsEmpty()
			? ObjectPath
			: Asset.DisplayName;
		const FName ObjectPathName(*ObjectPath);
		if (const FString* ExistingLabel =
			PlannedAssetLabels.Find(ObjectPathName))
		{
			OutError = FString::Printf(
				TEXT("Generated outputs '%s' and '%s' resolve to the same asset path: %s. Use distinct prefixes or suffixes."),
				*(*ExistingLabel),
				*DisplayLabel,
				*ObjectPath);
			return TOptional<FFoliageBakerExistingAssetDecision>();
		}
		PlannedAssetLabels.Add(ObjectPathName, DisplayLabel);

		if (DoesGeneratedAssetExist(ObjectPath))
		{
			ConflictingAssets.Add(Asset);
		}
	}

	if (ConflictingAssets.IsEmpty())
	{
		return FFoliageBakerExistingAssetDecision{
			EFoliageBakerExistingAssetPolicy::CreateOnly,
			0};
	}

	if (!FSlateApplication::IsInitialized())
	{
		return TOptional<FFoliageBakerExistingAssetDecision>();
	}

	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::FromString(
			TEXT("Foliage Baker - Existing Assets")))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMaximize(false)
		.SupportsMinimize(false);
	const TSharedRef<SFoliageBakerExistingAssetDialog> Dialog =
		SNew(SFoliageBakerExistingAssetDialog)
			.ConflictingAssets(MoveTemp(ConflictingAssets))
			.ParentWindow(Window);
	Window->SetContent(Dialog);
	FSlateApplication::Get().AddModalWindow(
		Window,
		FSlateApplication::Get().GetActiveTopLevelWindow(),
		false);

	const TOptional<EFoliageBakerExistingAssetPolicy> Policy =
		Dialog->GetPolicy();
	if (!Policy.IsSet())
	{
		return TOptional<FFoliageBakerExistingAssetDecision>();
	}
	if (Policy.GetValue()
		== EFoliageBakerExistingAssetPolicy::ReuseOrCreate)
	{
		return FFoliageBakerExistingAssetDecision{
			EFoliageBakerExistingAssetPolicy::ReuseOrCreate,
			0};
	}

	const int32 AssetNameVersion =
		FindAvailableAssetNameVersion(GeneratedAssets);
	if (AssetNameVersion == INDEX_NONE)
	{
		OutError =
			TEXT("Could not find an available shared name for the generated asset set.");
		return TOptional<FFoliageBakerExistingAssetDecision>();
	}
	return FFoliageBakerExistingAssetDecision{
		EFoliageBakerExistingAssetPolicy::CreateUnique,
		AssetNameVersion};
}
