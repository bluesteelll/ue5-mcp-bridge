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
				// Phase 4 Days 11-15 — Material surface adds UMaterialEditingLibrary +
				// UMaterialInstanceConstantFactoryNew (latter lives in UnrealEd/Factories — UnrealEd
				// already listed). MaterialEditor is required for UMaterialEditingLibrary's static
				// methods (GetScalar/Vector/Texture/StaticSwitch parameter names + Set*).
				"MaterialEditor",           // UMaterialEditingLibrary (material.* parameter IO).
				"RHI",                      // GMaxRHIShaderPlatform (material.get_compile_errors).
				                            // (GShaderCompilingManager is ENGINE_API — already linked.)
				// Phase 5 Chunk B — Editor utilities surface.
				// LevelEditor already listed above (Phase 3). Slate + SlateCore needed for
				// FSlateNotificationManager (editor.show_message) + SLevelViewport access
				// (editor.viewport_screenshot active-viewport resolution).
				"Slate",                    // FSlateNotificationManager, SNotificationItem
				"SlateCore",                // FNotificationInfo, slate enums
				// ImageWrapper already listed (Phase 2) — reused by FMCPScreenshotUtils for
				// PNG/JPG encode via FImageUtils::CompressImage.
				// Phase 5 Chunk C — UMG + Niagara + Physics surface.
				// UMG runtime: UWidget, UWidgetTree, UUserWidget, UPanelWidget.
				// UMGEditor: UWidgetBlueprint (asset class) — only available in editor builds; the
				// Phase 5 Chunk C tools are editor-only by design (UnrealEd is already a private dep).
				"UMG",                      // UWidget / UWidgetTree / UUserWidget (runtime classes).
				"UMGEditor",                // UWidgetBlueprint — Cast<UWidgetBlueprint> + WidgetTree access.
				// Niagara runtime — UNiagaraSystem / UNiagaraEmitter / FNiagaraParameterStore /
				// FNiagaraUserRedirectionParameterStore / FNiagaraVariable + FNiagaraTypeDefinition
				// (niagara.list_parameters read-only enumeration). NiagaraEditor NOT needed for
				// read-side enumeration — exposed parameters live on the runtime UNiagaraSystem.
				"Niagara",                  // UNiagaraSystem::GetExposedParameters / GetEmitterHandles.
				// Physics traces (physics.line_trace / sweep_capsule) — UWorld::LineTraceMultiByChannel
				// / SweepMultiByChannel + FCollisionShape::MakeCapsule + FCollisionQueryParams +
				// ECollisionChannel + FHitResult are all in Engine (already a public dep). No additional
				// physics module required for Chaos query path.
			}
		);
	}
}
