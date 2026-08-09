#include "FoliageBakerEditorModule.h"

#include "FoliageBakerBillboardCloudsModule.h"
#include "FoliageBakerBillboardCloudsSettings.h"
#include "FoliageBakerCardsModule.h"
#include "FoliageBakerCardsSettings.h"
#include "FoliageBakerImpostorModule.h"
#include "FoliageBakerImpostorSettings.h"
#include "FoliageBakerFeatureTool.h"
#include "FoliageBakerTreeHierarchyColorBaker.h"
#include "FoliageBakerTreeHierarchyPreview.h"
#include "FoliageBakerTreeHierarchySettings.h"
#include "DetailLayoutBuilder.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "IDetailCustomization.h"
#include "ISettingsModule.h"
#include "Misc/MessageDialog.h"
#include "PropertyEditorModule.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Templates/SubclassOf.h"
#include "ToolMenus.h"
#include "UObject/ObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"

#define LOCTEXT_NAMESPACE "FFoliageBakerEditorModule"

namespace
{
	const FName FoliageBakerToolTabName(TEXT("FoliageBakerTools"));
	constexpr int32 DataBakeWorkflowIndex = 0;
	constexpr int32 ProxyBakeWorkflowIndex = 1;
	constexpr int32 BillboardFeatureIndex = 0;
	const FName EditorSettingsContainerName(TEXT("Editor"));
	const FName PluginsSettingsCategoryName(TEXT("Plugins"));
	const FName SingleBillboardSettingsSectionName(TEXT("FoliageBakerSingleBillboard"));
	const FName CrossCardsSettingsSectionName(TEXT("FoliageBakerCrossCards"));
	const FName ImpostorSettingsSectionName(TEXT("FoliageBakerImpostor"));
	const FName MultiBillboardSettingsSectionName(TEXT("FoliageBakerMultiBillboard"));
	const FName BillboardCloudsSettingsSectionName(TEXT("FoliageBakerBillboardClouds"));

	enum class EFoliageBakerFeatureKind : uint8
	{
		Billboard,
		CrossCards,
		Impostor,
		MultiBillboard,
		BillboardClouds
	};

	struct FFoliageBakerFeatureDescriptor
	{
		EFoliageBakerFeatureKind Kind = EFoliageBakerFeatureKind::Billboard;
		FText TabLabel;
		FText Title;
		FText Description;
		FText Metadata;
	};

	const TArray<FFoliageBakerFeatureDescriptor>& GetFeatureDescriptors()
	{
		static const TArray<FFoliageBakerFeatureDescriptor> Descriptors =
		{
			{
				EFoliageBakerFeatureKind::Billboard,
				LOCTEXT("BillboardTab", "Billboard"),
				LOCTEXT("BillboardFeatureTitle", "Billboard"),
				LOCTEXT(
					"BillboardFeatureDescription",
					"Bake either one camera-facing plane or two parallel camera-facing planes captured from horizontal directions 90 degrees apart."),
				LOCTEXT(
					"BillboardFeatureMetadata",
					"Selectable source LOD  |  Single Plane - One View, Single Plane - Two Views, or Double Planes - Two Views  |  orthographic capture  |  1024 default")
			},
			{
				EFoliageBakerFeatureKind::CrossCards,
				LOCTEXT("CrossCardsTab", "Cross Cards"),
				LOCTEXT("CrossCardsFeatureTitle", "Cross Cards"),
				LOCTEXT(
					"CrossCardsFeatureDescription",
					"Bake 2-5 equally spaced vertical cards. Every angle is cropped independently and captures both front and back."),
				LOCTEXT(
					"CrossCardsFeatureMetadata",
					"Selectable source LOD  |  2-5 planes  |  front + back  |  2048 default")
			},
			{
				EFoliageBakerFeatureKind::Impostor,
				LOCTEXT("ImpostorTab", "Impostor"),
				LOCTEXT("ImpostorFeatureTitle", "Impostor"),
				LOCTEXT(
					"ImpostorFeatureDescription",
					"Bake an octahedrally encoded upper-hemisphere or full-sphere view atlas and a camera-facing cutout proxy."),
				LOCTEXT(
					"ImpostorFeatureMetadata",
					"Selectable source LOD  |  fixed N x N direction grid  |  shared projection and depth range")
			},
			{
				EFoliageBakerFeatureKind::MultiBillboard,
				LOCTEXT("MultiBillboardTab", "MultiBillboard"),
				LOCTEXT("MultiBillboardFeatureTitle", "MultiBillboard"),
				LOCTEXT(
					"MultiBillboardFeatureDescription",
					"Select leaf geometry by material-name keywords, replace each local cluster with camera-facing Billboards, and optionally retain a simplified trunk using the source materials."),
				LOCTEXT(
					"MultiBillboardFeatureMetadata",
					"Selectable source LOD  |  material-based leaf selection  |  optional reduced trunk  |  1-128 local Billboard clusters")
			},
			{
				EFoliageBakerFeatureKind::BillboardClouds,
				LOCTEXT("BillboardCloudsTab", "BillboardClouds"),
				LOCTEXT("BillboardCloudsFeatureTitle", "BillboardClouds"),
				LOCTEXT(
					"BillboardCloudsFeatureDescription",
					"Generate an adaptive cloud of planes using the existing BillboardClouds workflow."),
				LOCTEXT(
					"BillboardCloudsFeatureMetadata",
					"Selectable source LOD  |  adaptive plane cloud  |  existing output workflow")
			}
		};
		return Descriptors;
	}

