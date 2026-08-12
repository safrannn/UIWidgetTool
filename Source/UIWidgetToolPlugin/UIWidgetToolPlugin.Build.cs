using UnrealBuildTool;

public class UIWidgetToolPlugin : ModuleRules
{
	public UIWidgetToolPlugin(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"UMG",			
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"UnrealEd",          // editor, blueprint compile hooks
				"DeveloperSettings", // UDeveloperSettings config store
				"ToolMenus",         // menu/toolbar + content browser context menu
				"ContentBrowser",
				"AssetTools",
				"EditorSubsystem",
				"WorkspaceMenuStructure",
				"Projects",
				"UMGEditor",         // UWidgetBlueprint
				"InputCore",         // EKeys, used internally by STableRow
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
