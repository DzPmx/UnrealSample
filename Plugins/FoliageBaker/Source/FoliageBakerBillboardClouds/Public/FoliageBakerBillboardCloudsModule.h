#pragma once

#include "Modules/ModuleManager.h"
#include "UObject/StrongObjectPtr.h"

class FReply;
class IDetailsView;
class SWidget;
class UFoliageBakerBillboardCloudsSettings;

class FOLIAGEBAKERBILLBOARDCLOUDS_API FFoliageBakerBillboardCloudsModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	TSharedRef<SWidget> CreateFeaturePanel();

private:
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
