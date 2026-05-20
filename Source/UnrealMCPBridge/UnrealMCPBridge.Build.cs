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
				"InputCore",
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
				// (niagara.list_parameters read-only enumeration).
				"Niagara",                  // UNiagaraSystem::GetExposedParameters / GetEmitterHandles
				                            // + UNiagaraComponent::SetEmitterEnable (niagara.set_emitter_enabled).
				// Wave B 2026-05 — Niagara writes need UNiagaraEmitterFactoryNew (niagara.create_emitter).
				// NiagaraEditor is editor-only — fine for the bridge (UnrealMCPBridge is editor-only too).
				"NiagaraEditor",            // UNiagaraEmitterFactoryNew.
				// Physics traces (physics.line_trace / sweep_capsule) — UWorld::LineTraceMultiByChannel
				// / SweepMultiByChannel + FCollisionShape::MakeCapsule + FCollisionQueryParams +
				// ECollisionChannel + FHitResult are all in Engine (already a public dep). No additional
				// physics module required for Chaos query path.
				// Phase 5 Chunk D — Sequencer read-only surface.
				// LevelSequence runtime: ULevelSequence class + asset type.
				// MovieScene runtime: UMovieScene + UMovieSceneTrack + UMovieSceneSection + FMovieSceneBinding
				// + FMovieScenePossessable + FMovieSceneSpawnable + FMovieSceneChannelProxy + float/double/
				// bool/integer channel structs.
				// MovieSceneTracks: UMovieSceneCameraCutTrack + UMovieSceneCameraCutSection.
				// LevelSequenceEditor: ULevelSequenceEditorBlueprintLibrary (GetCurrentLevelSequence +
				// GetGlobalPosition for sequencer.get_current_time). Editor-only — the plugin is editor-only
				// already so the dep is safe.
				"LevelSequence",
				"MovieScene",
				"MovieSceneTracks",
				"LevelSequenceEditor",
				// Phase 6 Chunk A — Source Control surface.
				// `SourceControl` module exposes ISourceControlModule, ISourceControlProvider,
				// ISourceControlState, ISourceControlRevision, and the operation classes
				// (FUpdateStatus, FCheckOut, FRevert, FCheckIn) used by sc.* tools.
				// Provider implementations (Git LFS, Perforce, Subversion) live in separate
				// plugins and are loaded by their own modules — we never reference them directly
				// (we go through the abstract ISourceControlProvider interface).
				"SourceControl",
				// Wave D Surface 1 2026-05 — GameplayTag surface.
				// UGameplayTagsManager + FGameplayTag + FGameplayTagContainer + IGameplayTagAssetInterface
				// live in the Runtime/GameplayTags module.
				"GameplayTags",
			}
		);

		// Phase 6 Chunk E — Live Coding surface (Windows desktop editor only).
		// ``LiveCoding`` module exposes ``ILiveCodingModule`` which the Phase 6 livecoding.recompile
		// async composite uses to drive ``Compile()`` + ``IsCompiling()`` + ``GetOnPatchCompleteDelegate()``.
		// LiveCoding.Build.cs restricts the module to Win64 (it links Windows-specific patch
		// machinery in Developer/Windows/LiveCoding/Private). All non-Windows references must be
		// gated by ``#if PLATFORM_WINDOWS`` in our code, and the dep must be conditional here so
		// non-Windows builds of the bridge module link without LiveCoding's Win64 libs.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}
	}
}
