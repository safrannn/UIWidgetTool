using UnrealBuildTool;

public class UIWidgetToolPluginEditor : ModuleRules
{
	public UIWidgetToolPluginEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"ApplicationCore",
				"Slate",
				"SlateCore",
				"InputCore",
				"UnrealEd",
				"DeveloperSettings",
				"Settings",
				"ToolMenus",
				"AssetTools",
				"AssetRegistry",
				"DirectoryWatcher",
				"EditorSubsystem",
				"Json",
				"WorkspaceMenuStructure",
				"Projects",
				"UMG",
				"UMGEditor",
				"ToolsetRegistry",
				"ModelContextProtocol",
				"Sockets",
				"DesktopPlatform",
				"ImageWrapper",
				"ImageCore",
				"RenderCore",
				"UIWidgetToolPlugin",
			}
			);
	}
}
