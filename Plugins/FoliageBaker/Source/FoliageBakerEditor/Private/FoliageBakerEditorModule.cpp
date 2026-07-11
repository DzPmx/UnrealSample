#include "FoliageBakerEditorModule.h"

#include "FoliageBakerBillboardCloudsModule.h"
#include "FoliageBakerCardsModule.h"
#include "FoliageBakerCardsSettings.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FFoliageBakerEditorModule"

namespace
{
	const FName FoliageBakerToolTabName(TEXT("FoliageBakerTools"));
	constexpr int32 SingleBillboardFeatureIndex = 0;
	constexpr int32 CrossCardsFeatureIndex = 1;
	constexpr int32 ImpostorFeatureIndex = 2;
	constexpr int32 BillboardCloudsFeatureIndex = 3;
	constexpr int32 FeatureCount = 4;
}

void FFoliageBakerEditorModule::StartupModule()
{
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

void FFoliageBakerEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);
	UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
	if (!ToolsMenu)
	{
		return;
	}

	FToolMenuSection& Section = ToolsMenu->FindOrAddSection(TEXT("FoliageBaker"));
	Section.Label = LOCTEXT("FoliageBakerSection", "Foliage Baker");
	Section.AddMenuEntry(
		TEXT("OpenFoliageBaker"),
		LOCTEXT("OpenFoliageBakerLabel", "Foliage Baker"),
		LOCTEXT("OpenFoliageBakerTooltip", "Open the unified Single Billboard, Cross Cards, Impostor, and BillboardClouds tool."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"),
		FToolMenuExecuteAction::CreateRaw(this, &FFoliageBakerEditorModule::ExecuteOpenTool));
}

TSharedRef<SDockTab> FFoliageBakerEditorModule::SpawnToolTab(const FSpawnTabArgs& SpawnTabArgs)
{
	(void)SpawnTabArgs;
	ActiveFeatureIndex = SingleBillboardFeatureIndex;

	FFoliageBakerBillboardCloudsModule& BillboardCloudsModule =
		FModuleManager::LoadModuleChecked<FFoliageBakerBillboardCloudsModule>(TEXT("FoliageBakerBillboardClouds"));
	FFoliageBakerCardsModule& CardsModule =
		FModuleManager::LoadModuleChecked<FFoliageBakerCardsModule>(TEXT("FoliageBakerCards"));

	TSharedRef<SSegmentedControl<int32>> FeatureTabs =
		SNew(SSegmentedControl<int32>)
		.Value_Lambda([this]() { return GetActiveFeatureIndex(); })
		.OnValueChanged_Lambda([this](const int32 NewFeatureIndex) { HandleFeatureChanged(NewFeatureIndex); })
		.UniformPadding(FMargin(22.0f, 7.0f))
		.MaxSegmentsPerLine(FeatureCount);

	FeatureTabs->AddSlot(SingleBillboardFeatureIndex)
		.HAlign(HAlign_Center)
		.Text(LOCTEXT("SingleBillboardTab", "Single Billboard"));
	FeatureTabs->AddSlot(CrossCardsFeatureIndex)
		.HAlign(HAlign_Center)
		.Text(LOCTEXT("CrossCardsTab", "Cross Cards"));
	FeatureTabs->AddSlot(ImpostorFeatureIndex)
		.HAlign(HAlign_Center)
		.Text(LOCTEXT("ImpostorTab", "Impostor"));
	FeatureTabs->AddSlot(BillboardCloudsFeatureIndex)
		.HAlign(HAlign_Center)
		.Text(LOCTEXT("BillboardCloudsTab", "BillboardClouds"));

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
					SAssignNew(FeatureSwitcher, SWidgetSwitcher)
					+ SWidgetSwitcher::Slot()
					[
						CardsModule.CreateFeaturePanel(EFoliageBakerCardMode::SingleBillboard)
					]
					+ SWidgetSwitcher::Slot()
					[
						CardsModule.CreateFeaturePanel(EFoliageBakerCardMode::CrossCards)
					]
					+ SWidgetSwitcher::Slot()
					[
						CreateImpostorPlaceholder()
					]
					+ SWidgetSwitcher::Slot()
					[
						BillboardCloudsModule.CreateFeaturePanel()
					]
				]
			]
		];

	FeatureSwitcher->SetActiveWidgetIndex(ActiveFeatureIndex);
	return ToolTab;
}

TSharedRef<SWidget> FFoliageBakerEditorModule::CreateImpostorPlaceholder() const
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("NoBorder"))
		.Padding(40.0f)
		.VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ImpostorDeferredTitle", "Impostor baking is reserved"))
				.TextStyle(FAppStyle::Get(), "LargeText")
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ImpostorDeferredBody", "The interface is ready for the future hemisphere and full-sphere workflow. Baking remains disabled until the remaining requirements are confirmed."))
				.AutoWrapText(true)
			]
		];
}

FText FFoliageBakerEditorModule::GetActiveFeatureTitle() const
{
	switch (ActiveFeatureIndex)
	{
	case CrossCardsFeatureIndex:
		return LOCTEXT("CrossCardsFeatureTitle", "Cross Cards");
	case ImpostorFeatureIndex:
		return LOCTEXT("ImpostorFeatureTitle", "Impostor");
	case BillboardCloudsFeatureIndex:
		return LOCTEXT("BillboardCloudsFeatureTitle", "BillboardClouds");
	case SingleBillboardFeatureIndex:
	default:
		return LOCTEXT("SingleBillboardFeatureTitle", "Single Billboard");
	}
}

FText FFoliageBakerEditorModule::GetActiveFeatureDescription() const
{
	switch (ActiveFeatureIndex)
	{
	case CrossCardsFeatureIndex:
		return LOCTEXT("CrossCardsFeatureDescription", "Bake 2-5 equally spaced vertical cards. Every angle is cropped independently and captures both front and back.");
	case ImpostorFeatureIndex:
		return LOCTEXT("ImpostorFeatureDescription", "Reserved for the future hemisphere and full-sphere impostor workflow.");
	case BillboardCloudsFeatureIndex:
		return LOCTEXT("BillboardCloudsFeatureDescription", "Generate an adaptive cloud of planes using the existing BillboardClouds workflow.");
	case SingleBillboardFeatureIndex:
	default:
		return LOCTEXT("SingleBillboardFeatureDescription", "Bake one vertical billboard from a user-selected +X, -X, +Y, or -Y capture axis.");
	}
}

FText FFoliageBakerEditorModule::GetActiveFeatureMetadata() const
{
	switch (ActiveFeatureIndex)
	{
	case CrossCardsFeatureIndex:
		return LOCTEXT("CrossCardsFeatureMetadata", "Selectable source LOD  |  2-5 planes  |  front + back  |  2048 default");
	case ImpostorFeatureIndex:
		return LOCTEXT("ImpostorFeatureMetadata", "Deferred  |  no assets are generated from this tab");
	case BillboardCloudsFeatureIndex:
		return LOCTEXT("BillboardCloudsFeatureMetadata", "Selectable source LOD  |  adaptive plane cloud  |  existing output workflow");
	case SingleBillboardFeatureIndex:
	default:
		return LOCTEXT("SingleBillboardFeatureMetadata", "Selectable source LOD  |  one plane  |  one baked side  |  1024 default");
	}
}

int32 FFoliageBakerEditorModule::GetActiveFeatureIndex() const
{
	return ActiveFeatureIndex;
}

void FFoliageBakerEditorModule::HandleFeatureChanged(const int32 NewFeatureIndex)
{
	ActiveFeatureIndex = FMath::Clamp(NewFeatureIndex, 0, FeatureCount - 1);
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
