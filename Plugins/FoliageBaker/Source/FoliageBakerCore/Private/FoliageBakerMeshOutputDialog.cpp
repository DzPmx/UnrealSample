#include "FoliageBakerMeshOutputDialog.h"

#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
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
								.MinValue(0)
								.MaxValue(FMath::Max(GetLastLODIndex(), 0))
								.MinSliderValue(0)
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
				&& LODIndex != SourceLODIndex;
		}

		bool CanReplaceSelectedLOD() const
		{
			return CanReplaceLOD(ReplaceLODIndex);
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
			return Confirm(EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset, INDEX_NONE);
		}

		FReply ChooseAddLOD()
		{
			return Confirm(EFoliageBakerMeshAssetOutputMode::AddToSourceMeshLOD, INDEX_NONE);
		}

		FReply ChooseReplaceLOD()
		{
			return Confirm(EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD, ReplaceLODIndex);
		}

		FReply ChooseReplaceLastLOD()
		{
			return Confirm(EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD, GetLastLODIndex());
		}

		FReply Cancel()
		{
			if (ParentWindow.IsValid())
			{
				ParentWindow.Pin()->RequestDestroyWindow();
			}
			return FReply::Handled();
		}

		FReply Confirm(const EFoliageBakerMeshAssetOutputMode OutputMode, const int32 ReplaceIndex)
		{
			FFoliageBakerMeshOutputSelection ResolvedSelection;
			ResolvedSelection.OutputMode = OutputMode;
			ResolvedSelection.ReplaceLODIndex = ReplaceIndex;
			Selection = ResolvedSelection;
			return Cancel();
		}

		const UStaticMesh* SourceStaticMesh = nullptr;
		int32 SourceLODIndex = 0;
		int32 ReplaceLODIndex = 0;
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
