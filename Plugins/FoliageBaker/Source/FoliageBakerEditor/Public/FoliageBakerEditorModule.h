#pragma once

#include "Modules/ModuleManager.h"
#include "UObject/StrongObjectPtr.h"

class SDockTab;
class SWidget;
class SWidgetSwitcher;
class SFoliageBakerTreeHierarchyPreview;
class FSpawnTabArgs;
class FFoliageBakerFeatureController;
class FAutoConsoleCommand;
class UFoliageBakerTreeHierarchySettings;
struct FToolMenuContext;
struct FFoliageBakerTreeHierarchyPreviewData;
template <typename OptionType>
class SListView;

class FFoliageBakerEditorModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterEditorPreferences();
	void UnregisterEditorPreferences();
	void RegisterMenus();
	void ExecuteOpenTool(const FToolMenuContext& MenuContext);
	TSharedRef<SDockTab> SpawnToolTab(const FSpawnTabArgs& SpawnTabArgs);
	TSharedRef<SWidget> CreateDataBakePanel();
	void EnsureDataBakeSettings();
	bool CanBakeTreeHierarchyColors() const;
	void BakeTreeHierarchyColors();
	void RefreshDataBakeBranchOptions();
	void RefreshDataBakeBranchSelection();
	void MarkSelectedHierarchyBranchAsTrunk();
	FText GetActiveFeatureTitle() const;
	FText GetActiveFeatureDescription() const;
	FText GetActiveFeatureMetadata() const;
	int32 GetActiveWorkflowIndex() const;
	int32 GetActiveFeatureIndex() const;
	void HandleWorkflowChanged(int32 NewWorkflowIndex);
	void HandleFeatureChanged(int32 NewFeatureIndex);

	int32 ActiveWorkflowIndex = 0;
	int32 ActiveFeatureIndex = 0;
	TArray<FName> RegisteredPreferenceClassNames;
	TStrongObjectPtr<UFoliageBakerTreeHierarchySettings> DataBakeSettings;
	TSharedPtr<FFoliageBakerFeatureController> DataBakeController;
	TSharedPtr<SFoliageBakerTreeHierarchyPreview> DataBakePreview;
	TSharedPtr<FFoliageBakerTreeHierarchyPreviewData> DataBakePreviewData;
	TArray<TSharedPtr<int32>> DataBakeBranchOptions;
	TSet<int32> SelectedDataBakeBranchIDs;
	TSharedPtr<SListView<TSharedPtr<int32>>> DataBakeBranchList;
	TSharedPtr<SWidgetSwitcher> WorkflowSwitcher;
	TSharedPtr<SWidgetSwitcher> FeatureSwitcher;
	TUniquePtr<FAutoConsoleCommand> DebugPreviewCommand;
};
