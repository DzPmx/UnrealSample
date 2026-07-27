#include "SHThicknessBakeCore.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PropertyCustomizationHelpers.h"
#include "RenderingThread.h"
#include "ScopedTransaction.h"
#include "ShaderCore.h"
#include "StaticMeshOperations.h"
#include "Styling/AppStyle.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "ToolMenus.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "TextureBakerEditor"

namespace
{

using namespace SHThicknessBaker;

const FName TextureBakerTabName(TEXT("TextureBaker"));
constexpr int32 SHThicknessBakerIndex = 0;
constexpr int32 CPUBakeModeIndex = 0;
constexpr int32 GPUBakeModeIndex = 1;

struct FSourceMeshSelection
{
	TStrongObjectPtr<UObject> Mesh;
};

TSharedRef<SWidget> MakeIntegerSettingRow(
	const FText& Label,
	const FText& ToolTip,
	const int32 MinValue,
	const int32 MaxValue,
	const int32 Delta,
	const TAttribute<int32>& Value,
	const SSpinBox<int32>::FOnValueChanged& OnValueChanged)
{
	return SNew(SHorizontalBox)
		.ToolTipText(ToolTip)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(Label)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SBox)
			.WidthOverride(120.0f)
			[
				SNew(SSpinBox<int32>)
				.MinValue(MinValue)
				.MaxValue(MaxValue)
				.Delta(Delta)
				.AlwaysUsesDeltaSnap(true)
				.Value(Value)
				.OnValueChanged(OnValueChanged)
			]
		];
}

} // namespace

class FTextureBakerEditorModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const TSharedPtr<IPlugin> Plugin =
			IPluginManager::Get().FindPlugin(TEXT("TextureBaker"));
		check(Plugin.IsValid());
		AddShaderSourceDirectoryMapping(
			TEXT("/Plugin/TextureBaker"),
			FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));

		if (GEngine != nullptr)
		{
			RegisterEditorUI();
		}
		else
		{
			PostEngineInitHandle =
				FCoreDelegates::OnPostEngineInit.AddRaw(
					this,
					&FTextureBakerEditorModule::RegisterEditorUI);
		}
	}

	virtual void ShutdownModule() override
	{
		if (PostEngineInitHandle.IsValid())
		{
			FCoreDelegates::OnPostEngineInit.Remove(
				PostEngineInitHandle);
			PostEngineInitHandle.Reset();
		}

		if (const TSharedPtr<SNotificationItem> Notification = ActiveNotification.Pin())
		{
			Notification->SetCompletionState(SNotificationItem::CS_None);
			Notification->ExpireAndFadeout();
		}
		if (ActiveJob.IsValid())
		{
			ActiveJob->RequestCancel();
		}
		if (ActiveFuture.IsValid())
		{
			// GPU waits observe cancellation; then drain any render lambdas
			// that still hold plugin-owned dispatch contexts.
			ActiveFuture.Wait();
			FlushRenderingCommands();
		}
		if (ActiveTickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(ActiveTickerHandle);
			ActiveTickerHandle.Reset();
		}
		ActiveNotification.Reset();
		ActiveJob.Reset();
		ActiveSourceMeshes.Reset();

		if (bEditorUIRegistered)
		{
			UToolMenus::UnRegisterStartupCallback(this);
			UToolMenus::UnregisterOwner(this);
			if (FSlateApplication::IsInitialized())
			{
				if (const TSharedPtr<SDockTab> LiveTab =
					FGlobalTabmanager::Get()->FindExistingLiveTab(TextureBakerTabName))
				{
					LiveTab->RequestCloseTab();
				}
				FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TextureBakerTabName);
			}
		}
		SelectedSourceMeshes.Reset();
		SourceMeshListBox.Reset();
	}

