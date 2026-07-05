using UnrealBuildTool;

public class BillboardCloudsEditor : ModuleRules
{
	public BillboardCloudsEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"DeveloperSettings",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"UnrealEd",
				"ToolMenus",
				"Projects",
				"AssetTools",
				"AssetRegistry",
				"ContentBrowser",
				"MeshDescription",
				"StaticMeshDescription",
				"GeometryCore",
				"MeshConversion",
			}
		);
	}
}
