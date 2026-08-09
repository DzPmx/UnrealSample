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
				"Engine",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"RenderCore",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"Settings",
				"PropertyEditor",
				"EditorFramework",
				"UnrealEd",
				"FoliageBakerCore",
				"FoliageBakerEditorCommon",
				"FoliageBakerCards",
				"FoliageBakerImpostor",
				"FoliageBakerBillboardClouds",
			}
		);
	}
}
