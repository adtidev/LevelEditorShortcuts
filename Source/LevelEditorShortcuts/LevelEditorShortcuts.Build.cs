using UnrealBuildTool;

public class LevelEditorShortcuts : ModuleRules
{
	public LevelEditorShortcuts(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"Slate",
			"SlateCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"EditorFramework",
			"LevelEditor"
		});
	}
}