	const FFoliageBakerFeatureDescriptor& GetFeatureDescriptor(const int32 Index)
	{
		const TArray<FFoliageBakerFeatureDescriptor>& Descriptors = GetFeatureDescriptors();
		return Descriptors[FMath::Clamp(Index, 0, Descriptors.Num() - 1)];
	}

	struct FFoliageBakerEditorPreferenceDescriptor
	{
		FName SectionName;
		FText DisplayName;
		FText Description;
		TSubclassOf<UObject> SettingsClass;
	};

	class FFoliageBakerEditorPreferenceCustomization final : public IDetailCustomization
	{
	public:
		static TSharedRef<IDetailCustomization> MakeInstance()
		{
			return MakeShared<FFoliageBakerEditorPreferenceCustomization>();
		}

		virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override
		{
			DetailBuilder.HideProperty(
				GET_MEMBER_NAME_CHECKED(UFoliageBakerCardsSettings, SourceStaticMeshes));
		}
	};

	const TArray<FFoliageBakerEditorPreferenceDescriptor>& GetEditorPreferenceDescriptors()
	{
		static const TArray<FFoliageBakerEditorPreferenceDescriptor> Descriptors =
		{
			{
				SingleBillboardSettingsSectionName,
				LOCTEXT("BillboardSettingsName", "Foliage Baker - Billboard"),
				LOCTEXT(
					"BillboardSettingsDescription",
					"Configure Single Plane - One View, Single Plane - Two Views, and Double Planes - Two Views preferences, including their default Parent Material Instances."),
				UFoliageBakerSingleBillboardSettings::StaticClass()
			},
			{
				CrossCardsSettingsSectionName,
				LOCTEXT("CrossCardsSettingsName", "Foliage Baker - Cross Cards"),
				LOCTEXT(
					"CrossCardsSettingsDescription",
					"Configure Cross Cards preferences, including its default Parent Material Instance."),
				UFoliageBakerCrossCardsSettings::StaticClass()
			},
			{
				ImpostorSettingsSectionName,
				LOCTEXT("ImpostorSettingsName", "Foliage Baker - Impostor"),
				LOCTEXT(
					"ImpostorSettingsDescription",
					"Configure Impostor preferences, including its default Parent Material Instance."),
				UFoliageBakerImpostorSettings::StaticClass()
			},
			{
				MultiBillboardSettingsSectionName,
				LOCTEXT("MultiBillboardSettingsName", "Foliage Baker - MultiBillboard"),
				LOCTEXT(
					"MultiBillboardSettingsDescription",
					"Configure MultiBillboard preferences, including its default Parent Material Instance."),
				UFoliageBakerMultiBillboardSettings::StaticClass()
			},
			{
				BillboardCloudsSettingsSectionName,
				LOCTEXT("BillboardCloudsSettingsName", "Foliage Baker - Billboard Clouds"),
				LOCTEXT(
					"BillboardCloudsSettingsDescription",
					"Configure Billboard Clouds preferences, including its default Parent Material Instance."),
				UFoliageBakerBillboardCloudsSettings::StaticClass()
			}
		};
		return Descriptors;
	}
}

void FFoliageBakerEditorModule::StartupModule()
{
	RegisterEditorPreferences();

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		FoliageBakerToolTabName,
		FOnSpawnTab::CreateRaw(this, &FFoliageBakerEditorModule::SpawnToolTab))
		.SetDisplayName(LOCTEXT("FoliageBakerToolTabTitle", "Foliage Baker"))
		.SetTooltipText(LOCTEXT("FoliageBakerToolTabTooltip", "Open the foliage Data Bake and Proxy Bake workflows."))
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FFoliageBakerEditorModule::RegisterMenus));
}

