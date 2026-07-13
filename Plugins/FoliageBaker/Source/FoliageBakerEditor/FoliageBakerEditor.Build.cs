using UnrealBuildTool;

public class FoliageBakerEditor : ModuleRules
{
	public FoliageBakerEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Engine",
				"Slate",
				"SlateCore",
				"UnrealEd",
				"ToolMenus",
				"Settings",
				"FoliageBakerCards",
				"FoliageBakerImpostor",
				"FoliageBakerBillboardClouds",
			}
		);
	}
}
