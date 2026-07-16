using UnrealBuildTool;

public class FoliageBakerCards : ModuleRules
{
	public FoliageBakerCards(ReadOnlyTargetRules Target) : base(Target)
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
			}
		);
	}
}
