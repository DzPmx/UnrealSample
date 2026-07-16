#pragma once

#include "Modules/ModuleManager.h"
#include "UObject/StrongObjectPtr.h"

class FReply;
class IDetailsView;
class SWidget;
class UFoliageBakerImpostorSettings;

class FOLIAGEBAKERIMPOSTOR_API FFoliageBakerImpostorModule final : public IModuleInterface
{
public:
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

	TStrongObjectPtr<UFoliageBakerImpostorSettings> ToolSettings;
	TSharedPtr<IDetailsView> DetailsView;
};
