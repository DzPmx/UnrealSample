#pragma once

#include "Modules/ModuleManager.h"
#include "UObject/StrongObjectPtr.h"

class FFoliageBakerFeatureController;
class SWidget;
class UFoliageBakerImpostorSettings;

class FOLIAGEBAKERIMPOSTOR_API FFoliageBakerImpostorModule final : public IModuleInterface
{
public:
	virtual void ShutdownModule() override;

	TSharedRef<SWidget> CreateFeaturePanel();

private:
	void EnsureToolSettings();
	void Bake();
	bool CanBake() const;

	TStrongObjectPtr<UFoliageBakerImpostorSettings> ToolSettings;
	TSharedPtr<FFoliageBakerFeatureController> FeatureController;
};