void FFoliageBakerEditorModule::ShutdownModule()
{
	UnregisterEditorPreferences();

	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	if (FSlateApplication::IsInitialized())
	{
		if (TSharedPtr<SDockTab> LiveTab = FGlobalTabmanager::Get()->FindExistingLiveTab(FoliageBakerToolTabName))
		{
			LiveTab->RequestCloseTab();
		}
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(FoliageBakerToolTabName);
	}
	WorkflowSwitcher.Reset();
	FeatureSwitcher.Reset();
	DataBakeController.Reset();
	DataBakePreview.Reset();
	DataBakePreviewData.Reset();
	DataBakeBranchOptions.Reset();
	SelectedDataBakeBranchIDs.Reset();
	DataBakeBranchList.Reset();
	DataBakeSettings.Reset();
}

void FFoliageBakerEditorModule::RegisterEditorPreferences()
{
	ISettingsModule& SettingsModule = FModuleManager::LoadModuleChecked<ISettingsModule>(TEXT("Settings"));
	FPropertyEditorModule& PropertyEditorModule =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	RegisteredPreferenceClassNames.Reset();
	for (const FFoliageBakerEditorPreferenceDescriptor& Descriptor :
		GetEditorPreferenceDescriptors())
	{
		check(Descriptor.SettingsClass);
		UObject& SettingsObject =
			*GetMutableDefault<UObject>(Descriptor.SettingsClass.Get());
		const FName SettingsClassName = SettingsObject.GetClass()->GetFName();
		PropertyEditorModule.RegisterCustomClassLayout(
			SettingsClassName,
			FOnGetDetailCustomizationInstance::CreateStatic(
				&FFoliageBakerEditorPreferenceCustomization::MakeInstance));
		RegisteredPreferenceClassNames.Add(SettingsClassName);
		SettingsModule.RegisterSettings(
			EditorSettingsContainerName,
			PluginsSettingsCategoryName,
			Descriptor.SectionName,
			Descriptor.DisplayName,
			Descriptor.Description,
			TWeakObjectPtr<UObject>(&SettingsObject));
	}
	PropertyEditorModule.NotifyCustomizationModuleChanged();
}

void FFoliageBakerEditorModule::UnregisterEditorPreferences()
{
	if (!UObjectInitialized())
	{
		RegisteredPreferenceClassNames.Reset();
		return;
	}

	if (FModuleManager::Get().IsModuleLoaded(TEXT("Settings")))
	{
		ISettingsModule& SettingsModule =
			FModuleManager::GetModuleChecked<ISettingsModule>(
				TEXT("Settings"));
		for (const FFoliageBakerEditorPreferenceDescriptor& Descriptor :
			GetEditorPreferenceDescriptors())
		{
			SettingsModule.UnregisterSettings(
				EditorSettingsContainerName,
				PluginsSettingsCategoryName,
				Descriptor.SectionName);
		}
	}
	if (FModuleManager::Get().IsModuleLoaded(TEXT("PropertyEditor")))
	{
		FPropertyEditorModule& PropertyEditorModule =
			FModuleManager::GetModuleChecked<FPropertyEditorModule>(
				TEXT("PropertyEditor"));
		for (const FName& SettingsClassName : RegisteredPreferenceClassNames)
		{
			PropertyEditorModule.UnregisterCustomClassLayout(SettingsClassName);
		}
		PropertyEditorModule.NotifyCustomizationModuleChanged();
	}
	RegisteredPreferenceClassNames.Reset();
}

void FFoliageBakerEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);
	const TObjectPtr<UToolMenu> ToolsMenu =
		UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
	if (!ToolsMenu)
	{
		return;
	}

	FToolMenuSection& Section = ToolsMenu->FindOrAddSection(TEXT("FoliageBaker"));
	Section.Label = LOCTEXT("FoliageBakerSection", "Foliage Baker");
	Section.AddMenuEntry(
		TEXT("OpenFoliageBaker"),
		LOCTEXT("OpenFoliageBakerLabel", "Foliage Baker"),
		LOCTEXT("OpenFoliageBakerTooltip", "Open the unified Data Bake and Proxy Bake tool."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"),
		FToolMenuExecuteAction::CreateRaw(this, &FFoliageBakerEditorModule::ExecuteOpenTool));
}

