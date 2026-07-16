#pragma once

#include "Modules/ModuleManager.h"
#include "UObject/StrongObjectPtr.h"

class FFoliageBakerFeatureController;
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
	void Bake();
	bool CanBake() const;

	TStrongObjectPtr<UFoliageBakerBillboardCloudsSettings> ToolSettings;
	TSharedPtr<FFoliageBakerFeatureController> FeatureController;
};
