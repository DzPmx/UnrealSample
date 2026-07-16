using UnrealBuildTool;

public class FoliageBakerImpostor : ModuleRules
{
	public FoliageBakerImpostor(ReadOnlyTargetRules Target) : base(Target)
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
				"FoliageBakerCore",
				"FoliageBakerEditorCommon",
				"MeshDescription",
				"StaticMeshDescription",
				"MaterialBaking",
			}
		);
	}
}
