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
				"StaticMeshEditor",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"PropertyEditor",
				"AssetRegistry",
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
