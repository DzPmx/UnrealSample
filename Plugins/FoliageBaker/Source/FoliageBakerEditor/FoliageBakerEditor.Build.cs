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
				"InputCore",
				"MeshDescription",
				"RenderCore",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"Settings",
				"StaticMeshDescription",
				"PropertyEditor",
				"EditorFramework",
				"UnrealEd",
				"ToolWidgets",
				"FoliageBakerCore",
				"FoliageBakerEditorCommon",
				"FoliageBakerCards",
				"FoliageBakerImpostor",
				"FoliageBakerBillboardClouds",
			}
		);
	}
}
