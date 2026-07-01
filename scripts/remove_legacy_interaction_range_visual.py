import unreal


TARGET_BLUEPRINTS = [
    "/Game/BP/GameFrame/BP_PlayerAtom",
]

LEGACY_NAME_FRAGMENTS = (
    "ProximityVisual",
    "ProximityRange",
    "RangeVisual",
    "InteractionRangeVisual",
    "FusionDisplayCanvas",
)

PLANE_MESH_PATH = "/Engine/BasicShapes/Plane"


def get_subobject_data(handle):
    library = unreal.SubobjectDataBlueprintFunctionLibrary
    try:
        return library.get_data(handle)
    except TypeError:
        result = library.get_data(handle)
        if isinstance(result, tuple) and result:
            return result[-1]
        return result


def get_object_for_blueprint(data, blueprint):
    library = unreal.SubobjectDataBlueprintFunctionLibrary
    try:
        return library.get_object_for_blueprint(data, blueprint)
    except Exception:
        try:
            return library.get_associated_object(data)
        except Exception:
            return None


def should_delete_subobject(data, blueprint):
    library = unreal.SubobjectDataBlueprintFunctionLibrary
    variable_name = str(library.get_variable_name(data))
    display_name = str(library.get_display_name(data))
    candidate_name = variable_name or display_name

    if any(fragment in candidate_name for fragment in LEGACY_NAME_FRAGMENTS):
        return True

    obj = get_object_for_blueprint(data, blueprint)
    if not obj:
        return False

    if not isinstance(obj, unreal.StaticMeshComponent):
        return False

    static_mesh = obj.get_editor_property("static_mesh")
    mesh_path = static_mesh.get_path_name() if static_mesh else ""
    return PLANE_MESH_PATH in mesh_path and (
        candidate_name == "VisualMesh" or
        candidate_name == "FusionDisplayCanvas" or
        bool(library.can_delete(data))
    )


def main():
    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    if not subsystem:
        subsystem = unreal.get_editor_subsystem(unreal.SubobjectDataSubsystem)
    if not subsystem:
        raise RuntimeError("SubobjectDataSubsystem is unavailable.")

    total_deleted = 0
    for asset_path in TARGET_BLUEPRINTS:
        blueprint = unreal.EditorAssetLibrary.load_asset(asset_path)
        if not blueprint:
            raise RuntimeError(f"Cannot load blueprint: {asset_path}")

        handles = subsystem.k2_gather_subobject_data_for_blueprint(blueprint)
        if not handles:
            unreal.log_warning(f"[ChemicalBond] No subobject handles found for {asset_path}")
            continue

        context_handle = handles[0]
        handles_to_delete = []
        for handle in handles:
            data = get_subobject_data(handle)
            if not data:
                continue
            if should_delete_subobject(data, blueprint):
                handles_to_delete.append(handle)

        if handles_to_delete:
            deleted_count = subsystem.delete_subobjects(context_handle, handles_to_delete, blueprint)
            total_deleted += deleted_count
            unreal.log(f"[ChemicalBond] Deleted {deleted_count} legacy interaction range component(s) from {asset_path}.")
            unreal.EditorAssetLibrary.save_loaded_asset(blueprint)
        else:
            unreal.log(f"[ChemicalBond] No legacy interaction range component found in {asset_path}.")

    unreal.log(f"[ChemicalBond] Legacy interaction range component deletion complete. TotalDeleted={total_deleted}")


if __name__ == "__main__":
    main()
