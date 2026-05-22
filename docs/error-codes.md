---
title: Error codes
layout: default
nav_order: 5
---

# Error codes

Every error response carries a JSON-RPC-style code from the `-32000..-32099` server-error range. All codes are defined in `Source/UnrealMCPBridge/Public/MCPTypes.h`.

## JSON-RPC base codes

| Code | Name | When |
|---|---|---|
| `-32600` | InvalidRequest | Malformed JSON envelope |
| `-32601` | MethodNotFound | Unknown method (not in C++ map nor Python registry) |
| `-32602` | InvalidParams | Missing required arg, wrong type, malformed payload |
| `-32603` | InternalError | Unexpected null after passing validation; should be rare |

## Asset / object lookup

| Code | Name | When | Recovery |
|---|---|---|---|
| `-32004` | ObjectNotFound | LoadObject returned null after retry | Verify path / pre-load via cb.load_asset |
| `-32005` | PropertyNotFound | FProperty by name not on target class | Use marshall.list_properties to enumerate |
| `-32006` | PropertyTypeMismatch | Cast failed OR ImportText couldn't parse value | Check expected type in marshall.describe_struct |
| `-32007` | PropertyAccessDenied | CPF_EditorOnly / CPF_BlueprintReadOnly write blocked | Use edit-const bypass tool if available |

## Asset paths + queries

| Code | Name | When |
|---|---|---|
| `-32010` | InvalidPath | Empty, backslashes, `..`, unknown mount |
| `-32011` | WrongClass | class_path doesn't resolve to expected UClass |
| `-32012` | OverlyBroadQuery | Recursive /Game scan with no filters; or hard caps (500/10k/5000) |
| `-32013` | PathEscape | Disk path arg outside sandbox whitelist |
| `-32014` | PathInUse | rename/duplicate/create target already exists |
| `-32015` | StaleCursor | page_token's embedded filter_hash mismatches current filter (pagination semantic ONLY — was double-meaning until Refactor Phase 4 split engine-side refusal into `-32058 OperationFailed`) |
| `-32016` | JobSubmitFailed | FMCPJobRegistry::SubmitJob failed |
| `-32017` | InputTooLarge | Synchronous-batch limit exceeded (200) |
| `-32018` | ThumbnailRenderFailed | RenderThumbnail returned empty or encode failed |

## Level + Actor

| Code | Name | When |
|---|---|---|
| `-32019` | LevelNotFound | Map asset path resolves to no UWorld |
| `-32020` | ClassNotFound | UClass not loadable |
| `-32021` | ClassAbstract | actor.spawn target has CLASS_Abstract |
| `-32022` | WrongClassFamily | actor.spawn class isn't an AActor subclass |
| `-32023` | InvalidClassPath | Malformed (no /Script/ or /Game/ mount) |
| `-32024` | AmbiguousComponent | Multiple components match the name |
| `-32025` | PropertyPathTooDeep | Property nesting > 16 segments |
| `-32026` | PropertyIndexOOB | [N] index past array bounds |
| `-32027` | PIEActive | Editor-world mutator refused during PIE |
| `-32028` | LevelNotStreamingEntry | Target loaded but not in GetStreamingLevels() |
| `-32029` | WorldPartitionNotSupported | Phase 3 tool can't work on WP maps |
| `-32030` | OperationCancelled | Async job cancelled before completion |

## Blueprint specifics

| Code | Name | When |
|---|---|---|
| `-32031` | BlueprintTypeMismatch | Asset loaded but isn't a UBlueprint |
| `-32032` | PinTypeUnsupported | Pin type JSON unsupported by FMCPPinTypeUtils |
| `-32034` | KismetCompileFailed | bp.compile reported FBPCompileResults errors |
| `-32035` | FunctionNotFound | bp.* function name doesn't exist on BP |
| `-32036` | VariableAlreadyExists | bp.add_variable: name collision |
| `-32037` | VariableNotFound | bp.set_variable_default: name not in NewVariables |

## Sequencer

| Code | Name | When |
|---|---|---|
| `-32040` | NiagaraParameterNotFound | niagara.set_user_param: name not in store |
| `-32042` | NoActiveSequencer | sequencer.get_current_time: no Sequencer tab |
| `-32043` | TrackNotFound | sequencer track_path doesn't resolve |
| `-32044` | SectionIndexOOB | Section index out of range |

## Source Control + Test + Config + Logs + LiveCoding

| Code | Name | When |
|---|---|---|
| `-32045` | SourceControlProviderUnavailable | No provider plugin loaded |
| `-32046` | TestNotFound | test.run target test ID not registered |
| `-32047` | CVarReadOnly | Console variable has read-only flag |
| `-32048` | LiveCodingDisabled | LiveCoding module disabled or not Win64 |
| `-32049` | LogCategoryUnknown | logs.* category name not in GLog dispatch |

## Wave B/C/D/E/F additions

| Code | Name | When | Wave |
|---|---|---|---|
| `-32050` | GraphNotFound | bp.* graph_name not in UbergraphPages/FunctionGraphs/MacroGraphs | Wave B Tier 4 |
| `-32051` | NodeNotFound | bp.* node_guid not in target graph | Wave B Tier 4 |
| `-32052` | PinNotFound | bp.* pin_name not on target node | Wave B Tier 4 |
| `-32053` | PinConnectionRefused | UEdGraphSchema_K2 rejected the connection | Wave B Tier 4 |
| `-32054` | SkeletonMismatch | anim.create_montage: skeleton mismatch | Wave C Tier 5b |
| `-32055` | NotifyTrackNotFound | anim.add_notify: notify_track_name doesn't exist | Wave C Tier 5b |
| `-32056` | FolderNotFound | folder.delete: folder_path doesn't exist | Wave D Surface 4 |
| `-32057` | FunctionParameterDuplicate | bp.add_function_parameter: param_name already exists | Wave F S2 |
| `-32058` | OperationFailed | Generic "engine-side operation refused" — used when an engine API returned a recognized failure indicator (status enum, false bool with no diagnostic) that isn't covered by a more specific code. Currently raised by `ai.eqs.run_query` when `EEnvQueryStatus` is Failed/Aborted/OwnerLost/MissingParam. | Refactor Phase 4 |

## Recovery patterns

Most error codes carry a hint in the `message` field — verify before any complex retry logic:

```python
r = call("actor.spawn", {"class_path": "/Game/MyBP_PlayerCharacter"})
if not r["ok"]:
    code = r["error"]["code"]
    if code == -32004:
        # Asset not loaded yet — preload then retry
        call("cb.load_asset", {"asset_path": "/Game/MyBP_PlayerCharacter"})
        r = call("actor.spawn", {"class_path": "/Game/MyBP_PlayerCharacter_C"})  # note _C suffix
    elif code == -32027:
        # PIE active — stop PIE first
        call("pie.stop")
        r = call("actor.spawn", {"class_path": "/Game/MyBP_PlayerCharacter_C"})
```
