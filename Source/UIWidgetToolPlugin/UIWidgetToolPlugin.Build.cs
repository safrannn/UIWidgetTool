using UnrealBuildTool;

public class UIWidgetToolPlugin : ModuleRules
{
	public UIWidgetToolPlugin(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"UMG",
				"DeveloperSettings",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Json",
			}
			);
	}
}