TSharedRef<SDockTab> FFoliageBakerEditorModule::SpawnToolTab(const FSpawnTabArgs& SpawnTabArgs)
{
	(void)SpawnTabArgs;
	ActiveWorkflowIndex = DataBakeWorkflowIndex;
	ActiveFeatureIndex = BillboardFeatureIndex;

	FFoliageBakerBillboardCloudsModule& BillboardCloudsModule =
		FModuleManager::LoadModuleChecked<FFoliageBakerBillboardCloudsModule>(TEXT("FoliageBakerBillboardClouds"));
	FFoliageBakerCardsModule& CardsModule =
		FModuleManager::LoadModuleChecked<FFoliageBakerCardsModule>(TEXT("FoliageBakerCards"));
	FFoliageBakerImpostorModule& ImpostorModule =
		FModuleManager::LoadModuleChecked<FFoliageBakerImpostorModule>(TEXT("FoliageBakerImpostor"));

	TSharedRef<SSegmentedControl<int32>> WorkflowTabs =
		SNew(SSegmentedControl<int32>)
		.Value_Lambda([this]() { return GetActiveWorkflowIndex(); })
		.OnValueChanged_Lambda(
			[this](const int32 NewWorkflowIndex)
			{
				HandleWorkflowChanged(NewWorkflowIndex);
			})
		.UniformPadding(FMargin(30.0f, 8.0f))
		+ SSegmentedControl<int32>::Slot(DataBakeWorkflowIndex)
		.Text(LOCTEXT("DataBakeWorkflowTab", "Data Bake"))
		+ SSegmentedControl<int32>::Slot(ProxyBakeWorkflowIndex)
		.Text(LOCTEXT("ProxyBakeWorkflowTab", "Proxy Bake"));

	TSharedRef<SSegmentedControl<int32>> FeatureTabs =
		SNew(SSegmentedControl<int32>)
		.Value_Lambda([this]() { return GetActiveFeatureIndex(); })
		.OnValueChanged_Lambda([this](const int32 NewFeatureIndex) { HandleFeatureChanged(NewFeatureIndex); })
		.UniformPadding(FMargin(22.0f, 7.0f))
		.MaxSegmentsPerLine(GetFeatureDescriptors().Num());

	for (int32 FeatureIndex = 0; FeatureIndex < GetFeatureDescriptors().Num(); ++FeatureIndex)
	{
		FeatureTabs->AddSlot(FeatureIndex)
			.HAlign(HAlign_Center)
			.Text(GetFeatureDescriptors()[FeatureIndex].TabLabel);
	}

	SAssignNew(FeatureSwitcher, SWidgetSwitcher);
	for (const FFoliageBakerFeatureDescriptor& Descriptor : GetFeatureDescriptors())
	{
		TSharedRef<SWidget> FeaturePanel =
			[&CardsModule, &ImpostorModule, &BillboardCloudsModule, &Descriptor]()
			{
				switch (Descriptor.Kind)
				{
				case EFoliageBakerFeatureKind::Billboard:
					return CardsModule.CreateFeaturePanel(
						EFoliageBakerCardMode::SingleBillboard);
				case EFoliageBakerFeatureKind::CrossCards:
					return CardsModule.CreateFeaturePanel(
						EFoliageBakerCardMode::CrossCards);
				case EFoliageBakerFeatureKind::Impostor:
					return ImpostorModule.CreateFeaturePanel();
				case EFoliageBakerFeatureKind::MultiBillboard:
					return CardsModule.CreateFeaturePanel(
						EFoliageBakerCardMode::MultiBillboard);
				case EFoliageBakerFeatureKind::BillboardClouds:
					return BillboardCloudsModule.CreateFeaturePanel();
				default:
					checkNoEntry();
					return SNullWidget::NullWidget;
				}
			}();
		FeatureSwitcher->AddSlot()
		[
			FeaturePanel
		];
	}

	const TSharedRef<SWidget> DataBakePanel = CreateDataBakePanel();

	TSharedRef<SWidget> ProxyBakePanel =
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(10.0f, 0.0f, 10.0f, 6.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(14.0f, 10.0f))
			[
				FeatureTabs
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(10.0f, 0.0f, 10.0f, 6.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(14.0f, 10.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return GetActiveFeatureTitle(); })
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 3.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return GetActiveFeatureDescription(); })
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 5.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return GetActiveFeatureMetadata(); })
					.TextStyle(FAppStyle::Get(), "SmallText")
				]
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(10.0f, 0.0f, 10.0f, 10.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(1.0f)
			[
				FeatureSwitcher.ToSharedRef()
			]
		];

	SAssignNew(WorkflowSwitcher, SWidgetSwitcher)
		+ SWidgetSwitcher::Slot()
		[
			DataBakePanel
		]
		+ SWidgetSwitcher::Slot()
		[
			ProxyBakePanel
		];

	TSharedRef<SDockTab> ToolTab = SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(10.0f, 10.0f, 10.0f, 6.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(FMargin(14.0f, 12.0f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SBox)
							.WidthOverride(32.0f)
							.HeightOverride(32.0f)
							[
								SNew(SImage)
								.Image(FAppStyle::GetBrush("LevelEditor.Tabs.Details"))
							]
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.Padding(12.0f, 0.0f, 0.0f, 0.0f)
						.VAlign(VAlign_Center)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot()
							.AutoHeight()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("FoliageBakerHeaderTitle", "Foliage Baker"))
								.TextStyle(FAppStyle::Get(), "LargeText")
							]
							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0.0f, 2.0f, 0.0f, 0.0f)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("FoliageBakerHeaderSubtitle", "Choose between foliage data processing and proxy generation workflows."))
							]
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 14.0f, 0.0f, 0.0f)
					[
						WorkflowTabs
					]
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				WorkflowSwitcher.ToSharedRef()
			]
		];

	FeatureSwitcher->SetActiveWidgetIndex(ActiveFeatureIndex);
	WorkflowSwitcher->SetActiveWidgetIndex(ActiveWorkflowIndex);
	return ToolTab;
}

