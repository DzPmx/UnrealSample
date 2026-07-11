#pragma once

#include "Modules/ModuleManager.h"
#include "UObject/StrongObjectPtr.h"

class FReply;
class IDetailsView;
class SDockTab;
class SWidget;
class UFoliageBakerBillboardCloudsSettings;
class FSpawnTabArgs;
struct FToolMenuContext;

class FOLIAGEBAKERBILLBOARDCLOUDS_API FFoliageBakerBillboardCloudsModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;


	TSharedRef<SWidget> CreateFeaturePanel();

private:
	void RegisterMenus();
	void ExecuteCreatePlaneProxyMeshes(const FToolMenuContext& MenuContext);
	TSharedRef<SDockTab> SpawnBillboardCloudsToolTab(const FSpawnTabArgs& SpawnTabArgs);
	void EnsureToolSettings();
	void AddContentBrowserSelectionToTool();
	FReply HandleAddSelectedMeshes();
	FReply HandleClearMeshes();
	FReply HandleBake();
	bool CanBake() const;
	FText GetSourceMeshCountText() const;

	TStrongObjectPtr<UFoliageBakerBillboardCloudsSettings> ToolSettings;
	TSharedPtr<IDetailsView> SettingsDetailsView;
};
