#pragma once

#include "Modules/ModuleManager.h"
#include "UObject/StrongObjectPtr.h"

class FReply;
class IDetailsView;
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
	TSharedPtr<IDetailsView>& GetDetailsView(EFoliageBakerCardMode Mode);
	void AddContentBrowserSelectionToTool(EFoliageBakerCardMode Mode);
	FReply HandleAddSelectedMeshes(EFoliageBakerCardMode Mode);
	FReply HandleClearMeshes(EFoliageBakerCardMode Mode);
	FReply HandleBake(EFoliageBakerCardMode Mode);
	bool CanBake(EFoliageBakerCardMode Mode) const;
	FText GetSourceMeshCountText(EFoliageBakerCardMode Mode) const;

	TStrongObjectPtr<UFoliageBakerCardsSettings> SingleBillboardSettings;
	TStrongObjectPtr<UFoliageBakerCardsSettings> CrossCardsSettings;
	TSharedPtr<IDetailsView> SingleBillboardDetailsView;
	TSharedPtr<IDetailsView> CrossCardsDetailsView;
};
