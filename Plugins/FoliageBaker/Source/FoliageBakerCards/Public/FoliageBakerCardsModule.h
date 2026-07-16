#pragma once

#include "Modules/ModuleManager.h"
#include "UObject/StrongObjectPtr.h"

class FFoliageBakerFeatureController;
class SWidget;
class UFoliageBakerCardsSettings;
enum class EFoliageBakerCardMode : uint8;

class FOLIAGEBAKERCARDS_API FFoliageBakerCardsModule final : public IModuleInterface
{
public:
	virtual void ShutdownModule() override;

	TSharedRef<SWidget> CreateFeaturePanel(EFoliageBakerCardMode Mode);

private:
	void EnsureToolSettings(EFoliageBakerCardMode Mode);
	UFoliageBakerCardsSettings* GetToolSettings(EFoliageBakerCardMode Mode) const;
	TSharedPtr<FFoliageBakerFeatureController>& GetFeatureController(EFoliageBakerCardMode Mode);
	void Bake(EFoliageBakerCardMode Mode);
	bool CanBake(EFoliageBakerCardMode Mode) const;

	TStrongObjectPtr<UFoliageBakerCardsSettings> SingleBillboardSettings;
	TStrongObjectPtr<UFoliageBakerCardsSettings> CrossCardsSettings;
	TSharedPtr<FFoliageBakerFeatureController> SingleBillboardController;
	TSharedPtr<FFoliageBakerFeatureController> CrossCardsController;
};