private:
	void RegisterEditorUI()
	{
		if (bEditorUIRegistered)
		{
			return;
		}

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
			TextureBakerTabName,
			FOnSpawnTab::CreateRaw(this, &FTextureBakerEditorModule::SpawnToolTab))
			.SetDisplayName(LOCTEXT("TextureBakerTabTitle", "Texture Baker"))
			.SetTooltipText(LOCTEXT(
				"TextureBakerTabToolTip",
				"Bake mesh-derived data into textures."))
			.SetIcon(FSlateIcon(
				FAppStyle::GetAppStyleSetName(),
				"LevelEditor.Tabs.Details"))
			.SetMenuType(ETabSpawnerMenuType::Hidden);

		UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateRaw(
				this,
				&FTextureBakerEditorModule::RegisterMenus));
		bEditorUIRegistered = true;
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(
			TEXT("LevelEditor.MainMenu.Tools"));
		if (Menu == nullptr)
		{
			return;
		}

		FToolMenuSection& Section =
			Menu->FindOrAddSection(TEXT("TextureBaker"));
		Section.Label = LOCTEXT("TextureBakerMenuSection", "Texture Baker");
		Section.AddMenuEntry(
			TEXT("TextureBaker.OpenWindow"),
			LOCTEXT("OpenTextureBakerLabel", "Texture Baker"),
			LOCTEXT(
				"OpenTextureBakerToolTip",
				"Open the Texture Baker tool."),
			FSlateIcon(
				FAppStyle::GetAppStyleSetName(),
				"LevelEditor.Tabs.Details"),
			FToolMenuExecuteAction::CreateRaw(
				this,
				&FTextureBakerEditorModule::ExecuteOpenTool));
	}

	void ExecuteOpenTool(const FToolMenuContext& MenuContext)
	{
		(void)MenuContext;
		FGlobalTabmanager::Get()->TryInvokeTab(TextureBakerTabName);
	}

	bool HasSkeletalSource() const
	{
		for (const TSharedPtr<FSourceMeshSelection>& Selection :
			SelectedSourceMeshes)
		{
			if (Selection.IsValid()
				&& Selection->Mesh.IsValid()
				&& Selection->Mesh->IsA<USkeletalMesh>())
			{
				return true;
			}
		}
		return false;
	}

	bool AreSourceSlotsReady() const
	{
		if (SelectedSourceMeshes.IsEmpty())
		{
			return false;
		}
		for (const TSharedPtr<FSourceMeshSelection>& Selection :
			SelectedSourceMeshes)
		{
			if (!Selection.IsValid()
				|| !Selection->Mesh.IsValid())
			{
				return false;
			}
		}
		return true;
	}

	void HandleSourceMeshChanged(
		const FAssetData& AssetData,
		const TSharedPtr<FSourceMeshSelection>& Selection)
	{
		if (!Selection.IsValid())
		{
			return;
		}

		UObject* Asset = AssetData.GetAsset();
		if (Asset == nullptr
			|| (!Asset->IsA<UStaticMesh>()
				&& !Asset->IsA<USkeletalMesh>()))
		{
			Selection->Mesh.Reset();
		}
		else
		{
			Selection->Mesh.Reset(Asset);
		}

		if (HasSkeletalSource())
		{
			PanelSettings.CoefficientSpace =
				ECoefficientSpace::Tangent;
		}
	}

	void RebuildSourceMeshList()
	{
		if (!SourceMeshListBox.IsValid())
		{
			return;
		}

		SourceMeshListBox->ClearChildren();
		for (const TSharedPtr<FSourceMeshSelection>& Selection :
			SelectedSourceMeshes)
		{
			const TWeakPtr<FSourceMeshSelection> WeakSelection =
				Selection;
			SourceMeshListBox->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SBox)
						.MinDesiredHeight(64.0f)
						[
							SNew(SObjectPropertyEntryBox)
								.AllowedClass(UObject::StaticClass())
								.OnShouldFilterAsset_Lambda(
									[](const FAssetData& AssetData)
									{
										return !AssetData.IsInstanceOf(
												UStaticMesh::StaticClass())
											&& !AssetData.IsInstanceOf(
												USkeletalMesh::StaticClass());
									})
								.OnShouldSetAsset_Lambda(
									[](const FAssetData& AssetData)
									{
										return !AssetData.IsValid()
											|| AssetData.IsInstanceOf(
												UStaticMesh::StaticClass())
											|| AssetData.IsInstanceOf(
												USkeletalMesh::StaticClass());
									})
								.AllowClear(true)
								.AllowCreate(false)
								.DisplayUseSelected(true)
								.DisplayBrowse(true)
								.DisplayCompactSize(false)
								.DisplayThumbnail(true)
								.ThumbnailSizeOverride(
									FIntPoint(64, 64))
								.ThumbnailPool(
									UThumbnailManager::Get()
										.GetSharedThumbnailPool())
								.ObjectPath_Lambda([WeakSelection]()
								{
									const TSharedPtr<
										FSourceMeshSelection>
										PinnedSelection =
											WeakSelection.Pin();
									const UObject* Mesh =
										PinnedSelection.IsValid()
											? PinnedSelection->Mesh.Get()
											: nullptr;
									return Mesh != nullptr
										? Mesh->GetPathName()
										: FString();
								})
								.OnObjectChanged(
									FOnSetObject::CreateLambda(
										[this, WeakSelection](
											const FAssetData& AssetData)
										{
											HandleSourceMeshChanged(
												AssetData,
												WeakSelection.Pin());
										}))
						]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
						.Text(LOCTEXT(
							"RemoveSourceMesh",
							"Remove"))
						.ToolTipText(LOCTEXT(
							"RemoveSourceMeshTip",
							"Remove this mesh from the group bake."))
						.OnClicked_Lambda(
							[this, WeakSelection]()
							{
								const TSharedPtr<
									FSourceMeshSelection>
									PinnedSelection =
										WeakSelection.Pin();
								SelectedSourceMeshes.Remove(
									PinnedSelection);
								if (SelectedSourceMeshes.IsEmpty())
								{
									SelectedSourceMeshes.Add(
										MakeShared<
											FSourceMeshSelection>());
								}
								RebuildSourceMeshList();
								return FReply::Handled();
							})
				]
			];
		}
	}

	TSharedRef<SDockTab> SpawnToolTab(const FSpawnTabArgs& SpawnTabArgs)
	{
		(void)SpawnTabArgs;
		if (SelectedSourceMeshes.IsEmpty())
		{
			SelectedSourceMeshes.Add(
				MakeShared<FSourceMeshSelection>());
		}

		TSharedRef<SSegmentedControl<int32>> BakerTypeSelector =
			SNew(SSegmentedControl<int32>)
			.Value_Lambda([this]()
			{
				return ActiveBakerTypeIndex;
			})
			.OnValueChanged_Lambda([this](const int32 NewIndex)
			{
				check(NewIndex == SHThicknessBakerIndex);
				ActiveBakerTypeIndex = NewIndex;
			})
			.UniformPadding(FMargin(18.0f, 6.0f))
			.MaxSegmentsPerLine(1)
			.IsEnabled_Lambda([this]()
			{
				return !ActiveJob.IsValid();
			});
		BakerTypeSelector->AddSlot(SHThicknessBakerIndex)
			.HAlign(HAlign_Center)
			.Text(LOCTEXT("SHThicknessBakerType", "SH Thickness"));

		TSharedRef<SSegmentedControl<int32>> BakeModeSelector =
			SNew(SSegmentedControl<int32>)
			.Value_Lambda([this]()
			{
				return PanelSettings.BakeMode == EBakeMode::GPU
					? GPUBakeModeIndex
					: CPUBakeModeIndex;
			})
			.OnValueChanged_Lambda([this](const int32 NewMode)
			{
				check(
					NewMode == CPUBakeModeIndex
					|| NewMode == GPUBakeModeIndex);
				PanelSettings.BakeMode =
					NewMode == GPUBakeModeIndex
						? EBakeMode::GPU
						: EBakeMode::CPU;
			})
			.UniformPadding(FMargin(18.0f, 6.0f))
			.MaxSegmentsPerLine(2);
		BakeModeSelector->AddSlot(CPUBakeModeIndex)
			.HAlign(HAlign_Center)
			.Text(LOCTEXT("CPUBakeMode", "CPU"))
			.ToolTip(LOCTEXT(
				"CPUBakeModeTip",
				"Reference implementation using UE's CPU mesh AABB tree and double-precision watertight ray tests."));
		BakeModeSelector->AddSlot(GPUBakeModeIndex)
			.HAlign(HAlign_Center)
			.Text(LOCTEXT("GPUBakeMode", "GPU"))
			.ToolTip(LOCTEXT(
				"GPUBakeModeTip",
				"Compute Shader implementation using an uploaded mesh BVH. It keeps the same bake contract and is intended for faster high-sample bakes."));

		TSharedRef<SSegmentedControl<ECoefficientSpace>>
			BakeSpaceSelector =
				SNew(SSegmentedControl<ECoefficientSpace>)
				.Value_Lambda([this]()
				{
					return PanelSettings.CoefficientSpace;
				})
				.OnValueChanged_Lambda(
					[this](const ECoefficientSpace NewSpace)
					{
						check(
							NewSpace == ECoefficientSpace::Tangent
								|| NewSpace
									== ECoefficientSpace::Local);
						if (!HasSkeletalSource())
						{
							PanelSettings.CoefficientSpace =
								NewSpace;
						}
					})
				.IsEnabled_Lambda([this]()
				{
					return !HasSkeletalSource();
				})
				.UniformPadding(FMargin(18.0f, 6.0f))
				.MaxSegmentsPerLine(2);
		BakeSpaceSelector->AddSlot(
			ECoefficientSpace::Tangent)
			.HAlign(HAlign_Center)
			.Text(LOCTEXT("TangentBakeSpace", "Tangent Space"))
			.ToolTip(LOCTEXT(
				"TangentBakeSpaceTip",
				"Store SH directions in the final render tangent basis. Required for SkeletalMesh so directions follow the skinned surface basis."));
		BakeSpaceSelector->AddSlot(
			ECoefficientSpace::Local)
			.HAlign(HAlign_Center)
			.Text(LOCTEXT("LocalBakeSpace", "Local Space"))
			.ToolTip(LOCTEXT(
				"LocalBakeSpaceTip",
				"Store SH directions in StaticMesh local/object axes. Transform the runtime query direction into local space before the dot product."));

		TSharedRef<SSegmentedControl<int32>> BakeUVSelector =
			SNew(SSegmentedControl<int32>)
			.Value_Lambda([this]()
			{
				return PanelSettings.BakeUVChannel;
			})
			.OnValueChanged_Lambda([this](const int32 NewChannel)
			{
				check(
					NewChannel == 0
						|| NewChannel == 1
						|| NewChannel == 2);
				PanelSettings.BakeUVChannel = NewChannel;
				if (NewChannel == 0)
				{
					PanelSettings.bRegenerateBakeUV = false;
				}
			})
			.UniformPadding(FMargin(18.0f, 6.0f))
			.MaxSegmentsPerLine(3);
		BakeUVSelector->AddSlot(0)
			.HAlign(HAlign_Center)
			.Text(LOCTEXT("BakeUV0", "UV0"));
		BakeUVSelector->AddSlot(1)
			.HAlign(HAlign_Center)
			.Text(LOCTEXT("BakeUV1", "UV1"));
		BakeUVSelector->AddSlot(2)
			.HAlign(HAlign_Center)
			.Text(LOCTEXT("BakeUV2", "UV2"));

		TSharedRef<SSegmentedControl<ETextureResolution>>
			TextureResolutionSelector =
				SNew(SSegmentedControl<ETextureResolution>)
				.Value_Lambda([this]()
				{
					return PanelSettings.TextureResolution;
				})
				.OnValueChanged_Lambda(
					[this](const ETextureResolution NewResolution)
					{
						PanelSettings.TextureResolution = NewResolution;
					})
				.UniformPadding(FMargin(12.0f, 6.0f))
				.MaxSegmentsPerLine(4);
		TextureResolutionSelector->AddSlot(
			ETextureResolution::Size256)
			.HAlign(HAlign_Center)
			.Text(LOCTEXT("TextureResolution256", "256"));
		TextureResolutionSelector->AddSlot(
			ETextureResolution::Size512)
			.HAlign(HAlign_Center)
			.Text(LOCTEXT("TextureResolution512", "512"));
		TextureResolutionSelector->AddSlot(
			ETextureResolution::Size1024)
			.HAlign(HAlign_Center)
			.Text(LOCTEXT("TextureResolution1024", "1024"));
		TextureResolutionSelector->AddSlot(
			ETextureResolution::Size2048)
			.HAlign(HAlign_Center)
			.Text(LOCTEXT("TextureResolution2048", "2048"));

		TSharedRef<SVerticalBox> InputPanel =
			SNew(SVerticalBox)
			.IsEnabled_Lambda([this]()
			{
				return !ActiveJob.IsValid();
			})
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("BakerTypeLabel", "Baker type"))
				.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 14.0f)
			[
				BakerTypeSelector
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SourceMeshesLabel", "Source Meshes"))
				.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SAssignNew(SourceMeshListBox, SVerticalBox)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Left)
			.Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				SNew(SButton)
					.Text(LOCTEXT(
						"AddSourceMesh",
						"Add Mesh"))
					.ToolTipText(LOCTEXT(
						"AddSourceMeshTip",
						"Add another mesh at its authored identity local transform. Every mesh contributes to shared thickness rays and receives its own output texture."))
					.OnClicked_Lambda([this]()
					{
						SelectedSourceMeshes.Add(
							MakeShared<FSourceMeshSelection>());
						RebuildSourceMeshList();
						return FReply::Handled();
					})
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 14.0f)
			[
				SNew(STextBlock)
					.Text(LOCTEXT(
						"InPlaceModificationWarning",
						"All selected assets are combined at their authored identity Local transform for thickness rays, and each asset outputs its own texture using the shared UV channel. Tangent Space works for StaticMesh and SkeletalMesh; Local Space requires every source to be StaticMesh. SkeletalMesh uses its reference pose. UV0 is reuse-only. UV1 and UV2 can be reused or regenerated with XAtlas; UV2 requires every source mesh to already contain UV1.\n\nThickness definition: the tool applies one transient shared combined-Bounds normalization, then stores the farthest two-sided triangle hit for each direction. A direction with no qualifying hit is zero. This is not SolidLength: inside intervals are not paired or summed, and air gaps may contribute. Source-asset positions are not changed."))
					.AutoWrapText(true)
					.ColorAndOpacity(FLinearColor(1.0f, 0.65f, 0.15f))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(STextBlock)
					.Text(LOCTEXT("SHThicknessSettingsLabel", "SH Thickness settings"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
					.Text(LOCTEXT("BakeModeLabel", "Bake mode"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				BakeModeSelector
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
					.Text(LOCTEXT("BakeSpaceLabel", "Bake space"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				BakeSpaceSelector
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
					.Text(LOCTEXT("BakeUVChannelLabel", "Bake UV channel"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				BakeUVSelector
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f, 0.0f, 8.0f)
			[
				SNew(SCheckBox)
					.ToolTipText(LOCTEXT(
						"RegenerateBakeUVTip",
						"Unchecked: use the existing selected UV without changing it. Checked: generate a new XAtlas layout and replace or add UV1/UV2. UV0 is reuse-only. Adding UV2 requires every source mesh to already contain UV1."))
					.IsEnabled_Lambda([this]()
					{
						return PanelSettings.BakeUVChannel != 0;
					})
					.IsChecked_Lambda([this]()
					{
						return PanelSettings.bRegenerateBakeUV
							? ECheckBoxState::Checked
							: ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda(
						[this](const ECheckBoxState NewState)
						{
							PanelSettings.bRegenerateBakeUV =
								NewState == ECheckBoxState::Checked;
						})
					[
						SNew(STextBlock)
							.Text(LOCTEXT(
								"RegenerateBakeUVLabel",
								"Regenerate selected UV with XAtlas"))
					]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f, 0.0f, 8.0f)
			[
				SNew(SCheckBox)
					.ToolTipText(LOCTEXT(
						"RemapCoefficientRangeTip",
						"Unchecked: keep the bounds-normalized SH coefficients unchanged. Checked: after filtering every group output, apply one linear gain shared by all outputs and all four coefficients so signed RGB stays within 0.1-0.9 and C0 stays within 0-0.9. This improves RGBA8 range usage without changing the material decode formula. Outputs within one bake remain comparable; separately baked groups do not."))
					.IsChecked_Lambda([this]()
					{
						return PanelSettings.bRemapCoefficientRange
							? ECheckBoxState::Checked
							: ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda(
						[this](const ECheckBoxState NewState)
						{
							PanelSettings.bRemapCoefficientRange =
								NewState == ECheckBoxState::Checked;
						})
					[
						SNew(STextBlock)
							.Text(LOCTEXT(
								"RemapCoefficientRangeLabel",
								"Remap SH range for RGBA8"))
					]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
					.Text(LOCTEXT(
						"ResolutionLabel",
						"Texture resolution"))
					.ToolTipText(LOCTEXT(
						"ResolutionTip",
						"Texture width and height. Available values: 256, 512, 1024, and 2048. Default: 1024."))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				TextureResolutionSelector
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f)
			[
				MakeIntegerSettingRow(
					LOCTEXT("DirectionCountLabel", "Sphere directions"),
					LOCTEXT(
						"DirectionCountTip",
						"Even antipodally paired direction count, 8-256. Default high-quality value: 256."),
					8,
					256,
					2,
					TAttribute<int32>::CreateLambda([this]()
					{
						return PanelSettings.DirectionCount;
					}),
					SSpinBox<int32>::FOnValueChanged::CreateLambda(
						[this](const int32 Value)
						{
							PanelSettings.DirectionCount = Value;
						}))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f)
			[
				MakeIntegerSettingRow(
					LOCTEXT("SampleGridLabel", "Subpixel sample grid"),
					LOCTEXT(
						"SampleGridTip",
						"Square grid dimension, 1-4. Actual samples per pixel are grid dimension squared. Default high-quality value: 4."),
					1,
					4,
					1,
					TAttribute<int32>::CreateLambda([this]()
					{
						return FMath::RoundToInt(FMath::Sqrt(
							static_cast<float>(
								PanelSettings.SamplesPerPixel)));
					}),
					SSpinBox<int32>::FOnValueChanged::CreateLambda(
						[this](const int32 Value)
						{
							PanelSettings.SamplesPerPixel = Value * Value;
						}))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f)
			[
				MakeIntegerSettingRow(
					LOCTEXT("PaddingLabel", "Padding (texels)"),
					LOCTEXT(
						"PaddingTip",
						"Exact output-dilation texels in both modes, and approximate final-texture chart spacing when XAtlas regeneration is enabled. Reusing an existing UV does not increase its chart spacing. Default: 16."),
					0,
					64,
					1,
					TAttribute<int32>::CreateLambda([this]()
					{
						return PanelSettings.PaddingSize;
					}),
					SSpinBox<int32>::FOnValueChanged::CreateLambda(
						[this](const int32 Value)
						{
							PanelSettings.PaddingSize = Value;
						}))
			];

		RebuildSourceMeshList();

		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				.Padding(12.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush(
								"ToolPanel.GroupBorder"))
							.Padding(FMargin(14.0f, 12.0f))
							[
								SNew(SVerticalBox)
								+ SVerticalBox::Slot()
								.AutoHeight()
								[
									SNew(STextBlock)
										.Text(LOCTEXT(
											"TextureBakerHeader",
											"Texture Baker"))
										.TextStyle(
											FAppStyle::Get(),
											"LargeText")
								]
								+ SVerticalBox::Slot()
								.AutoHeight()
								.Padding(0.0f, 3.0f, 0.0f, 0.0f)
								[
									SNew(STextBlock)
										.Text(LOCTEXT(
											"TextureBakerSubtitle",
											"Bake mesh-derived fields into reusable texture assets."))
										.AutoWrapText(true)
								]
							]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush(
								"ToolPanel.GroupBorder"))
							.Padding(FMargin(14.0f, 12.0f))
							[
								InputPanel
							]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush(
								"ToolPanel.GroupBorder"))
							.Padding(FMargin(14.0f, 12.0f))
							[
								SNew(STextBlock)
									.Text(LOCTEXT(
										"OutputSummary",
										"Output: one linear RGBA8 coefficient texture per source mesh, cooked as BC7 with a normal mip chain. The group shares bounds normalization and one optional Remap gain. Logical RGBA = (Cx, Cy, Cz, C0); decode Cxyz = 2*RGB-1 and C0 = A. Evaluate max(0, C0 + dot(Cxyz, D)), where D is tangent-space or mesh-local according to Bake Space. Each material Texture Coordinate must match the shared UV0/UV1/UV2 selection. Names use T_<mesh name without SM_>_Thickness_M."))
									.AutoWrapText(true)
							]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.HAlign(HAlign_Right)
					[
						SNew(SButton)
							.Text(LOCTEXT("BakeButton", "Bake"))
							.IsEnabled_Lambda([this]()
							{
								return AreSourceSlotsReady()
									&& !ActiveJob.IsValid();
							})
							.OnClicked_Raw(
								this,
								&FTextureBakerEditorModule::HandleBakeClicked)
					]
				]
			];
	}

	FReply HandleBakeClicked()
	{
		if (ActiveJob.IsValid())
		{
			FMessageDialog::Open(
				EAppMsgType::Ok,
				LOCTEXT(
					"BakeAlreadyRunning",
					"A Texture Baker job is already running. Cancel it or wait for it to finish."));
			return FReply::Handled();
		}

		if (!AreSourceSlotsReady())
		{
			FMessageDialog::Open(
				EAppMsgType::Ok,
				LOCTEXT(
					"SelectSourceMeshes",
					"Every source slot must contain a StaticMesh or SkeletalMesh."));
			return FReply::Handled();
		}

		TArray<UObject*> SourceMeshes;
		SourceMeshes.Reserve(SelectedSourceMeshes.Num());
		for (const TSharedPtr<FSourceMeshSelection>& Selection :
			SelectedSourceMeshes)
		{
			SourceMeshes.Add(Selection->Mesh.Get());
		}

		switch (ActiveBakerTypeIndex)
		{
		case SHThicknessBakerIndex:
			StartSHThicknessBake(SourceMeshes, PanelSettings);
			break;
		default:
			checkNoEntry();
			break;
		}
		return FReply::Handled();
	}

	void StartSHThicknessBake(
		const TConstArrayView<UObject*> SourceMeshes,
		const FBakeSettings& Settings)
	{
		FBakePreparation Preparation;
		FText Error;
		if (!PrepareBake(
				SourceMeshes,
				Settings,
				Preparation,
				Error))
		{
			FMessageDialog::Open(EAppMsgType::Ok, Error);
			return;
		}

		if (!Preparation.Warnings.IsEmpty())
		{
			FString WarningText = TEXT("Warnings:\n\n");
			for (const FText& Warning : Preparation.Warnings)
			{
				WarningText += TEXT("- ");
				WarningText += Warning.ToString();
				WarningText += TEXT("\n");
			}
			WarningText += TEXT(
				"\nContinue? Selected source UVs are modified only when XAtlas regeneration is enabled, only after every output succeeds, and in one editor transaction. For StaticMesh, automatic lightmap UV generation is disabled during that same commit only when listed above.");
			if (FMessageDialog::Open(
				EAppMsgType::OkCancel,
				FText::FromString(WarningText)) != EAppReturnType::Ok)
			{
				return;
			}
		}

		ActiveJob = MakeShared<FBakeJob, ESPMode::ThreadSafe>(
			MoveTemp(Preparation));
		ActiveSourceMeshes.Reset(SourceMeshes.Num());
		for (UObject* SourceMesh : SourceMeshes)
		{
			ActiveSourceMeshes.Emplace(SourceMesh);
		}
		const TSharedPtr<FBakeJob, ESPMode::ThreadSafe> Job = ActiveJob;
		ActiveFuture = Async(EAsyncExecution::ThreadPool, [Job]()
		{
			Job->Run();
		});

		FNotificationInfo NotificationInfo(LOCTEXT(
			"BakeInProgress",
			"Baking SH thickness texture..."));
		NotificationInfo.bFireAndForget = false;
		NotificationInfo.ExpireDuration = 0.0f;
		NotificationInfo.FadeOutDuration = 1.0f;
		NotificationInfo.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("CancelBakeButton", "Cancel"),
			LOCTEXT(
				"CancelBakeButtonTip",
				"Cancel the active SH thickness bake."),
			FSimpleDelegate::CreateRaw(
				this,
				&FTextureBakerEditorModule::CancelActiveBake)));
		ActiveNotification =
			FSlateNotificationManager::Get().AddNotification(NotificationInfo);
		if (const TSharedPtr<SNotificationItem> Notification =
			ActiveNotification.Pin())
		{
			Notification->SetCompletionState(SNotificationItem::CS_Pending);
		}

		ActiveTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(
				this,
				&FTextureBakerEditorModule::TickActiveBake),
			0.1f);
	}

	void CancelActiveBake()
	{
		if (ActiveJob.IsValid())
		{
			ActiveJob->RequestCancel();
		}
	}

	bool TickActiveBake(float DeltaTime)
	{
		(void)DeltaTime;
		if (!ActiveJob.IsValid())
		{
			ActiveTickerHandle.Reset();
			return false;
		}

		if (const TSharedPtr<SNotificationItem> Notification =
			ActiveNotification.Pin())
		{
			Notification->SetText(
				FText::FromString(ActiveJob->GetStatusText()));
		}

		if (!ActiveFuture.IsValid() || !ActiveFuture.IsReady())
		{
			return true;
		}

		ActiveFuture.Get();
		const EJobStage Stage = ActiveJob->Stage.load();
		const bool bCancelled =
			ActiveJob->bCancelRequested.load(std::memory_order_relaxed);
		if (Stage == EJobStage::Succeeded && !bCancelled)
		{
			FText CommitError;
			if (CommitBakeAssets(*ActiveJob, CommitError))
			{
				const int32 TextureCount =
					ActiveJob->Preparation.Targets.Num();
				const FText SuccessText =
					ActiveJob->Preparation.Settings.bRegenerateBakeUV
						? FText::Format(
							LOCTEXT(
								"BakeSucceededWithRegeneratedUV",
								"{0} SH thickness texture(s) created or updated, and every source-mesh UV{1} updated."),
							FText::AsNumber(TextureCount),
							FText::AsNumber(
								ActiveJob->Preparation.Settings.BakeUVChannel))
						: FText::Format(
							LOCTEXT(
								"BakeSucceededWithExistingUV",
								"{0} SH thickness texture(s) created or updated using existing UV{1}; source UVs were not modified."),
							FText::AsNumber(TextureCount),
							FText::AsNumber(
								ActiveJob->Preparation.Settings.BakeUVChannel));
				FinishNotification(
					SuccessText,
					SNotificationItem::CS_Success);
			}
			else
			{
				FinishNotification(
					CommitError,
					SNotificationItem::CS_Fail);
				FMessageDialog::Open(EAppMsgType::Ok, CommitError);
			}
		}
		else if (Stage == EJobStage::Cancelled || bCancelled)
		{
			FinishNotification(
				LOCTEXT(
					"BakeCancelled",
					"SH thickness bake cancelled. Source meshes were not modified."),
				SNotificationItem::CS_None);
		}
		else
		{
			const FText Failure =
				FText::FromString(ActiveJob->GetStatusText());
			FinishNotification(Failure, SNotificationItem::CS_Fail);
			FMessageDialog::Open(EAppMsgType::Ok, Failure);
		}

		ActiveJob.Reset();
		ActiveSourceMeshes.Reset();
		ActiveTickerHandle.Reset();
		return false;
	}

	void FinishNotification(
		const FText& Text,
		const SNotificationItem::ECompletionState CompletionState)
	{
		if (const TSharedPtr<SNotificationItem> Notification =
			ActiveNotification.Pin())
		{
			Notification->SetText(Text);
			Notification->SetCompletionState(CompletionState);
			Notification->ExpireAndFadeout();
		}
		ActiveNotification.Reset();
	}

	bool CommitBakeAssets(FBakeJob& Job, FText& OutError)
	{
		check(IsInGameThread());
		if (Job.Preparation.Targets.IsEmpty()
			|| Job.EncodedRGBA.Num()
				!= Job.Preparation.Targets.Num())
		{
			OutError = LOCTEXT(
				"EncodedTextureCountMismatch",
				"The number of encoded coefficient textures does not match the prepared source meshes. No changes were committed.");
			return false;
		}

		struct FPendingCommit
		{
			const FBakeTargetPreparation* Preparation = nullptr;
			UObject* SourceMesh = nullptr;
			UStaticMesh* StaticMesh = nullptr;
			USkeletalMesh* SkeletalMesh = nullptr;
			FMeshDescription CurrentMeshDescription;
			bool bSourcePackageWasDirty = false;
			FString TexturePackageName;
			FString TextureAssetName;
			UPackage* TexturePackage = nullptr;
			UTexture2D* Texture = nullptr;
			bool bTexturePackageWasDirty = false;
			bool bTextureIsNew = false;
		};

		const int32 Resolution = GetTextureResolution(
			Job.Preparation.Settings.TextureResolution);
		const int64 PixelCount =
			static_cast<int64>(Resolution) * Resolution;
		TArray<FPendingCommit> PendingCommits;
		PendingCommits.Reserve(Job.Preparation.Targets.Num());
		TSet<FString> PendingTexturePaths;

		for (int32 TargetIndex = 0;
			TargetIndex < Job.Preparation.Targets.Num();
			++TargetIndex)
		{
			const FBakeTargetPreparation& Preparation =
				Job.Preparation.Targets[TargetIndex];
			FPendingCommit& Pending =
				PendingCommits.AddDefaulted_GetRef();
			Pending.Preparation = &Preparation;
			Pending.SourceMesh = Preparation.SourceMesh.Get();
			Pending.StaticMesh =
				Cast<UStaticMesh>(Pending.SourceMesh);
			Pending.SkeletalMesh =
				Cast<USkeletalMesh>(Pending.SourceMesh);

			if (Pending.SourceMesh == nullptr)
			{
				OutError = LOCTEXT(
					"SourceMeshUnloaded",
					"A source mesh was unloaded before the bake completed. No changes were committed.");
				return false;
			}
			Pending.bSourcePackageWasDirty =
				Pending.SourceMesh->GetOutermost()->IsDirty();
			if ((Preparation.bSourceIsSkeletalMesh
					&& Pending.SkeletalMesh == nullptr)
				|| (!Preparation.bSourceIsSkeletalMesh
					&& Pending.StaticMesh == nullptr))
			{
				OutError = FText::Format(
					LOCTEXT(
						"SourceMeshTypeChanged",
						"{0}: the source mesh type no longer matches the prepared bake. No changes were committed."),
					FText::FromString(
						Pending.SourceMesh->GetName()));
				return false;
			}

			bool bSourceStateValid = false;
			if (Pending.StaticMesh != nullptr)
			{
				bSourceStateValid =
					Pending.StaticMesh->IsSourceModelValid(0)
					&& Pending.StaticMesh
						->IsMeshDescriptionValid(0)
					&& !Pending.StaticMesh->IsReductionActive(0)
					&& !Pending.StaticMesh->IsNaniteEnabled()
					&& Pending.StaticMesh->CloneMeshDescription(
						0,
						Pending.CurrentMeshDescription)
					&& Pending.StaticMesh->GetSourceModel(0)
						.BuildSettings
						== Preparation.SourceBuildSettings
					&& Pending.StaticMesh
						->GetLegacyTangentScaling()
						== Preparation.bUseLegacyTangentScaling;
			}
			else
			{
				const FSkeletalMeshLODInfo* LODInfo =
					Pending.SkeletalMesh->GetLODInfo(0);
				bSourceStateValid =
					Pending.SkeletalMesh->IsSourceModelValid(0)
					&& Pending.SkeletalMesh
						->HasMeshDescription(0)
					&& LODInfo != nullptr
					&& !LODInfo->bHasBeenSimplified
					&& !Pending.SkeletalMesh->IsNaniteEnabled()
					&& Pending.SkeletalMesh->CloneMeshDescription(
						0,
						Pending.CurrentMeshDescription)
					&& LODInfo->BuildSettings
						== Preparation
							.SourceSkeletalBuildSettings;
			}
			if (!bSourceStateValid
				|| FStaticMeshOperations::ComputeSHAHash(
					Pending.CurrentMeshDescription,
					true)
					!= Preparation.SourceMeshDescriptionHash
				|| ComputeMeshDescriptionTopologyHash(
					Pending.CurrentMeshDescription)
					!= Preparation.SourceMeshTopologyHash)
			{
				OutError = FText::Format(
					LOCTEXT(
						"SourceMeshChangedDuringBake",
						"{0}: LOD0 or its build state changed during the bake. No changes were committed; run the bake again."),
					FText::FromString(
						Pending.SourceMesh->GetName()));
				return false;
			}

			if (Job.Preparation.Settings.bRegenerateBakeUV)
			{
				if (!ApplyPreparedBakeUV(
					Preparation.MeshDescription,
					Pending.CurrentMeshDescription,
					Job.Preparation.Settings.BakeUVChannel,
					OutError))
				{
					return false;
				}
				FMeshDescription* MutableMeshDescription =
					Pending.StaticMesh != nullptr
						? Pending.StaticMesh
							->GetMeshDescription(0)
						: Pending.SkeletalMesh
							->GetMeshDescription(0);
				if (MutableMeshDescription == nullptr)
				{
					OutError = FText::Format(
						LOCTEXT(
							"MutableMeshDescriptionMissing",
							"{0}: failed to access the mutable LOD0 MeshDescription. No changes were committed."),
						FText::FromString(
							Pending.SourceMesh->GetName()));
					return false;
				}
			}

			if (Job.EncodedRGBA[TargetIndex].Num()
				!= PixelCount * 4)
			{
				OutError = FText::Format(
					LOCTEXT(
						"EncodedTextureSizeMismatch",
						"{0}: the encoded coefficient buffer has an unexpected size. No changes were committed."),
					FText::FromString(
						Pending.SourceMesh->GetName()));
				return false;
			}

			FString SourceBaseName = Pending.SourceMesh->GetName();
			SourceBaseName.RemoveFromStart(TEXT("SM_"));
			const FString TextureOutputDirectory =
				Pending.SourceMesh->GetOutermost()->GetName()
						.StartsWith(TEXT("/Engine/"))
					? TEXT("/Game/TextureBaker")
					: FPackageName::GetLongPackagePath(
						Pending.SourceMesh->GetOutermost()
							->GetName());
			Pending.TextureAssetName = FString::Printf(
				TEXT("T_%s_Thickness_M"),
				*SourceBaseName);
			Pending.TexturePackageName = FString::Printf(
				TEXT("%s/%s"),
				*TextureOutputDirectory,
				*Pending.TextureAssetName);
			const FString TextureObjectPath = FString::Printf(
				TEXT("%s.%s"),
				*Pending.TexturePackageName,
				*Pending.TextureAssetName);
			if (PendingTexturePaths.Contains(TextureObjectPath))
			{
				OutError = FText::Format(
					LOCTEXT(
						"DuplicateTextureOutput",
						"Multiple source meshes resolve to the same output texture {0}. No changes were committed."),
					FText::FromString(TextureObjectPath));
				return false;
			}
			PendingTexturePaths.Add(TextureObjectPath);

			Pending.TexturePackage =
				FindPackage(nullptr, *Pending.TexturePackageName);
			const bool bTexturePackageExists =
				FPackageName::DoesPackageExist(
					Pending.TexturePackageName);
			if (Pending.TexturePackage == nullptr
				&& bTexturePackageExists)
			{
				Pending.TexturePackage = LoadPackage(
					nullptr,
					*Pending.TexturePackageName,
					LOAD_None);
			}
			if (bTexturePackageExists
				&& Pending.TexturePackage == nullptr)
			{
				OutError = FText::Format(
					LOCTEXT(
						"LoadTexturePackageFailed",
						"{0} exists but could not be loaded. No changes were committed."),
					FText::FromString(
						Pending.TexturePackageName));
				return false;
			}
			if (Pending.TexturePackage != nullptr)
			{
				Pending.bTexturePackageWasDirty =
					Pending.TexturePackage->IsDirty();
				UObject* ExistingObject = StaticFindObject(
					UObject::StaticClass(),
					Pending.TexturePackage,
					*Pending.TextureAssetName);
				if (ExistingObject != nullptr)
				{
					Pending.Texture =
						Cast<UTexture2D>(ExistingObject);
					if (Pending.Texture == nullptr)
					{
						OutError = FText::Format(
							LOCTEXT(
								"TextureOutputTypeConflict",
								"{0} already exists but is not a Texture2D. No changes were committed."),
							FText::FromString(
								TextureObjectPath));
						return false;
					}
				}
			}
		}

		FlushRenderingCommands();
		bool bCommitSucceeded = true;
		bool bTransactionChanged = false;
		FText CommitError;
		{
			const FScopedTransaction Transaction(
				LOCTEXT(
					"CommitSHThicknessBakeTransaction",
					"Texture Baker: Commit SH Thickness Bake"));

			for (FPendingCommit& Pending : PendingCommits)
			{
				if (Pending.TexturePackage == nullptr)
				{
					Pending.TexturePackage =
						CreatePackage(*Pending.TexturePackageName);
				}
				if (Pending.TexturePackage == nullptr)
				{
					CommitError = FText::Format(
						LOCTEXT(
							"CreateTexturePackageFailed",
							"{0}: failed to create the SH coefficient texture package."),
						FText::FromString(
							Pending.SourceMesh->GetName()));
					bCommitSucceeded = false;
					break;
				}
				if (Pending.Texture == nullptr)
				{
					Pending.Texture = NewObject<UTexture2D>(
						Pending.TexturePackage,
						*Pending.TextureAssetName,
						RF_Public | RF_Standalone
							| RF_Transactional);
					Pending.bTextureIsNew = true;
				}
				if (Pending.Texture == nullptr)
				{
					CommitError = FText::Format(
						LOCTEXT(
							"CreateTextureAssetFailed",
							"{0}: failed to create the SH coefficient texture asset."),
						FText::FromString(
							Pending.SourceMesh->GetName()));
					bCommitSucceeded = false;
					break;
				}
				Pending.Texture->SetFlags(
					RF_Public | RF_Standalone | RF_Transactional);
				if (!Pending.Texture->Modify())
				{
					CommitError = FText::Format(
						LOCTEXT(
							"ModifyTextureTransactionFailed",
							"{0}: failed to add the output texture to the editor transaction."),
						FText::FromString(
							Pending.TextureAssetName));
					bCommitSucceeded = false;
					break;
				}
				bTransactionChanged = true;
			}

			for (FPendingCommit& Pending : PendingCommits)
			{
				if (!bCommitSucceeded)
				{
					break;
				}

				Pending.Texture->PreEditChange(nullptr);
				Pending.Texture->Source.Init(
					Resolution,
					Resolution,
					1,
					1,
					TSF_BGRA8);
				uint8* SourceBGRA =
					Pending.Texture->Source.LockMip(0);
				if (SourceBGRA == nullptr)
				{
					Pending.Texture->PostEditChange();
					CommitError = FText::Format(
						LOCTEXT(
							"LockTextureSourceFailed",
							"{0}: failed to lock the coefficient texture source mip."),
						FText::FromString(
							Pending.SourceMesh->GetName()));
					bCommitSucceeded = false;
					break;
				}

				const int32 TargetIndex =
					static_cast<int32>(
						&Pending - PendingCommits.GetData());
				const TArray64<uint8>& EncodedRGBA =
					Job.EncodedRGBA[TargetIndex];
				for (int64 PixelIndex = 0;
					PixelIndex < PixelCount;
					++PixelIndex)
				{
					SourceBGRA[PixelIndex * 4 + 0] =
						EncodedRGBA[PixelIndex * 4 + 2];
					SourceBGRA[PixelIndex * 4 + 1] =
						EncodedRGBA[PixelIndex * 4 + 1];
					SourceBGRA[PixelIndex * 4 + 2] =
						EncodedRGBA[PixelIndex * 4 + 0];
					SourceBGRA[PixelIndex * 4 + 3] =
						EncodedRGBA[PixelIndex * 4 + 3];
				}
				Pending.Texture->Source.UnlockMip(0);
				Pending.Texture->SRGB = false;
				Pending.Texture->CompressionSettings = TC_BC7;
				Pending.Texture->CompressionNoAlpha = false;
				Pending.Texture->CompressionForceAlpha = true;
				Pending.Texture->LossyCompressionAmount =
					TLCA_None;
				Pending.Texture->MipGenSettings =
					TMGS_FromTextureGroup;
				Pending.Texture->AddressX = TA_Clamp;
				Pending.Texture->AddressY = TA_Clamp;
				Pending.Texture
					->SetModernSettingsForNewOrChangedTexture();
				Pending.Texture->PostEditChange();
			}

			if (bCommitSucceeded
				&& Job.Preparation.Settings.bRegenerateBakeUV)
			{
				for (FPendingCommit& Pending : PendingCommits)
				{
					Pending.SourceMesh->SetFlags(RF_Transactional);
					if (!Pending.SourceMesh->Modify())
					{
						CommitError = FText::Format(
							LOCTEXT(
								"ModifySourceMeshTransactionFailed",
								"{0}: failed to add the source mesh to the editor transaction."),
							FText::FromString(
								Pending.SourceMesh->GetName()));
						bCommitSucceeded = false;
						break;
					}
					if (Pending.StaticMesh != nullptr)
					{
						Pending.StaticMesh->ModifyMeshDescription(0);
					}
					else if (!Pending.SkeletalMesh
						->ModifyMeshDescription(0))
					{
						CommitError = FText::Format(
							LOCTEXT(
								"ModifySkeletalMeshDescriptionFailed",
								"{0}: failed to add LOD0 source data to the editor transaction."),
							FText::FromString(
								Pending.SourceMesh->GetName()));
						bCommitSucceeded = false;
						break;
					}
				}

				for (FPendingCommit& Pending : PendingCommits)
				{
					if (!bCommitSucceeded)
					{
						break;
					}

					Pending.SourceMesh->PreEditChange(nullptr);
					FMeshDescription* TargetMeshDescription =
						Pending.StaticMesh != nullptr
							? Pending.StaticMesh
								->GetMeshDescription(0)
							: Pending.SkeletalMesh
								->GetMeshDescription(0);
					if (TargetMeshDescription == nullptr)
					{
						Pending.SourceMesh->PostEditChange();
						CommitError = FText::Format(
							LOCTEXT(
								"CommitMeshDescriptionMissing",
								"{0}: LOD0 MeshDescription became unavailable during commit."),
							FText::FromString(
								Pending.SourceMesh->GetName()));
						bCommitSucceeded = false;
						break;
					}
					*TargetMeshDescription =
						MoveTemp(Pending.CurrentMeshDescription);

					if (Pending.Preparation
						->bDisableLightmapGeneration)
					{
						if (Pending.StaticMesh == nullptr)
						{
							Pending.SourceMesh->PostEditChange();
							CommitError = FText::Format(
								LOCTEXT(
									"LightmapDisableRequiresStaticMesh",
									"{0}: lightmap UV generation can only be disabled on a StaticMesh."),
								FText::FromString(
									Pending.SourceMesh->GetName()));
							bCommitSucceeded = false;
							break;
						}
						Pending.StaticMesh->GetSourceModel(0)
							.BuildSettings.bGenerateLightmapUVs =
								false;
					}

					bool bMeshCommitted = true;
					if (Pending.StaticMesh != nullptr)
					{
						Pending.StaticMesh
							->CommitMeshDescription(0);
					}
					else
					{
						bMeshCommitted =
							Pending.SkeletalMesh
								->CommitMeshDescription(0);
					}
					Pending.SourceMesh->PostEditChange();
					if (!bMeshCommitted)
					{
						CommitError = FText::Format(
							LOCTEXT(
								"CommitSkeletalMeshDescriptionFailed",
								"{0}: failed to commit the LOD0 MeshDescription."),
							FText::FromString(
								Pending.SourceMesh->GetName()));
						bCommitSucceeded = false;
						break;
					}
					Pending.SourceMesh->MarkPackageDirty();
				}
			}
		}

		if (!bCommitSucceeded)
		{
			if (!bTransactionChanged)
			{
				OutError = CommitError;
			}
			else if (!GEditor->UndoTransaction(false))
			{
				OutError = FText::Format(
					LOCTEXT(
						"CommitRollbackFailed",
						"{0} Automatic rollback also failed; inspect the affected assets before continuing."),
					CommitError);
			}
			else
			{
				OutError = FText::Format(
					LOCTEXT(
						"CommitRolledBack",
						"{0} All transactional changes were rolled back."),
					CommitError);
			}
			for (FPendingCommit& Pending : PendingCommits)
			{
				if (Pending.bTextureIsNew
					&& IsValid(Pending.Texture))
				{
					Pending.Texture->ClearFlags(
						RF_Public | RF_Standalone);
					Pending.Texture->Rename(
						nullptr,
						GetTransientPackage(),
						REN_DontCreateRedirectors
							| REN_NonTransactional);
					Pending.Texture->MarkAsGarbage();
				}
				if (Pending.TexturePackage != nullptr)
				{
					Pending.TexturePackage->SetDirtyFlag(
						Pending.bTexturePackageWasDirty);
				}
				if (Pending.SourceMesh != nullptr)
				{
					Pending.SourceMesh->GetOutermost()->SetDirtyFlag(
						Pending.bSourcePackageWasDirty);
				}
			}
			return false;
		}

		TArray<UObject*> AssetsToSync;
		AssetsToSync.Reserve(
			PendingCommits.Num()
				* (Job.Preparation.Settings.bRegenerateBakeUV
					? 2
					: 1));
		for (FPendingCommit& Pending : PendingCommits)
		{
			Pending.Texture->SetFlags(
				RF_Public | RF_Standalone | RF_Transactional);
			if (Pending.bTextureIsNew)
			{
				FAssetRegistryModule::AssetCreated(Pending.Texture);
			}
			Pending.Texture->MarkPackageDirty();
			Pending.TexturePackage->MarkPackageDirty();
			if (Job.Preparation.Settings.bRegenerateBakeUV)
			{
				AssetsToSync.Add(Pending.SourceMesh);
			}
			AssetsToSync.Add(Pending.Texture);
		}
		GEditor->SyncBrowserToObjects(AssetsToSync);
		return true;
	}

	FBakeSettings PanelSettings;
	int32 ActiveBakerTypeIndex = SHThicknessBakerIndex;
	TArray<TSharedPtr<FSourceMeshSelection>> SelectedSourceMeshes;
	TSharedPtr<SVerticalBox> SourceMeshListBox;
	TSharedPtr<FBakeJob, ESPMode::ThreadSafe> ActiveJob;
	TArray<TStrongObjectPtr<UObject>> ActiveSourceMeshes;
	TFuture<void> ActiveFuture;
	FTSTicker::FDelegateHandle ActiveTickerHandle;
	TWeakPtr<SNotificationItem> ActiveNotification;
	FDelegateHandle PostEngineInitHandle;
	bool bEditorUIRegistered = false;
};

IMPLEMENT_MODULE(FTextureBakerEditorModule, TextureBakerEditor)

#undef LOCTEXT_NAMESPACE
