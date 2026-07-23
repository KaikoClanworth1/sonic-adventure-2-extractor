"""Render a multi-object OBJ with a distinct color per object (front + iso)."""
import sys, os, math, colorsys
import bpy, mathutils

src, dst = sys.argv[1], sys.argv[2]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.wm.obj_import(filepath=src)
meshes = [o for o in bpy.data.objects if o.type == "MESH"]
if not meshes:
    print("no meshes"); sys.exit(1)

meshes.sort(key=lambda o: o.name)
for i, o in enumerate(meshes):
    h = (i * 0.61803398875) % 1.0
    r, g, b = colorsys.hsv_to_rgb(h, 0.65, 0.95)
    m = bpy.data.materials.new(o.name)
    m.diffuse_color = (r, g, b, 1)
    o.data.materials.clear(); o.data.materials.append(m)

lo = mathutils.Vector((1e30,)*3); hi = mathutils.Vector((-1e30,)*3)
for o in meshes:
    for c in o.bound_box:
        w = o.matrix_world @ mathutils.Vector(c)
        for i in range(3): lo[i]=min(lo[i],w[i]); hi[i]=max(hi[i],w[i])
ctr=(lo+hi)*0.5; radius=max((hi-lo).length*0.5,1e-3)

sc = bpy.context.scene
sc.render.engine = "BLENDER_WORKBENCH"
sc.render.resolution_x = 520; sc.render.resolution_y = 720
sc.display.shading.light = "STUDIO"
sc.display.shading.color_type = "MATERIAL"
light = bpy.data.objects.new("k", bpy.data.lights.new("k", type="SUN"))
light.data.energy = 2.5; light.rotation_euler = (math.radians(50), 0, math.radians(30))
sc.collection.objects.link(light)
cam_d = bpy.data.cameras.new("c"); cam = bpy.data.objects.new("c", cam_d)
sc.collection.objects.link(cam); sc.camera = cam; cam_d.type = "ORTHO"; cam_d.ortho_scale = radius*2.3
views = {"front": (0,-1,0.05), "iso": (0.8,-1,0.35)}
for name, dv in views.items():
    d = mathutils.Vector(dv).normalized()*radius*3
    cam.location = ctr+d
    cam.rotation_euler = (ctr-cam.location).to_track_quat("-Z","Z").to_euler()
    b,e = os.path.splitext(dst); sc.render.filepath = os.path.abspath(f"{b}_{name}{e}")
    bpy.ops.render.render(write_still=True); print("wrote", sc.render.filepath)
