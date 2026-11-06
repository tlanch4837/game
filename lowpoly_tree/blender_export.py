"""Run: blender -b -P blender_export.py
Converts out/obj/<Name>_LOD0/LOD1.obj -> out/cs2/<Name>/<Name>_LOD0.fbx / _LOD1.fbx
(CS2 Asset Importer layout: one folder per asset, LOD0 = highest detail.)"""
import bpy, pathlib

ROOT = pathlib.Path(__file__).parent / "out"
for obj in sorted((ROOT / "obj").glob("*_LOD*.obj")):
    name, lod = obj.stem.rsplit("_", 1)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.wm.obj_import(filepath=str(obj), forward_axis='NEGATIVE_Z', up_axis='Y')
    for o in bpy.context.scene.objects:
        o.name = o.data.name = f"{name}_{lod}"
        for m in o.data.materials:
            if m: m.name = name
    out = ROOT / "cs2" / name / f"{name}_{lod}.fbx"
    bpy.ops.export_scene.fbx(filepath=str(out), use_selection=False, apply_scale_options='FBX_SCALE_UNITS',
                             axis_forward='-Z', axis_up='Y', mesh_smooth_type='OFF', path_mode='STRIP',
                             embed_textures=False, use_mesh_modifiers=True, bake_anim=False)
    print("wrote", out)
