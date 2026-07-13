using UnrealBuildTool;

public class FoliageBakerCore : ModuleRules
{
	public FoliageBakerCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"MaterialBaking",
				"MeshDescription",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AssetRegistry",
				"AssetTools",
				"InputCore",
				"Slate",
				"SlateCore",
				"StaticMeshDescription",
				"UnrealEd",
			}
		);
	}
}
