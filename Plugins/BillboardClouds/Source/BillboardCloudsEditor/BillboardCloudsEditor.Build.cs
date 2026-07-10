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
				"AssetTools",
				"AssetRegistry",
				"ContentBrowser",
				"PropertyEditor",
				"MeshDescription",
				"StaticMeshDescription",
				"MaterialBaking",
				"RenderCore",
				"RHI",
				"Renderer",
			}
		);
	}
}
