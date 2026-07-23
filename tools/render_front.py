"""Render an FBX from front + side + 3/4, tiled, for inspecting character models."""
import sys, os, math
import bpy, mathutils

src, dst = sys.argv[1], sys.argv[2]
bpy.ops.wm.read_factory_settings(use_empty=True)
try:
    bpy.ops.wm.obj_import(filepath=src) if src.lower().endswith(".obj") else \
        bpy.ops.import_scene.fbx(filepath=src)
except Exception:
    bpy.ops.import_scene.fbx(filepath=src)

meshes = [o for o in bpy.data.objects if o.type == "MESH"]
if not meshes:
    print("no meshes"); sys.exit(1)

lo = mathutils.Vector((1e30,) * 3); hi = mathutils.Vector((-1e30,) * 3)
for o in meshes:
    for c in o.bound_box:
        w = o.matrix_world @ mathutils.Vector(c)
        for i in range(3):
            lo[i] = min(lo[i], w[i]); hi[i] = max(hi[i], w[i])
ctr = (lo + hi) * 0.5
radius = max((hi - lo).length * 0.5, 1e-3)
print("bounds lo", tuple(round(v, 2) for v in lo), "hi", tuple(round(v, 2) for v in hi))
print("size", tuple(round(hi[i]-lo[i],2) for i in range(3)))

scene = bpy.context.scene
scene.render.engine = "BLENDER_WORKBENCH"
scene.render.resolution_x = 500
scene.render.resolution_y = 700
scene.display.shading.light = "STUDIO"
scene.display.shading.color_type = "TEXTURE"

light = bpy.data.objects.new("k", bpy.data.lights.new("k", type="SUN"))
light.data.energy = 3; light.rotation_euler = (math.radians(50), 0, math.radians(30))
scene.collection.objects.link(light)

cam_data = bpy.data.cameras.new("c"); cam = bpy.data.objects.new("c", cam_data)
scene.collection.objects.link(cam); scene.camera = cam
cam_data.type = "ORTHO"; cam_data.ortho_scale = radius * 2.2

views = {"front": (0, -1, 0), "side": (1, 0, 0), "iso": (0.8, -0.8, 0.4)}
outs = []
for name, dirv in views.items():
    d = mathutils.Vector(dirv).normalized() * radius * 3
    cam.location = ctr + d
    cam.rotation_euler = (ctr - cam.location).to_track_quat("-Z", "Z").to_euler()
    base, ext = os.path.splitext(dst)
    p = f"{base}_{name}{ext}"
    scene.render.filepath = os.path.abspath(p)
    bpy.ops.render.render(write_still=True)
    outs.append(p)
    print("wrote", p)
