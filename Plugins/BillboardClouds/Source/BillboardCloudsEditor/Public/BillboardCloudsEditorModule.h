#pragma once

#include "Modules/ModuleManager.h"
#include "UObject/StrongObjectPtr.h"

class FReply;
class IDetailsView;
class SDockTab;
class UBillboardCloudsEditorSettings;
class FSpawnTabArgs;
struct FToolMenuContext;

class FBillboardCloudsEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

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

	TStrongObjectPtr<UBillboardCloudsEditorSettings> ToolSettings;
	TSharedPtr<IDetailsView> SettingsDetailsView;
};
