using UnrealBuildTool;

public class FoliageBakerBillboardClouds : ModuleRules
{
	public FoliageBakerBillboardClouds(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"FoliageBakerCore",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Engine",
				"Slate",
				"SlateCore",
				"UnrealEd",
				"AssetRegistry",
				"ContentBrowser",
				"PropertyEditor",
				"MeshDescription",
				"StaticMeshDescription",
				"MaterialBaking",
			}
		);
	}
}
