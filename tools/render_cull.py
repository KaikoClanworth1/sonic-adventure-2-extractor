"""Render an OBJ with backface culling ON (mimics the viewer) to find holes."""
import sys, os, math
import bpy, mathutils

src, dst = sys.argv[1], sys.argv[2]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.wm.obj_import(filepath=src)
meshes = [o for o in bpy.data.objects if o.type == "MESH"]
if not meshes:
    print("no meshes"); sys.exit(1)

# one material with backface culling
mat = bpy.data.materials.new("cull")
mat.use_backface_culling = True
mat.diffuse_color = (0.7, 0.7, 0.72, 1)
for o in meshes:
    o.data.materials.clear()
    o.data.materials.append(mat)

lo = mathutils.Vector((1e30,)*3); hi = mathutils.Vector((-1e30,)*3)
for o in meshes:
    for c in o.bound_box:
        w = o.matrix_world @ mathutils.Vector(c)
        for i in range(3): lo[i]=min(lo[i],w[i]); hi[i]=max(hi[i],w[i])
ctr=(lo+hi)*0.5; radius=max((hi-lo).length*0.5,1e-3)

sc = bpy.context.scene
sc.render.engine = "BLENDER_WORKBENCH"
sc.render.resolution_x = 500; sc.render.resolution_y = 700
sc.display.shading.light = "STUDIO"
sc.display.shading.show_backface_culling = True
sc.display.shading.color_type = "MATERIAL"
light = bpy.data.objects.new("k", bpy.data.lights.new("k", type="SUN"))
light.data.energy = 3; light.rotation_euler = (math.radians(50), 0, math.radians(30))
sc.collection.objects.link(light)
cam_d = bpy.data.cameras.new("c"); cam = bpy.data.objects.new("c", cam_d)
sc.collection.objects.link(cam); sc.camera = cam; cam_d.type = "ORTHO"; cam_d.ortho_scale = radius*2.2
for name, dv in {"front": (0,-1,0), "back": (0,1,0)}.items():
    d = mathutils.Vector(dv).normalized()*radius*3
    cam.location = ctr+d
    cam.rotation_euler = (ctr-cam.location).to_track_quat("-Z","Z").to_euler()
    b,e = os.path.splitext(dst); sc.render.filepath = os.path.abspath(f"{b}_{name}{e}")
    bpy.ops.render.render(write_still=True); print("wrote", sc.render.filepath)
