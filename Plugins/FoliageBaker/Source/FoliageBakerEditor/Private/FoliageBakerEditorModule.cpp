#include "FoliageBakerEditorModule.h"

#include "FoliageBakerBillboardCloudsModule.h"
#include "FoliageBakerBillboardCloudsSettings.h"
#include "FoliageBakerCardsModule.h"
#include "FoliageBakerCardsSettings.h"
#include "FoliageBakerImpostorModule.h"
#include "FoliageBakerImpostorSettings.h"
#include "FoliageBakerFeatureTool.h"
#include "FoliageBakerLeafUVPreview.h"
#include "FoliageBakerTreeHierarchyColorBaker.h"
#include "FoliageBakerTreeHierarchyPreview.h"
#include "FoliageBakerTreeHierarchySettings.h"
#include "AssetRegistry/AssetData.h"
#include "DetailLayoutBuilder.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/IConsoleManager.h"
#include "IDetailCustomization.h"
#include "ISettingsModule.h"
#include "PropertyCustomizationHelpers.h"
#include "PropertyEditorModule.h"
#include "Styling/AppStyle.h"
#include "Templates/SubclassOf.h"
#include "ToolMenus.h"
#include "UObject/ObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SWindow.h"
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
	FEditorDelegates::OnEditorPreExit.AddRaw(
		this,
		&FFoliageBakerEditorModule::HandleEditorPreExit);

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		FoliageBakerToolTabName,
		FOnSpawnTab::CreateRaw(this, &FFoliageBakerEditorModule::SpawnToolTab))
		.SetDisplayName(LOCTEXT("FoliageBakerToolTabTitle", "Foliage Baker"))
		.SetTooltipText(LOCTEXT("FoliageBakerToolTabTooltip", "Open the foliage Data Bake and Proxy Bake workflows."))
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FFoliageBakerEditorModule::RegisterMenus));
	DebugPreviewCommand = MakeUnique<FAutoConsoleCommand>(
		TEXT("FoliageBaker.DebugPreview"),
		TEXT("Open one Static Mesh object path in the hierarchy tool. Select the Leaf Material Section, then run Analyze Hierarchy."),
		FConsoleCommandWithArgsDelegate::CreateLambda(
			[this](const TArray<FString>& Arguments)
			{
				if (Arguments.IsEmpty())
				{
					return;
				}
				const TObjectPtr<UStaticMesh> StaticMesh = LoadObject<UStaticMesh>(nullptr, *Arguments[0]);
				if (!StaticMesh)
				{
					return;
				}
				FGlobalTabmanager::Get()->TryInvokeTab(FoliageBakerToolTabName);
				EnsureDataBakeSettings();
				DataBakeSettings->SourceStaticMesh = StaticMesh;
				RefreshDataBakeSourceInput();
			}));
}

void FFoliageBakerEditorModule::ShutdownModule()
{
	FEditorDelegates::OnEditorPreExit.RemoveAll(this);
	UnregisterEditorPreferences();
	DebugPreviewCommand.Reset();

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
	ReleaseToolResources();
}

void FFoliageBakerEditorModule::HandleEditorPreExit()
{
	ReleaseToolResources();
}

