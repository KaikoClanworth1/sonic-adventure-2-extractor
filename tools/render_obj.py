"""Render an OBJ with headless Blender from a few angles."""
import sys, os, math
import bpy, mathutils

src, dst = sys.argv[1], sys.argv[2]
bpy.ops.wm.read_factory_settings(use_empty=True)
# Blender 4.x has a built-in obj importer op
try:
    bpy.ops.wm.obj_import(filepath=src)
except Exception:
    bpy.ops.import_scene.obj(filepath=src)

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
print("bounds", tuple(round(v, 1) for v in lo), tuple(round(v, 1) for v in hi))

scene = bpy.context.scene
scene.render.engine = "BLENDER_WORKBENCH"
scene.render.resolution_x = 1100
scene.render.resolution_y = 800
scene.render.filepath = os.path.abspath(dst)
scene.display.shading.light = "MATCAP"
scene.display.shading.single_color = (0.7, 0.7, 0.72)
scene.display.shading.color_type = "SINGLE"

cam_data = bpy.data.cameras.new("cam")
cam = bpy.data.objects.new("cam", cam_data)
scene.collection.objects.link(cam); scene.camera = cam
d = radius * 2.2
ang = math.radians(40)
cam.location = (ctr.x + d * math.cos(ang), ctr.y - d * math.sin(ang), ctr.z + radius * 0.7)
cam.rotation_euler = (ctr - cam.location).to_track_quat("-Z", "Y").to_euler()
cam_data.clip_start = radius * 0.001
cam_data.clip_end = radius * 30
cam_data.lens = 24

light = bpy.data.objects.new("key", bpy.data.lights.new("key", type="SUN"))
light.data.energy = 3.0
light.rotation_euler = (math.radians(55), 0, math.radians(40))
scene.collection.objects.link(light)

bpy.ops.render.render(write_still=True)
print("wrote", scene.render.filepath)
