---
title: bp.* (Blueprint)
layout: default
parent: Tools reference
nav_order: 1
---

# bp.* — Blueprint authoring (49 tools)
{: .no_toc }

After Wave F, the `bp.*` namespace covers **full Blueprint CRUD without opening the editor**: assets, compilation, variables, functions, graph nodes, SCS components, interfaces, metadata, comments, UFUNCTION invocation, introspection.

1. TOC
{:toc}

## Asset lifecycle

### `bp.create_blueprint`
Create a new UBlueprint asset.

| Arg | Type | Required | Notes |
|---|---|---|---|
| `dest_path` | string | yes | e.g. `/Game/MyBP_PlayerCharacter` |
| `parent_class_path` | string | yes | e.g. `/Script/Engine.Actor` |
| `save` | bool | no | If true, save to disk immediately |

Returns: `{ created, asset_path }`. PIE-guarded. `-32014` if path exists.

### `bp.compile`
Run `FBlueprintCompilationManager::CompileSynchronously`.

| Arg | Type | Required |
|---|---|---|
| `blueprint_path` | string | yes |

Returns: `{ compiled, status: "UpToDate"|"Error"|"Warning", errors: [...], warnings: [...] }`.

### `bp.compile_all_dirty`
Compile every dirty UBlueprint in `/Game`. Returns batch summary.

## Variables

### `bp.add_variable`
| Arg | Required | Notes |
|---|---|---|
| `blueprint_path` | yes | |
| `variable_name` | yes | |
| `pin_type` | yes | `{ category, subcategory?, container?, is_reference? }` |
| `default_value` | no | String representation |

Returns: `{ added, variable_index }`. `-32036` on name collision.

### `bp.set_variable_default`
Mutate the default value (also goes through CDO update).

### `bp.set_variable_metadata` ⭐ Wave F5
Mutate variable metadata (Category, Tooltip, EditAnywhere, ExposeOnSpawn, Replicate, SaveGame, Transient).

| Arg | Type | Notes |
|---|---|---|
| `variable_name` | string | |
| `metadata` | object | At least one field — see below |
| `metadata.category` | string | |
| `metadata.tooltip` | string | |
| `metadata.edit_anywhere` | bool | Sets CPF_Edit + clears CPF_DisableEditOnInstance |
| `metadata.expose_on_spawn` | bool | Dual-tracked (CPF flag + MetaData key) |
| `metadata.replicate` | string | `"none"\|"replicated"\|"rep_notify"` |
| `metadata.rep_notify_function` | string | Required if replicate=rep_notify |
| `metadata.save_game` | bool | CPF_SaveGame |
| `metadata.transient` | bool | CPF_Transient |

Returns: `{ prior, new }` diff. Triggers structural recompile.

### `bp.list_variables`
Enumerate `Blueprint->NewVariables`. Returns `{ variables: [{ name, type, default_value, category? }] }`.

### `bp.get_variable`
Read full FBPVariableDescription.

### `bp.list_categories` ⭐ Wave F5
Enumerate unique Category values across NewVariables + function metadata.

## Functions

### `bp.add_function`
Create a new function graph. Returns `{ function_name, graph_name }`.

### `bp.delete_function` / `bp.rename_function`

### `bp.add_function_parameter` ⭐ Wave F2
| Arg | Required | Notes |
|---|---|---|
| `function_name` | yes | |
| `param_name` | yes | |
| `pin_type` | yes | `{ category, subcategory?, ... }` |
| `direction` | yes | `"input"\|"output"` |
| `default_value` | no | Only meaningful for inputs |

Mechanic: input params → `K2Node_FunctionEntry.UserDefinedPins` (rendered as OUTPUT pins on Entry). Output params → `K2Node_FunctionResult.UserDefinedPins` (rendered as INPUT pins on Result). Lazy Result node creation if first output.

Returns: `{ added, param_name, direction }`. `-32057` on duplicate name.

### `bp.remove_function_parameter` ⭐ Wave F2