void FFoliageBakerEditorModule::ReleaseToolResources()
{
	WorkflowSwitcher.Reset();
	FeatureSwitcher.Reset();
	DataBakePreview.Reset();
	DataBakeLeafUVPreview.Reset();
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
	DataBakeSettings->SourceStaticMesh = nullptr;
	DataBakePreviewData.Reset();
	DataBakeBranchOptions.Reset();
	SelectedDataBakeBranchIDs.Reset();
	DataBakeBranchList.Reset();

	SAssignNew(DataBakePreview, SFoliageBakerTreeHierarchyPreview);
	const TWeakPtr<SFoliageBakerTreeHierarchyPreview> WeakDataBakePreview =
		DataBakePreview;
	SAssignNew(DataBakeLeafUVPreview, SFoliageBakerLeafUVPreview)
		.OnResolvedLeavesChanged_Lambda(
			[WeakDataBakePreview](
				const TArray<FFoliageBakerResolvedLeafCluster>& ResolvedLeaves)
			{
				if (const TSharedPtr<SFoliageBakerTreeHierarchyPreview> Preview =
					WeakDataBakePreview.Pin())
				{
					Preview->SetResolvedLeaves(ResolvedLeaves);
				}
			})
		.OnLeafMaterialChanged_Lambda(
			[this](const int32 LeafMaterialIndex)
			{
				HandleDataBakeLeafMaterialChanged(LeafMaterialIndex);
			});

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(10.0f, 0.0f, 10.0f, 6.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(12.0f, 10.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT(
							"TreeHierarchyDataBakeTitle",
							"Tree Data Bake"))
						.Font(FAppStyle::GetFontStyle("NormalFontBold"))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT(
							"TreeHierarchySingleMeshMode",
							"Single Static Mesh workflow"))
						.TextStyle(FAppStyle::Get(), "SmallText")
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(SWrapBox)
					.UseAllottedSize(true)
					.InnerSlotPadding(FVector2D(10.0f, 6.0f))
					+ SWrapBox::Slot()
					.FillEmptySpace(true)
					.FillLineWhenSizeLessThan(760.0f)
					.VAlign(VAlign_Center)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT(
								"TreeHierarchySourceMeshLabel",
								"Static Mesh"))
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						[
							SNew(SObjectPropertyEntryBox)
							.AllowedClass(UStaticMesh::StaticClass())
							.AllowClear(true)
							.DisplayUseSelected(true)
							.DisplayBrowse(true)
							.DisplayCompactSize(true)
							.DisplayThumbnail(false)
							.ObjectPath_Lambda(
								[this]()
								{
									return DataBakeSettings.IsValid()
										&& DataBakeSettings->SourceStaticMesh
										? DataBakeSettings->SourceStaticMesh->GetPathName()
										: FString();
								})
							.OnObjectChanged_Lambda(
								[this](const FAssetData& AssetData)
								{
									EnsureDataBakeSettings();
									const TObjectPtr<UStaticMesh> SourceStaticMesh =
										Cast<UStaticMesh>(AssetData.GetAsset());
									DataBakeSettings->SourceStaticMesh = SourceStaticMesh;
									RefreshDataBakeSourceInput();
								})
						]
					]
					+ SWrapBox::Slot()
					.VAlign(VAlign_Center)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 6.0f, 0.0f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT(
								"TreeHierarchySourceLODLabel",
								"LOD"))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SBox)
							.WidthOverride(72.0f)
							[
								SNew(SNumericEntryBox<int32>)
								.AllowSpin(true)
								.MinValue(0)
								.MaxValue(7)
								.MinSliderValue(0)
								.MaxSliderValue(7)
								.Value_Lambda(
									[this]() -> TOptional<int32>
									{
										return DataBakeSettings.IsValid()
											? TOptional<int32>(DataBakeSettings->SourceLODIndex)
											: TOptional<int32>();
									})
								.OnValueCommitted_Lambda(
									[this](
										const int32 SourceLODIndex,
										const ETextCommit::Type CommitType)
									{
										(void)CommitType;
										EnsureDataBakeSettings();
										DataBakeSettings->SourceLODIndex = FMath::Clamp(
											SourceLODIndex,
											0,
											7);
										RefreshDataBakeSourceInput();
									})
							]
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(SWrapBox)
					.UseAllottedSize(true)
					.InnerSlotPadding(FVector2D(8.0f, 6.0f))
					+ SWrapBox::Slot()
					[
						SNew(SBox)
						.MinDesiredWidth(180.0f)
						[
							SNew(SButton)
							.HAlign(HAlign_Center)
							.Text(LOCTEXT(
								"AnalyzeTreeHierarchyButton",
								"1  Analyze Hierarchy"))
							.ToolTipText(LOCTEXT(
								"AnalyzeTreeHierarchyTooltip",
								"Analyze trunk and branch geometry after excluding the selected Leaf Material Section. The Static Mesh asset is not modified."))
							.IsEnabled_Lambda(
								[this]()
								{
									return CanAnalyzeTreeHierarchy();
								})
							.OnClicked_Lambda(
								[this]()
								{
									AnalyzeTreeHierarchy();
									return FReply::Handled();
								})
						]
					]
					+ SWrapBox::Slot()
					[
						SNew(SBox)
						.MinDesiredWidth(180.0f)
						[
							SNew(SButton)
							.HAlign(HAlign_Center)
							.Text(LOCTEXT(
								"ResolveDataBakeLeafOwnershipButton",
								"2  Resolve Leaves"))
							.ToolTipText(LOCTEXT(
								"ResolveDataBakeLeafOwnershipTooltip",
								"Resolve complete UV0 Pivot/Tip templates onto physical leaf geometry and assign each leaf record to its nearest analyzed trunk or branch surface."))
							.IsEnabled_Lambda(
								[this]()
								{
									return CanResolveDataBakeLeafOwnership();
								})
							.OnClicked_Lambda(
								[this]()
								{
									ResolveDataBakeLeafOwnership();
									return FReply::Handled();
								})
						]
					]
					+ SWrapBox::Slot()
					[
						SNew(SBox)
						.MinDesiredWidth(180.0f)
						[
							SNew(SButton)
							.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
							.HAlign(HAlign_Center)
							.Text(LOCTEXT(
								"BakeWindDataButton",
								"3  Bake Wind Data"))
							.ToolTipText(LOCTEXT(
								"BakeWindDataTooltip",
								"Write bone texel centers to UV1 and create the linked PivPos/PivAxis data textures beside the selected Static Mesh."))
							.IsEnabled_Lambda(
								[this]()
								{
									return CanBakeWindData();
								})
							.OnClicked_Lambda(
								[this]()
								{
									BakeWindData();
									return FReply::Handled();
								})
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT(
						"TreeHierarchyDataBakeWorkflowHint",
						"Choose Leaf Material before Analyze. Mark UV0 Pivot/Tip templates, Resolve Leaves, then Bake Wind Data. Bake writes UV1 plus PivPos/PivAxis assets."))
					.AutoWrapText(true)
					.TextStyle(FAppStyle::Get(), "SmallText")
				]
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(10.0f, 0.0f, 10.0f, 10.0f)
		[
			SNew(SSplitter)
			.Orientation(Orient_Horizontal)
			.PhysicalSplitterHandleSize(4.0f)
			+ SSplitter::Slot()
			.Value(0.55f)
			.MinSize(420.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(1.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(10.0f, 8.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT(
								"TreeHierarchyPreviewTitle",
								"Hierarchy"))
							.Font(FAppStyle::GetFontStyle("NormalFontBold"))
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.Padding(8.0f, 0.0f, 0.0f, 0.0f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT(
								"TreeHierarchyPreviewSubtitle",
								"Trunk, branches, and resolved leaf axes"))
							.AutoWrapText(true)
							.TextStyle(FAppStyle::Get(), "SmallText")
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
								"Show trunk, branch, and selected leaf-axis debug drawing."))
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
									"Debug"))
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
							.WidthOverride(150.0f)
							[
								SNew(SVerticalBox)
								+ SVerticalBox::Slot()
								.AutoHeight()
								.Padding(2.0f, 2.0f, 2.0f, 4.0f)
								[
									SNew(SVerticalBox)
									+ SVerticalBox::Slot()
									.AutoHeight()
									[
										SNew(STextBlock)
										.Text(LOCTEXT(
											"TreeHierarchyBranchListTitle",
											"Branches"))
										.Font(FAppStyle::GetFontStyle("NormalFontBold"))
									]
									+ SVerticalBox::Slot()
									.AutoHeight()
									[
										SNew(STextBlock)
										.Text(LOCTEXT(
											"TreeHierarchyBranchListHint",
											"Ctrl / Shift: multi-select"))
										.TextStyle(FAppStyle::Get(), "SmallText")
									]
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
			+ SSplitter::Slot()
			.Value(0.45f)
			.MinSize(420.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(1.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(10.0f, 8.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT(
								"LeafUVPreviewTitle",
								"Leaf UV & Ownership"))
							.Font(FAppStyle::GetFontStyle("NormalFontBold"))
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.Padding(8.0f, 0.0f, 0.0f, 0.0f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT(
								"LeafUVPreviewSubtitle",
								"UV0 templates  |  Pivot / Tip  |  Parent Branch"))
							.AutoWrapText(true)
							.TextStyle(FAppStyle::Get(), "SmallText")
						]
					]
					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					[
						DataBakeLeafUVPreview.ToSharedRef()
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

bool FFoliageBakerEditorModule::CanAnalyzeTreeHierarchy() const
{
	return DataBakeSettings.IsValid()
		&& DataBakeLeafUVPreview.IsValid()
		&& DataBakeLeafUVPreview->GetSelectedMaterialIndex() != INDEX_NONE
		&& DataBakeSettings->SourceStaticMesh != nullptr;
}

void FFoliageBakerEditorModule::AnalyzeTreeHierarchy()
{
	EnsureDataBakeSettings();
	const int32 LeafMaterialIndex = DataBakeLeafUVPreview.IsValid()
		? DataBakeLeafUVPreview->GetSelectedMaterialIndex()
		: INDEX_NONE;
	DataBakePreviewData.Reset();
	RefreshDataBakeBranchOptions();
	if (DataBakePreview.IsValid())
	{
		DataBakePreview->ClearPreview();
	}
	if (DataBakeLeafUVPreview.IsValid())
	{
		DataBakeLeafUVPreview->SetPreviewData(nullptr);
	}
	const TWeakPtr<SFoliageBakerTreeHierarchyPreview> WeakPreview =
		DataBakePreview;
	const TWeakPtr<SFoliageBakerLeafUVPreview> WeakLeafUVPreview =
		DataBakeLeafUVPreview;
	TArray<TObjectPtr<UStaticMesh>> SourceStaticMeshes;
	if (DataBakeSettings->SourceStaticMesh)
	{
		SourceStaticMeshes.Add(DataBakeSettings->SourceStaticMesh);
	}
	const FString PreviewSourcePath = DataBakeSettings->SourceStaticMesh
		? DataBakeSettings->SourceStaticMesh->GetPathName()
		: FString();
	const FFoliageBakerFeatureBatchResult BatchResult =
		FFoliageBakerFeatureTool::RunBakeBatch(
			SourceStaticMeshes,
			LOCTEXT(
				"AnalyzeTreeHierarchySlowTask",
				"Analyzing trunk and branch hierarchy..."),
			true,
			TEXT("\n"),
			FFoliageBakerBakeStaticMeshDelegate::CreateLambda(
				[this,
					Settings = DataBakeSettings,
					WeakPreview,
					WeakLeafUVPreview,
					LeafMaterialIndex,
					PreviewSourcePath](
					UStaticMesh& StaticMesh)
				{
					const FFoliageBakerTreeHierarchyAnalysisResult Result =
						FFoliageBakerTreeHierarchyColorBaker::Analyze(
							StaticMesh,
							Settings->SourceLODIndex,
							LeafMaterialIndex);
					if (Result.bSucceeded
						&& Result.PreviewData.IsValid()
						&& StaticMesh.GetPathName() == PreviewSourcePath)
					{
						DataBakePreviewData = Result.PreviewData;
						RefreshDataBakeBranchOptions();
						if (const TSharedPtr<SFoliageBakerTreeHierarchyPreview> Preview =
							WeakPreview.Pin())
						{
							Preview->SetPreviewData(Result.PreviewData);
						}
						if (const TSharedPtr<SFoliageBakerLeafUVPreview> LeafUVPreview =
							WeakLeafUVPreview.Pin())
						{
							LeafUVPreview->SetPreviewData(Result.PreviewData);
						}
					}
					FFoliageBakerFeatureBakeItemResult ItemResult;
					ItemResult.bSucceeded = Result.bSucceeded;
					ItemResult.bCancelled = Result.bCancelled;
					ItemResult.Report = Result.Report;
					return ItemResult;
				}));

	FFoliageBakerFeatureTool::ShowBatchSummary(
		BatchResult,
		LOCTEXT(
			"AnalyzeTreeHierarchySummary",
			"Hierarchy analysis finished for {0} of {1} Static Mesh. No asset data was written.\n\n{2}"));
}

bool FFoliageBakerEditorModule::CanResolveDataBakeLeafOwnership() const
{
	return DataBakeLeafUVPreview.IsValid()
		&& DataBakeLeafUVPreview->CanResolveLeafOwnership();
}

void FFoliageBakerEditorModule::ResolveDataBakeLeafOwnership()
{
	if (DataBakeLeafUVPreview.IsValid())
	{
		DataBakeLeafUVPreview->ResolveLeafOwnership();
	}
}

bool FFoliageBakerEditorModule::CanBakeWindData() const
{
	return DataBakeSettings.IsValid()
		&& DataBakeSettings->SourceStaticMesh != nullptr
		&& DataBakePreviewData.IsValid()
		&& DataBakePreviewData->SourceStaticMesh.Get()
			== DataBakeSettings->SourceStaticMesh.Get()
		&& DataBakePreviewData->SourceLODIndex
			== DataBakeSettings->SourceLODIndex
		&& DataBakeLeafUVPreview.IsValid()
		&& DataBakeLeafUVPreview->HasResolvedLeafOwnership();
}

void FFoliageBakerEditorModule::BakeWindData()
{
	if (!CanBakeWindData())
	{
		return;
	}

	const TSharedPtr<const FFoliageBakerTreeHierarchyPreviewData> AnalysisData =
		DataBakePreviewData;
	const TArray<FFoliageBakerResolvedLeafCluster> ResolvedLeafClusters =
		DataBakeLeafUVPreview->GetResolvedLeafClusters();
	const int32 SourceLODIndex = DataBakeSettings->SourceLODIndex;
	int32 UnassignedTriangleCount = 0;
	TArray<TObjectPtr<UStaticMesh>> SourceStaticMeshes;
	SourceStaticMeshes.Add(DataBakeSettings->SourceStaticMesh);
	const FFoliageBakerFeatureBatchResult BatchResult =
		FFoliageBakerFeatureTool::RunBakeBatch(
			SourceStaticMeshes,
			LOCTEXT(
				"BakeWindDataSlowTask",
				"Baking linked tree wind data..."),
			false,
			TEXT("\n"),
			FFoliageBakerBakeStaticMeshDelegate::CreateLambda(
				[AnalysisData,
					ResolvedLeafClusters,
					SourceLODIndex,
					&UnassignedTriangleCount](
					UStaticMesh& StaticMesh)
				{
					const FFoliageBakerWindDataBakeResult Result =
						FFoliageBakerTreeHierarchyColorBaker::BakeWindData(
							StaticMesh,
							SourceLODIndex,
							*AnalysisData,
							ResolvedLeafClusters);
					FFoliageBakerFeatureBakeItemResult ItemResult;
					ItemResult.bSucceeded = Result.bSucceeded;
					ItemResult.Report = Result.Report;
					UnassignedTriangleCount = Result.UnassignedTriangleCount;
					if (Result.bSucceeded)
					{
						TStrongObjectPtr<UObject> PivotPositionTexture(
							StaticLoadObject(
								UObject::StaticClass(),
								nullptr,
								*Result.PivotPositionTexturePath));
						if (PivotPositionTexture)
						{
							ItemResult.CreatedAssets.Add(
								MoveTemp(PivotPositionTexture));
						}
						TStrongObjectPtr<UObject> PivotAxisTexture(
							StaticLoadObject(
								UObject::StaticClass(),
								nullptr,
								*Result.PivotAxisTexturePath));
						if (PivotAxisTexture)
						{
							ItemResult.CreatedAssets.Add(
								MoveTemp(PivotAxisTexture));
						}
					}
					return ItemResult;
				}));

	FFoliageBakerFeatureTool::SyncCreatedAssetsToContentBrowser(
		BatchResult.CreatedAssets);
	const FText SummaryFormat = LOCTEXT(
		"BakeWindDataSummary",
		"Wind Data bake finished for {0} of {1} Static Mesh.\n\n{2}");
	if (UnassignedTriangleCount <= 0)
	{
		FFoliageBakerFeatureTool::ShowBatchSummary(
			BatchResult,
			SummaryFormat);
		return;
	}

	const FText SummaryText = FText::Format(
		SummaryFormat,
		FText::AsNumber(BatchResult.SuccessCount),
		FText::AsNumber(BatchResult.TotalCount),
		FText::FromString(BatchResult.Report));
	const FText UnassignedGeometryWarning = FText::Format(
		LOCTEXT(
			"BakeWindDataUnassignedGeometryWarning",
			"{0} source triangle(s) had no hierarchy ownership and were mapped to Trunk BoneID 0."),
		FText::AsNumber(UnassignedTriangleCount));
	const TSharedRef<SWindow> SummaryWindow =
		SNew(SWindow)
		.Title(LOCTEXT("BakeWindDataSummaryTitle", "Foliage Baker - Wind Data Bake"))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMaximize(false)
		.SupportsMinimize(false);
	const TWeakPtr<SWindow> WeakSummaryWindow = SummaryWindow;
	SummaryWindow->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(16.0f)
		[
			SNew(SBox)
			.MaxDesiredWidth(760.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(SummaryText)
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 12.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(UnassignedGeometryWarning)
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Right)
				.Padding(0.0f, 16.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("BakeWindDataSummaryOk", "OK"))
					.OnClicked_Lambda([WeakSummaryWindow]()
					{
						if (const TSharedPtr<SWindow> Window =
							WeakSummaryWindow.Pin())
						{
							Window->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]
			]
		]);
	FSlateApplication::Get().AddModalWindow(
		SummaryWindow,
		FSlateApplication::Get().GetActiveTopLevelWindow());
}

void FFoliageBakerEditorModule::RefreshDataBakeSourceInput()
{
	DataBakePreviewData.Reset();
	RefreshDataBakeBranchOptions();
	if (DataBakePreview.IsValid())
	{
		DataBakePreview->ClearPreview();
	}
	TWeakObjectPtr<UStaticMesh> SourceStaticMesh;
	if (DataBakeSettings.IsValid()
		&& DataBakeSettings->SourceStaticMesh)
	{
		SourceStaticMesh = DataBakeSettings->SourceStaticMesh.Get();
	}
	if (DataBakeLeafUVPreview.IsValid())
	{
		DataBakeLeafUVPreview->SetSourceMesh(
			SourceStaticMesh,
			DataBakeSettings.IsValid()
				? DataBakeSettings->SourceLODIndex
				: 0);
	}
}

void FFoliageBakerEditorModule::HandleDataBakeLeafMaterialChanged(
	const int32 LeafMaterialIndex)
{
	(void)LeafMaterialIndex;
	DataBakePreviewData.Reset();
	RefreshDataBakeBranchOptions();
	if (DataBakePreview.IsValid())
	{
		DataBakePreview->ClearPreview();
	}
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
