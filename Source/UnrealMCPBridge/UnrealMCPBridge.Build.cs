// Copyright FatumGame. All Rights Reserved.

using UnrealBuildTool;

public class UnrealMCPBridge : ModuleRules
{
	public UnrealMCPBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		CppStandard = CppStandardVersion.Cpp20;

		IWYUSupport = IWYUSupport.Full;

		PublicIncludePaths.AddRange(
			new string[]
			{
			}
		);

		PrivateIncludePaths.AddRange(
			new string[]
			{
			}
		);

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Json",
				"JsonUtilities",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UnrealEd",
				"Sockets",
				"Networking",
				"Projects",
				"PythonScriptPlugin",
				// Phase 2 — Assets + Content Browser surface.
				"AssetRegistry",            // IAssetRegistry::Get, FARFilter, FAssetData, GetReferencers/GetDependencies
				"AssetTools",               // IAssetTools::ImportAssetTasks, FixupReferencers, RenameAssets
				"ContentBrowser",           // IContentBrowserSingleton (folder ops + future sync)
				"ContentBrowserData",       // FContentBrowserDataFilter (folder enumeration, used by cb.list_folders via IAR)
				"EditorScriptingUtilities", // UEditorAssetSubsystem (LoadAsset, DoesAssetExist, DeleteAsset, SaveAsset, etc.)
				"ImageCore",                // FImageView (constructs from FColor* for thumbnail encoding)
				"ImageWrapper",             // FImageUtils::CompressImageArray for thumbnail PNG encoding
			}
		);
	}
}