TSharedRef<SWidget> FFoliageBakerEditorModule::CreateDataBakePanel()
{
	EnsureDataBakeSettings();
	DataBakeSettings->SourceStaticMeshes.Reset();
	DataBakePreviewData.Reset();
	DataBakeBranchOptions.Reset();
	SelectedDataBakeBranchIDs.Reset();
	DataBakeBranchList.Reset();

	FFoliageBakerFeatureControllerArgs ControllerArgs;
	ControllerArgs.SettingsObject.Reset(DataBakeSettings.Get());
	ControllerArgs.GetSourceStaticMeshes =
		[Settings = DataBakeSettings]() -> TArray<TObjectPtr<UStaticMesh>>&
		{
			return Settings->SourceStaticMeshes;
		};
	ControllerArgs.BakeButtonText = LOCTEXT(
		"BakeTreeHierarchyColorsButton",
		"Write Test Vertex Colors");
	ControllerArgs.BakeButtonTooltip = LOCTEXT(
		"BakeTreeHierarchyColorsTooltip",
		"Recognize the three-level tree hierarchy, overwrite the selected source LOD vertex colors, and refresh the Hierarchy View.");
	ControllerArgs.RequirementsHint = LOCTEXT(
		"TreeHierarchyColorsRequirements",
		"Queue at least one Static Mesh. The selected source LOD must contain separate card-foliage and wood materials.");
	ControllerArgs.AddMeshesTransactionText = LOCTEXT(
		"AddTreeHierarchySourceMeshesTransaction",
		"Add Tree Hierarchy Test Source Meshes");
	ControllerArgs.ClearMeshesTransactionText = LOCTEXT(
		"ClearTreeHierarchySourceMeshesTransaction",
		"Clear Tree Hierarchy Test Source Meshes");
	ControllerArgs.CanBake =
		FFoliageBakerFeaturePredicateDelegate::CreateLambda(
			[this]()
			{
				return CanBakeTreeHierarchyColors();
			});
	ControllerArgs.Bake =
		FFoliageBakerFeatureActionDelegate::CreateLambda(
			[this]()
			{
				BakeTreeHierarchyColors();
			});
	DataBakeController = FFoliageBakerFeatureController::Create(ControllerArgs);
	SAssignNew(DataBakePreview, SFoliageBakerTreeHierarchyPreview);
	const TWeakPtr<SFoliageBakerTreeHierarchyPreview> WeakDataBakePreview =
		DataBakePreview;

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(10.0f, 0.0f, 10.0f, 6.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(14.0f, 10.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT(
						"TreeHierarchyColorsTitle",
						"Tree Hierarchy Test Colors"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 3.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT(
						"TreeHierarchyColorsDescription",
						"Recognize one trunk, first-level branch chains, and card foliage, then write validation colors and draw the collapsed hierarchy over the source mesh."))
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 5.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT(
						"TreeHierarchyColorsMetadata",
						"Trunk: white  |  Card foliage: black  |  Each Branch ID: one stable pseudo-random color"))
					.TextStyle(FAppStyle::Get(), "SmallText")
				]
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(10.0f, 0.0f, 10.0f, 10.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(1.0f)
			[
				SNew(SSplitter)
				.Orientation(Orient_Horizontal)
				.PhysicalSplitterHandleSize(3.0f)
				+ SSplitter::Slot()
				.Value(0.45f)
				.MinSize(280.0f)
				[
					DataBakeController->GetWidget()
				]
				+ SSplitter::Slot()
				.Value(0.55f)
				.MinSize(320.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(8.0f, 6.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT(
								"TreeHierarchyPreviewTitle",
								"Hierarchy View"))
							.Font(FAppStyle::GetFontStyle("NormalFontBold"))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(4.0f, 0.0f, 0.0f, 0.0f)
						.VAlign(VAlign_Center)
						[
							SNew(SButton)
							.Text(LOCTEXT(
								"TreeHierarchySetAsTrunk",
								"Set as Trunk"))
							.ToolTipText(LOCTEXT(
								"TreeHierarchySetAsTrunkTooltip",
								"Write all selected Branch IDs and their associated cap geometry as trunk-white vertex colors."))
							.IsEnabled_Lambda(
								[this]()
								{
									return DataBakePreviewData.IsValid()
										&& !SelectedDataBakeBranchIDs.IsEmpty();
								})
							.OnClicked_Lambda(
								[this]()
								{
									MarkSelectedHierarchyBranchAsTrunk();
									return FReply::Handled();
								})
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(8.0f, 0.0f, 0.0f, 0.0f)
						.VAlign(VAlign_Center)
						[
							SNew(SCheckBox)
							.IsChecked(ECheckBoxState::Checked)
							.ToolTipText(LOCTEXT(
								"TreeHierarchyShowDebugTooltip",
								"Show trunk and branch cylinders, joints, and labels in the Hierarchy View."))
							.OnCheckStateChanged_Lambda(
								[WeakDataBakePreview](const ECheckBoxState NewState)
								{
									if (const TSharedPtr<SFoliageBakerTreeHierarchyPreview> Preview =
										WeakDataBakePreview.Pin())
									{
										Preview->SetDebugDrawingEnabled(
											NewState == ECheckBoxState::Checked);
									}
								})
							[
								SNew(STextBlock)
								.Text(LOCTEXT(
									"TreeHierarchyShowDebug",
									"Show Debug"))
							]
						]
					]
					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(6.0f, 0.0f, 4.0f, 6.0f)
						[
							SNew(SBox)
							.WidthOverride(140.0f)
							[
								SNew(SVerticalBox)
								+ SVerticalBox::Slot()
								.AutoHeight()
								.Padding(2.0f, 2.0f, 2.0f, 4.0f)
								[
									SNew(STextBlock)
									.Text(LOCTEXT(
										"TreeHierarchyBranchListTitle",
										"Branches (Ctrl/Shift to multi-select)"))
									.Font(FAppStyle::GetFontStyle("NormalFontBold"))
								]
								+ SVerticalBox::Slot()
								.FillHeight(1.0f)
								[
									SNew(SBorder)
									.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
									[
										SAssignNew(
											DataBakeBranchList,
											SListView<TSharedPtr<int32>>)
										.ListItemsSource(&DataBakeBranchOptions)
										.SelectionMode(ESelectionMode::Multi)
										.OnGenerateRow_Lambda(
											[](
												const TSharedPtr<int32> BranchID,
												const TSharedRef<STableViewBase>& OwnerTable)
											{
												return SNew(
													STableRow<TSharedPtr<int32>>,
													OwnerTable)
												[
													SNew(STextBlock)
													.Text(BranchID.IsValid()
														? FText::Format(
															LOCTEXT(
																"TreeHierarchyBranchOption",
																"Branch ID {0}"),
															FText::AsNumber(*BranchID))
														: FText::GetEmpty())
												];
											})
										.OnSelectionChanged_Lambda(
											[this](
												const TSharedPtr<int32> BranchID,
												const ESelectInfo::Type SelectionType)
											{
												(void)BranchID;
												(void)SelectionType;
												RefreshDataBakeBranchSelection();
											})
									]
							]
						]
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						[
							DataBakePreview.ToSharedRef()
						]
					]
				]
			]
		];
}

