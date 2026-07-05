#pragma once

#include "Modules/ModuleManager.h"

struct FToolMenuContext;

class FBillboardCloudsEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();
	void ExecuteAnalyzeSelectedStaticMeshes(const FToolMenuContext& MenuContext) const;
	void ExecuteCreatePlaneProxyMeshes(const FToolMenuContext& MenuContext) const;
};
