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
				// Phase 5 module split (2026-05-22) — sister module owning server + helpers + utils.
				// Public dep so Tools surfaces transparently see Core's Public/ headers
				// (MCPToolHelpers, MCPAssetLoader, MCPMutatorScope, MCPJsonBuilder, Utils/*).
				"UnrealMCPBridgeCore",
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
				// Wave E Surface 5 2026-05 — Enhanced Input introspection surface.
				// UInputMappingContext + UInputAction + FEnhancedActionKeyMapping + UInputModifier +
				// UInputTrigger + UEnhancedInputLocalPlayerSubsystem all live in EnhancedInput
				// (runtime module, plugin loaded by default in UE 5.7).
				"EnhancedInput",
				// Wave E Surface 6 2026-05 — Generic UE subsystem reflection surface.
				// UEditorSubsystem lives in its own module (Editor/EditorSubsystem) and its header
				// (EditorSubsystem.h) is required for the editor-collection enumeration in
				// subsystem.list. UEngineSubsystem / UWorldSubsystem / UGameInstanceSubsystem /
				// ULocalPlayerSubsystem all live in Engine (already a public dep).
				"EditorSubsystem",
				// Wave G Surface 3 2026-05 — Navigation system query surface.
				// UNavigationSystemV1 / ANavigationData / ARecastNavMesh / FPathFindingQuery /
				// FPathFindingResult / FNavLocation / FNavPathPoint all live in the NavigationSystem
				// module (runtime). The plugin is editor-only so this dep is safe.
				"NavigationSystem",
				// Wave G Surface 4 2026-05 — Anim Blueprint state machine surface.
				// UAnimGraphNode_StateMachine / UAnimationStateMachineGraph / UAnimStateNode /
				// UAnimStateTransitionNode / UAnimStateEntryNode / UAnimGraphNode_StateResult /
				// UAnimGraphNode_SequencePlayer / UAnimationGraphSchema all live in the AnimGraph
				// module (editor-only — fine, the plugin is editor-only too). AnimGraphRuntime is
				// pulled transitively (AnimGraph publicly depends on AnimGraphRuntime).
				"AnimGraph",
				// Wave H Surface 3 2026-05 — Data validation surface.
				// UEditorValidatorSubsystem / UEditorValidatorBase live in the DataValidation
				// plugin module (editor-only, shipped with Engine — Plugins/Editor/DataValidation).
				// FDataValidationContext + EDataValidationResult are in CoreUObject (already
				// transitively linked via UnrealEd → Engine → CoreUObject).
				"DataValidation",
				// Wave H Surface 6 2026-05 — Cooking automation surface.
				// ITargetPlatform / ITargetPlatformManagerModule live in the TargetPlatform module
				// (Developer/TargetPlatform). DesktopPlatform exposes FDesktopPlatformModule which
				// the cook subprocess helpers use for cross-platform exe path resolution helpers.
				// FMonitoredProcess + FProcHandle live in Core (already a public dep transitively).
				"TargetPlatform",
				"DesktopPlatform",
				// Wave I Surface 6 2026-05: landscape.* read-only surface.
				// ``Landscape`` runtime module exposes ALandscape + ULandscapeInfo + FLandscapeEditDataInterface
				// (the latter is gated by ``#if WITH_EDITOR`` and exported via LANDSCAPE_API — the plugin is
				// editor-only so the gate is always satisfied). LandscapeTools.cpp samples heightmap +
				// weightmap data via the editor-only edit-data interface.
				// ``Foliage`` is a transitive dep of LandscapeEdit.h — it includes InstancedFoliageActor.h
				// directly so we must list the module even though LandscapeTools.cpp never names a
				// foliage symbol itself.
				"Landscape",
				"Foliage",
				// Wave J 2026-05: ai.* surface (6 sub-surfaces, ~19 tools — bt/bb/controller/eqs/
				// perception/crowd). All 6 share AIModule which exports AAIController +
				// UBehaviorTree + UBlackboardComponent + UAIPerceptionComponent + UCrowdManager +
				// UCrowdFollowingComponent + UEnvQuery + UEnvQueryManager + the FAISenseID/
				// UAISense_* + UBlackboardKeyType_* hierarchy. GameplayTasks is transitive via
				// AIModule (public dep) so explicit listing is redundant. First wave using
				// the auto-register architecture — agents touched ONLY their own AIxxxTools.{h,cpp}
				// files + MCP_REGISTER_SURFACE macro; UnrealMCPBridge.cpp untouched.
				"AIModule",
				// Wave K 2026-05-22: render_target.* surface needs FlushRenderingCommands() from
				// RenderCore. UTextureRenderTarget2D class itself + FReadSurfaceDataFlags +
				// FTextureRenderTargetResource live in Engine + RHI (already transitive). Only
				// RenderCore is missing for the GPU readback sync barrier.
				"RenderCore",
				// Wave L 2026-05-23: insights.* surface needs FTraceAuxiliary (Core header,
				// implementation in TraceLog) + UE::Trace::FChannel::EnumerateChannels (also
				// TraceLog). The header itself ships in Core's ProfilingDebugging/, but every
				// concrete entry point links against TraceLog's static lib. stat.* and
				// memreport.* surfaces transitively pick up RHIStats / RenderTimer globals
				// from already-linked modules (RHI / RenderCore).
				"TraceLog",
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