void FFoliageBakerEditorModule::EnsureDataBakeSettings()
{
	FFoliageBakerFeatureTool::EnsureTransientSettings(
		DataBakeSettings,
		FName(TEXT("FoliageBakerTreeHierarchySettings")));
}

bool FFoliageBakerEditorModule::CanBakeTreeHierarchyColors() const
{
	return DataBakeSettings.IsValid()
		&& FFoliageBakerFeatureTool::HasAnyValidStaticMesh(
			DataBakeSettings->SourceStaticMeshes);
}

void FFoliageBakerEditorModule::BakeTreeHierarchyColors()
{
	EnsureDataBakeSettings();
	DataBakePreviewData.Reset();
	RefreshDataBakeBranchOptions();
	if (DataBakePreview.IsValid())
	{
		DataBakePreview->ClearPreview();
	}
	const TWeakPtr<SFoliageBakerTreeHierarchyPreview> WeakPreview =
		DataBakePreview;
	const FScopedTransaction Transaction(LOCTEXT(
		"BakeTreeHierarchyColorsTransaction",
		"Write Tree Hierarchy Test Vertex Colors"));
	const FFoliageBakerFeatureBatchResult BatchResult =
		FFoliageBakerFeatureTool::RunBakeBatch(
			DataBakeSettings->SourceStaticMeshes,
			LOCTEXT(
				"BakeTreeHierarchyColorsSlowTask",
				"Writing tree hierarchy test vertex colors..."),
			true,
			TEXT("\n"),
			FFoliageBakerBakeStaticMeshDelegate::CreateLambda(
				[this, Settings = DataBakeSettings, WeakPreview](
					UStaticMesh& StaticMesh)
				{
					const FFoliageBakerTreeHierarchyColorBakeResult Result =
						FFoliageBakerTreeHierarchyColorBaker::Bake(
							StaticMesh,
							Settings->SourceLODIndex);
					if (Result.bSucceeded && Result.PreviewData.IsValid())
					{
						DataBakePreviewData = Result.PreviewData;
						RefreshDataBakeBranchOptions();
						if (const TSharedPtr<SFoliageBakerTreeHierarchyPreview> Preview =
							WeakPreview.Pin())
						{
							Preview->SetPreviewData(Result.PreviewData);
						}
					}
					return FFoliageBakerFeatureTool::MakeBakeItemResult(Result);
				}));

	FFoliageBakerFeatureTool::SyncCreatedAssetsToContentBrowser(
		BatchResult.CreatedAssets);
	FFoliageBakerFeatureTool::ShowBatchSummary(
		BatchResult,
		LOCTEXT(
			"BakeTreeHierarchyColorsSummary",
			"Foliage Baker wrote hierarchy test colors to {0} of {1} Static Mesh asset(s).\n\n{2}"));
}

