#include "FoliageBakerCoreModule.h"

#include "Interfaces/IPluginManager.h"
#include "ShaderCore.h"

void FFoliageBakerCoreModule::StartupModule()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FoliageBaker"));
	if (Plugin.IsValid())
	{
		AddShaderSourceDirectoryMapping(
			TEXT("/Plugin/FoliageBaker"),
			FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
	}
}

IMPLEMENT_MODULE(FFoliageBakerCoreModule, FoliageBakerCore)
