using System.IO;
using UnrealBuildTool;

public class FoliageBakerCore : ModuleRules
{
	public FoliageBakerCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// The shared masked output baker reuses the engine export compiler helpers
		// from a plugin-owned material proxy. No Engine source change is required.
		PrivateIncludePaths.Add(
			Path.Combine(EngineDirectory, "Source/Developer/MaterialBaking/Private")
		);

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
				"ImageCore",
				"InputCore",
				"RenderCore",
				"RHI",
				"Renderer",
				"Slate",
				"SlateCore",
				"StaticMeshDescription",
				"UnrealEd",
			}
		);
	}
}
