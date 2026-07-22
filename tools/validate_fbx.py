"""Validate exported FBX files by importing them into headless Blender (bpy).

    pip install bpy      (needs CPython 3.11)
    python tools/validate_fbx.py <file.fbx> [more.fbx ...]

Checks, per file:
  * import succeeds
  * mesh objects exist, with polygons and loops
  * UV layers present and inside a sane range
  * materials and image textures wired up
  * an armature exists with bones
  * vertex groups (skin weights) exist and reference real bones
  * actions (animations) exist with F-Curves and keyframes
Exit code 0 only when every file passes every applicable check.
"""
import sys, os, math

try:
    import bpy
except ImportError:
    print("ERROR: bpy is not installed. Run: pip install bpy")
    sys.exit(3)


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def validate(path):
    print("=" * 74)
    print("FBX:", path)
    print("=" * 74)
    problems = []
    reset()
    try:
        bpy.ops.import_scene.fbx(filepath=path)
    except Exception as e:
        print("  IMPORT FAILED:", e)
        return ["import failed: %s" % e]

    objs = list(bpy.data.objects)
    meshes = [o for o in objs if o.type == "MESH"]
    armatures = [o for o in objs if o.type == "ARMATURE"]
    print(f"  objects        : {len(objs)}")
    print(f"  mesh objects   : {len(meshes)}")
    print(f"  armatures      : {len(armatures)}")

    if not meshes:
        problems.append("no mesh objects")

    total_verts = total_polys = total_loops = 0
    uv_ok = 0
    uv_range_bad = 0
    with_groups = 0
    for o in meshes:
        me = o.data
        total_verts += len(me.vertices)
        total_polys += len(me.polygons)
        total_loops += len(me.loops)
        if me.uv_layers:
            uv_ok += 1
            uv = me.uv_layers[0].data
            for i in range(0, len(uv), max(1, len(uv) // 200)):
                u, v = uv[i].uv
                # Tiled stage geometry legitimately uses large UVs (a long road
                # with a repeating texture), so only non-finite / absurd values
                # indicate a real export bug.
                if not (math.isfinite(u) and math.isfinite(v)) or abs(u) > 1e5 or abs(v) > 1e5:
                    uv_range_bad += 1
                    break
        if o.vertex_groups:
            with_groups += 1
    print(f"  vertices       : {total_verts:,}")
    print(f"  polygons       : {total_polys:,}")
    print(f"  loops          : {total_loops:,}")
    print(f"  meshes w/ UVs  : {uv_ok}/{len(meshes)}")
    print(f"  meshes w/ vgrp : {with_groups}/{len(meshes)}")

    if total_polys == 0:
        problems.append("no polygons")
    if meshes and uv_ok == 0:
        problems.append("no UV layers on any mesh")
    if uv_range_bad:
        problems.append(f"{uv_range_bad} meshes have out-of-range UVs")

    mats = list(bpy.data.materials)
    imgs = [i for i in bpy.data.images if i.name != "Render Result"]
    print(f"  materials      : {len(mats)}")
    print(f"  images         : {len(imgs)}")
    for im in imgs[:5]:
        print(f"     - {im.name} {tuple(im.size)}")
    if not mats:
        problems.append("no materials")

    bones = 0
    for a in armatures:
        bones += len(a.data.bones)
    print(f"  bones          : {bones}")
    if armatures and bones == 0:
        problems.append("armature has no bones")

    # skin weights must reference bones that exist
    if armatures and with_groups:
        bone_names = set()
        for a in armatures:
            bone_names |= {b.name for b in a.data.bones}
        dangling = 0
        for o in meshes:
            for g in o.vertex_groups:
                if g.name not in bone_names:
                    dangling += 1
        print(f"  dangling vgrps : {dangling}")
        if dangling:
            problems.append(f"{dangling} vertex groups do not match any bone")

    def action_fcurves(a):
        """Blender 4.4+ moved F-Curves into layers/strips/channelbags."""
        if hasattr(a, "fcurves"):
            return list(a.fcurves)
        out = []
        for layer in getattr(a, "layers", []):
            for strip in getattr(layer, "strips", []):
                for cb in getattr(strip, "channelbags", []):
                    out.extend(cb.fcurves)
        return out

    acts = list(bpy.data.actions)
    total_fc = total_keys = 0
    for a in acts:
        fcs = action_fcurves(a)
        total_fc += len(fcs)
        for fc in fcs:
            total_keys += len(fc.keyframe_points)
    print(f"  actions        : {len(acts)}")
    print(f"  fcurves        : {total_fc}")
    print(f"  keyframes      : {total_keys:,}")
    for a in acts[:5]:
        rng = a.frame_range
        print(f"     - {a.name}: {len(action_fcurves(a))} curves, "
              f"frames {rng[0]:.0f}..{rng[1]:.0f}")
    if acts and total_keys == 0:
        problems.append("actions exist but contain no keyframes")

    if problems:
        print("  RESULT: FAIL")
        for p in problems:
            print("    !", p)
    else:
        print("  RESULT: PASS")
    return problems


def main():
    files = sys.argv[1:]
    if not files:
        print("usage: validate_fbx.py <file.fbx> ...")
        return 2
    bad = 0
    for f in files:
        if not os.path.exists(f):
            print("missing:", f)
            bad += 1
            continue
        if validate(f):
            bad += 1
        print()
    print(f"{len(files) - bad}/{len(files)} files passed")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
