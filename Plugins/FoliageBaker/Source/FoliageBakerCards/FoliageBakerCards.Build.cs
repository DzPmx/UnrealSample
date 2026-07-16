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
				"ContentBrowser",
				"PropertyEditor",
			}
		);
	}
}
