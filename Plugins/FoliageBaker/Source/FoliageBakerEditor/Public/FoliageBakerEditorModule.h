#pragma once

#include "Modules/ModuleManager.h"

class SDockTab;
class SWidget;
class SWidgetSwitcher;
class FSpawnTabArgs;
struct FToolMenuContext;

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
	TSharedPtr<SWidgetSwitcher> WorkflowSwitcher;
	TSharedPtr<SWidgetSwitcher> FeatureSwitcher;
};
