using UnrealBuildTool;

public class TextureBakerEditor : ModuleRules
{
	public TextureBakerEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"Core",
				"CoreUObject",
				"Engine"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"UnrealEd",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"PropertyEditor",
				"AssetRegistry",
				"AssetTools",
				"Projects",
				"RHI",
				"RenderCore",
				"MeshDescription",
				"StaticMeshDescription",
				"MeshBuilder",
				"MeshBuilderCommon",
				"GeometryCore",
				"DynamicMesh",
				"MeshConversion",
				"GeometryAlgorithms"
			}
		);
	}
}