### `bp.list_function_parameters` ⭐ Wave F2
Returns `{ inputs: [...], outputs: [...] }`.

### `bp.set_function_metadata` ⭐ Wave F2
| `metadata.is_pure` | bool | FUNC_BlueprintPure |
| `metadata.is_const` | bool | FUNC_Const |
| `metadata.category` | string | |
| `metadata.access_specifier` | string | `"public"\|"protected"\|"private"` |
| `metadata.call_in_editor` | bool | |
| `metadata.tooltip` | string | |

### `bp.list_functions`
Enumerate Blueprint->FunctionGraphs.

### `bp.get_function`
Read function summary (params, locals, node count).

### `bp.call_function`
Invoke UFUNCTION via reflection. Args are typed JSON marshalled via FMCPMarshalling. Returns `{ return_value? }`.

### `bp.list_class_functions`
Reflect functions on the generated class (includes inherited).

## Events

### `bp.add_event` / `bp.delete_event`

## Graph nodes (Wave B Tier 4 + Wave F1)

### `bp.add_node`
Spawn a K2Node subclass on a graph.

| Arg | Required | Notes |
|---|---|---|
| `blueprint_path` | yes | |
| `node_class` | yes | e.g. `/Script/BlueprintGraph.K2Node_VariableGet` |
| `graph_name` | no | Default `"EventGraph"` |
| `position` | no | `[x, y]`, default `[0, 0]` |
| `variable_name` | no | For K2Node_Variable* — self member |
| `function_name` | no | For K2Node_CallFunction |
| `function_class` | no | For K2Node_CallFunction (self if omitted) |
| `event_name` | no | For K2Node_CustomEvent |

Returns: `{ node_guid, node_class, position, pins: [...] }`.

### `bp.connect_pins`
Wire two pins via `UEdGraphSchema_K2::TryCreateConnection`. Returns `{ connected, broke_existing_count, response }`.

### `bp.set_node_property` ⭐ Wave F1
Write a UPROPERTY on a K2Node by name. Uses `FMCPReflection::WritePropertyValueAt` + `FMCPWritePropertyScope` (4-step PreEdit/Modify/write/PostEdit). Auto `ReconstructNode` for property-driven pin layout changes.

| Arg | Required | Notes |
|---|---|---|
| `node_guid` | yes | |
| `property_name` | yes | |
| `value` | yes | Typed JSON (number/string/array/object — marshalled per property type) |

### `bp.set_pin_default` ⭐ Wave F1
Set DefaultValue/DefaultObject on an UNCONNECTED pin. Routes to schema's `TrySetDefaultObject` (object refs) or `TrySetDefaultValue` (primitives).

Refuses connected pins → `-32602`.

### `bp.delete_node` ⭐ Wave F1
Refuses K2Node_FunctionEntry / K2Node_FunctionResult / K2Node_Event (CustomEvent OK). Auto-breaks pin links via `Graph->RemoveNode`.

### `bp.disconnect_pin` ⭐ Wave F1
`Pin->BreakAllPinLinks()`. Returns `{ links_broken }`.

### `bp.move_node` ⭐ Wave F1
Reposition (NodePosX/NodePosY).

### `bp.list_nodes_in_function`
Walk Graph->Nodes (paginated). Returns `{ nodes: [{ guid, class, title, pins: [...] }] }`.

## Components — SimpleConstructionScript (Wave F3)

### `bp.add_component` ⭐ Wave F3
| Arg | Required | Notes |
|---|---|---|
| `component_class_path` | yes | Must derive UActorComponent |
| `variable_name` | yes | |
| `parent_component` | no | Default `"DefaultSceneRoot"` (scene components) or root (non-scene) |

Returns: `{ added, variable_name, component_class, parent }`. `-32014` on name collision.

### `bp.remove_component` ⭐ Wave F3
Re-parents children to deleted node's parent. Returns `{ removed, reparented_children_count }`.

### `bp.list_components` ⭐ Wave F3
Walk SCS tree. `recursive` default true. Returns `{ root, components: [{ variable_name, component_class, parent_variable_name?, attach_socket? }] }`.

