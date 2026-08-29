using UnrealBuildTool;

public class SpeechForge : ModuleRules
{
	public SpeechForge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Dependencies are declared ahead of the code that needs them, on purpose. Adding a module
		// dependency later forces a full close-build-reopen cycle - Live Coding cannot hot-patch a
		// Build.cs change and crashes with an engine assert that looks like anything but your own
		// code. Front-loading the list costs nothing and buys back a lost afternoon.
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",           // public headers derive from UDataAsset / reference USoundWave
				"DeveloperSettings",
				"EditorSubsystem",  // the pipeline subsystem, next step
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"HTTP",             // provider REST calls
				"Json",
				"JsonUtilities",
				"Projects",         // IPluginManager
				"UnrealEd",         // asset creation, UAssetImportTask
				"AudioEditor",      // USoundFactory lives here, not in UnrealEd as the include path suggests
				"AssetTools",
				"AssetRegistry",
				"Slate",
				"SlateCore",
			}
			);

		// The credential store talks to the Windows Credential Manager directly. Other platforms
		// fall back to the environment variable until a Keychain/libsecret backend is written.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("Advapi32.lib");
		}
	}
}