void FFoliageBakerEditorModule::RefreshDataBakeBranchOptions()
{
	DataBakeBranchOptions.Reset();
	SelectedDataBakeBranchIDs.Reset();
	if (DataBakePreviewData.IsValid())
	{
		for (const FFoliageBakerTreeHierarchyPreviewBranch& Branch :
			DataBakePreviewData->Branches)
		{
			if (Branch.BranchID != INDEX_NONE)
			{
				DataBakeBranchOptions.Add(MakeShared<int32>(Branch.BranchID));
			}
		}
		DataBakeBranchOptions.Sort(
			[](const TSharedPtr<int32>& First, const TSharedPtr<int32>& Second)
			{
				return *First < *Second;
			});
	}
	if (DataBakeBranchList.IsValid())
	{
		DataBakeBranchList->ClearSelection();
		DataBakeBranchList->RequestListRefresh();
	}
	if (DataBakePreview.IsValid())
	{
		DataBakePreview->SetHighlightedBranchIDs(
			SelectedDataBakeBranchIDs);
	}
}

void FFoliageBakerEditorModule::RefreshDataBakeBranchSelection()
{
	SelectedDataBakeBranchIDs.Reset();
	if (DataBakeBranchList.IsValid())
	{
		for (const TSharedPtr<int32>& BranchID :
			DataBakeBranchList->GetSelectedItems())
		{
			if (BranchID.IsValid())
			{
				SelectedDataBakeBranchIDs.Add(*BranchID);
			}
		}
	}
	if (DataBakePreview.IsValid())
	{
		DataBakePreview->SetHighlightedBranchIDs(
			SelectedDataBakeBranchIDs);
	}
}

