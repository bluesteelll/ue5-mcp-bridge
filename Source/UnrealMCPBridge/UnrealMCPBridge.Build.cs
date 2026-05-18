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
				// Phase 3 — Level + Actor + Component surface.
				"LevelEditor",              // ULevelEditorSubsystem (NewLevel/LoadLevel/SaveCurrentLevel/etc.),
				                            // FEditorFileUtils streaming-level helpers used by level.* tools.
				// Phase 4 — Blueprint + Material surface. FBlueprintEditorUtils + FKismetEditorUtilities
				// + FCompilerResultsLog all live in UnrealEd (already listed). These two add the K2 graph
				// types (UK2Node_FunctionEntry / Result) and the PC_* enum constants from UEdGraphSchema_K2,
				// plus FKismetCompilerContext for future write-side support.
				"KismetCompiler",           // FCompilerResultsLog + FKismetCompilerContext
				                            // (bp.compile / bp.compile_all_dirty).
				"BlueprintGraph",           // UK2Node_FunctionEntry, UK2Node_FunctionResult, UEdGraphSchema_K2
				                            // PC_* constants (variable/function pin type IO).
			}
		);
	}
}
