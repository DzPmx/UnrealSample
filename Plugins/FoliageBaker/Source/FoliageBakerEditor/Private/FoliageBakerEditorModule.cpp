#include "FoliageBakerEditorModule.h"

#include "FoliageBakerBillboardCloudsModule.h"
#include "FoliageBakerBillboardCloudsSettings.h"
#include "FoliageBakerCardsModule.h"
#include "FoliageBakerCardsSettings.h"
#include "FoliageBakerImpostorModule.h"
#include "FoliageBakerImpostorSettings.h"
#include "DetailLayoutBuilder.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "IDetailCustomization.h"
#include "ISettingsModule.h"
#include "PropertyEditorModule.h"
#include "Styling/AppStyle.h"
#include "Templates/SubclassOf.h"
#include "ToolMenus.h"
#include "UObject/ObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FFoliageBakerEditorModule"

namespace
{
	const FName FoliageBakerToolTabName(TEXT("FoliageBakerTools"));
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
					"Selectable source LOD  |  Single Plane or Double Planes  |  orthographic capture  |  1024 default")
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
					"Configure Single Plane and Double Planes Billboard preferences, including their default Parent Material Instances."),
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
		.SetTooltipText(LOCTEXT("FoliageBakerToolTabTooltip", "Bake distant foliage representations from a selected Static Mesh LOD."))
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
	FeatureSwitcher.Reset();
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
		LOCTEXT("OpenFoliageBakerTooltip", "Open the unified Billboard, Cross Cards, Impostor, MultiBillboard, and BillboardClouds tool."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"),
		FToolMenuExecuteAction::CreateRaw(this, &FFoliageBakerEditorModule::ExecuteOpenTool));
}

TSharedRef<SDockTab> FFoliageBakerEditorModule::SpawnToolTab(const FSpawnTabArgs& SpawnTabArgs)
{
	(void)SpawnTabArgs;
	ActiveFeatureIndex = BillboardFeatureIndex;

	FFoliageBakerBillboardCloudsModule& BillboardCloudsModule =
		FModuleManager::LoadModuleChecked<FFoliageBakerBillboardCloudsModule>(TEXT("FoliageBakerBillboardClouds"));
	FFoliageBakerCardsModule& CardsModule =
		FModuleManager::LoadModuleChecked<FFoliageBakerCardsModule>(TEXT("FoliageBakerCards"));
	FFoliageBakerImpostorModule& ImpostorModule =
		FModuleManager::LoadModuleChecked<FFoliageBakerImpostorModule>(TEXT("FoliageBakerImpostor"));

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
								.Text(LOCTEXT("FoliageBakerHeaderSubtitle", "Bake optimized distant foliage representations from a selected Static Mesh LOD."))
							]
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 14.0f, 0.0f, 0.0f)
					[
						FeatureTabs
					]
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
			]
		];

	FeatureSwitcher->SetActiveWidgetIndex(ActiveFeatureIndex);
	return ToolTab;
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