void FFoliageBakerEditorModule::MarkSelectedHierarchyBranchAsTrunk()
{
	if (!DataBakePreviewData.IsValid()
		|| SelectedDataBakeBranchIDs.IsEmpty()
		|| !DataBakePreviewData->SourceStaticMesh.IsValid())
	{
		return;
	}

	TArray<int32> SelectedBranchIndices;
	TArray<int32> SelectedSourceTriangleIDs;
	int32 TrunkBranchIndex = INDEX_NONE;
	for (int32 BranchIndex = 0;
		BranchIndex < DataBakePreviewData->Branches.Num();
		++BranchIndex)
	{
		const int32 BranchID =
			DataBakePreviewData->Branches[BranchIndex].BranchID;
		if (BranchID == INDEX_NONE)
		{
			TrunkBranchIndex = BranchIndex;
		}
		else if (SelectedDataBakeBranchIDs.Contains(BranchID))
		{
			SelectedBranchIndices.Add(BranchIndex);
			SelectedSourceTriangleIDs.Append(
				DataBakePreviewData->Branches[BranchIndex].SourceTriangleIDs);
		}
	}
	if (SelectedBranchIndices.IsEmpty()
		|| !DataBakePreviewData->Branches.IsValidIndex(TrunkBranchIndex))
	{
		return;
	}

	UStaticMesh& StaticMesh =
		*DataBakePreviewData->SourceStaticMesh.Get();
	const FScopedTransaction Transaction(LOCTEXT(
		"MarkTreeHierarchyBranchesAsTrunkTransaction",
		"Mark Tree Hierarchy Branches as Trunk"));
	FString Error;
	if (!FFoliageBakerTreeHierarchyColorBaker::MarkBranchAsTrunk(
			StaticMesh,
			DataBakePreviewData->SourceLODIndex,
			SelectedSourceTriangleIDs,
			Error))
	{
		FMessageDialog::Open(
			EAppMsgType::Ok,
			FText::Format(
				LOCTEXT(
					"MarkTreeHierarchyBranchesAsTrunkFailed",
					"Could not mark the selected branches as trunk.\n\n{0}"),
				FText::FromString(Error)));
		return;
	}

	FFoliageBakerTreeHierarchyPreviewBranch& TrunkBranch =
		DataBakePreviewData->Branches[TrunkBranchIndex];
	for (const int32 SelectedBranchIndex : SelectedBranchIndices)
	{
		const FFoliageBakerTreeHierarchyPreviewBranch& SelectedBranch =
			DataBakePreviewData->Branches[SelectedBranchIndex];
		TrunkBranch.Cylinders.Append(SelectedBranch.Cylinders);
		TrunkBranch.Joints.Append(SelectedBranch.Joints);
		TrunkBranch.SourceTriangleIDs.Append(SelectedBranch.SourceTriangleIDs);
	}
	SelectedBranchIndices.Sort(
		[](const int32 First, const int32 Second)
		{
			return First > Second;
		});
	for (const int32 SelectedBranchIndex : SelectedBranchIndices)
	{
		DataBakePreviewData->Branches.RemoveAt(SelectedBranchIndex);
	}
	for (FFoliageBakerTreeHierarchyPreviewLeafCluster& LeafCluster :
		DataBakePreviewData->LeafClusters)
	{
		if (SelectedDataBakeBranchIDs.Contains(LeafCluster.ParentBranchID))
		{
			LeafCluster.ParentBranchID = INDEX_NONE;
		}
	}
	RefreshDataBakeBranchOptions();
	if (DataBakePreview.IsValid())
	{
		DataBakePreview->SetPreviewData(DataBakePreviewData, false);
	}
}

FText FFoliageBakerEditorModule::GetActiveFeatureTitle() const
{
	return GetFeatureDescriptor(ActiveFeatureIndex).Title;
}

FText FFoliageBakerEditorModule::GetActiveFeatureDescription() const
{
	return GetFeatureDescriptor(ActiveFeatureIndex).Description;
}

FText FFoliageBakerEditorModule::GetActiveFeatureMetadata() const
{
	return GetFeatureDescriptor(ActiveFeatureIndex).Metadata;
}

int32 FFoliageBakerEditorModule::GetActiveFeatureIndex() const
{
	return ActiveFeatureIndex;
}

int32 FFoliageBakerEditorModule::GetActiveWorkflowIndex() const
{
	return ActiveWorkflowIndex;
}

void FFoliageBakerEditorModule::HandleWorkflowChanged(
	const int32 NewWorkflowIndex)
{
	ActiveWorkflowIndex = FMath::Clamp(
		NewWorkflowIndex,
		DataBakeWorkflowIndex,
		ProxyBakeWorkflowIndex);
	if (WorkflowSwitcher.IsValid())
	{
		WorkflowSwitcher->SetActiveWidgetIndex(
			ActiveWorkflowIndex);
	}
}

void FFoliageBakerEditorModule::HandleFeatureChanged(const int32 NewFeatureIndex)
{
	ActiveFeatureIndex = FMath::Clamp(
		NewFeatureIndex,
		0,
		GetFeatureDescriptors().Num() - 1);
	if (FeatureSwitcher.IsValid())
	{
		FeatureSwitcher->SetActiveWidgetIndex(ActiveFeatureIndex);
	}
}

void FFoliageBakerEditorModule::ExecuteOpenTool(const FToolMenuContext& MenuContext)
{
	(void)MenuContext;
	FGlobalTabmanager::Get()->TryInvokeTab(FoliageBakerToolTabName);
}

IMPLEMENT_MODULE(FFoliageBakerEditorModule, FoliageBakerEditor)

#undef LOCTEXT_NAMESPACE