### `bp.set_component_default` ⭐ Wave F3
Mutate ComponentTemplate (CDO archetype) UPROPERTY. Edit-const gate intentionally OMITTED (SCS templates are the authoring surface).

## Interfaces (Wave F4)

### `bp.add_interface` ⭐ Wave F4
Implement BP interface via `FBlueprintEditorUtils::ImplementNewInterface`. Returns `{ added, interface_class, generated_event_count }`.

### `bp.remove_interface` ⭐ Wave F4
`FBlueprintEditorUtils::RemoveInterface` with `bPreserveFunctions=false`.

### `bp.list_interfaces` ⭐ Wave F4
| `include_parent_interfaces` | bool | Default true |

Returns `{ implemented_interfaces: [{ interface_class, source: "blueprint"|"parent" }] }`.

## Comments (Wave F5)

### `bp.add_comment` ⭐ Wave F5
Create a UEdGraphNode_Comment.

| Arg | Required | Default |
|---|---|---|
| `position` | yes | |
| `text` | yes | |
| `graph_name` | no | `"EventGraph"` |
| `size` | no | `[300, 200]` |
| `color` | no | `[0.5, 0.5, 0.5, 1.0]` mid-grey |

Returns: `{ node_guid, position, size }`.

### `bp.delete_comment` ⭐ Wave F5
Cast guard prevents accidental K2 node deletion through this path → `-32051` if Guid points to a non-comment node.

## Introspection

### `bp.list_blueprints`
Paginated enumeration of `/Game` UBlueprint assets.

### `bp.get_summary` / `bp.get_class_summary`
Full BP / generated class summary.

## End-to-end example

```python
BP = "/Game/Demo/BP_Char"

# Create + compile
call("bp.create_blueprint", {"dest_path": BP, "parent_class_path": "/Script/Engine.Pawn"})

# Variables
call("bp.add_variable", {"blueprint_path": BP, "variable_name": "Health",
                        "pin_type": {"category": "Real", "subcategory": "double"},
                        "default_value": "100.0"})
call("bp.set_variable_metadata", {"blueprint_path": BP, "variable_name": "Health",
                                  "metadata": {"category": "Stats", "edit_anywhere": True,
                                               "replicate": "rep_notify",
                                               "rep_notify_function": "OnRep_Health"}})

# Function with params
call("bp.add_function", {"blueprint_path": BP, "function_name": "TakeDamage"})
call("bp.add_function_parameter", {"blueprint_path": BP, "function_name": "TakeDamage",
                                   "param_name": "Amount", "direction": "input",
                                   "pin_type": {"category": "Real", "subcategory": "double"}})
call("bp.set_function_metadata", {"blueprint_path": BP, "function_name": "TakeDamage",
                                  "metadata": {"category": "Combat",
                                               "tooltip": "Apply damage and replicate"}})

# Components
call("bp.add_component", {"blueprint_path": BP,
                          "component_class_path": "/Script/Engine.SkeletalMeshComponent",
                          "variable_name": "MyMesh",
                          "parent_component": "DefaultSceneRoot"})
call("bp.set_component_default", {"blueprint_path": BP, "variable_name": "MyMesh",
                                  "property_name": "bReceivesDecals", "value": False})

# Implement an interface
call("bp.add_interface", {"blueprint_path": BP,
                          "interface_class_path": "/Script/Engine.ActorSoundParameterInterface"})

# Add a graph comment
call("bp.add_comment", {"blueprint_path": BP, "graph_name": "EventGraph",
                        "position": [0, 0], "size": [600, 400],
                        "text": "Combat logic — Wave F demo",
                        "color": [0.3, 0.7, 1.0, 0.5]})

# Compile + verify
r = call("bp.compile", {"blueprint_path": BP})
assert r["result"]["status"] in ("UpToDate", "BlueprintErrorsAndOrWarnings")
print(f"BP compiled: {r['result']['compiled']}")
```
