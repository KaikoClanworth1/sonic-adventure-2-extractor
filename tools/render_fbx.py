"""Render an exported FBX with headless Blender - independent visual check.

    python tools/render_fbx.py <in.fbx> <out.png>
"""
import sys, os, math

import bpy
import mathutils


def main():
    src, dst = sys.argv[1], sys.argv[2]
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=src)

    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    if not meshes:
        print("no meshes")
        return 1

    # world-space bounds over every mesh
    lo = mathutils.Vector((1e30,) * 3)
    hi = mathutils.Vector((-1e30,) * 3)
    for o in meshes:
        for c in o.bound_box:
            w = o.matrix_world @ mathutils.Vector(c)
            for i in range(3):
                lo[i] = min(lo[i], w[i])
                hi[i] = max(hi[i], w[i])
    ctr = (lo + hi) * 0.5
    radius = max((hi - lo).length * 0.5, 1e-3)

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.render.resolution_x = 900
    scene.render.resolution_y = 900
    scene.render.film_transparent = False
    scene.render.filepath = os.path.abspath(dst)
    scene.render.image_settings.file_format = "PNG"
    try:
        scene.display.shading.light = "STUDIO"
        scene.display.shading.color_type = "TEXTURE"
    except Exception:
        pass

    cam_data = bpy.data.cameras.new("cam")
    cam = bpy.data.objects.new("cam", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam

    # 3/4 view, framed on the bounds
    d = radius * 2.6
    ang = math.radians(35)
    cam.location = (ctr.x + d * math.cos(ang), ctr.y - d * math.sin(ang),
                    ctr.z + radius * 0.55)
    direction = ctr - cam.location
    cam.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
    cam_data.clip_start = radius * 0.005
    cam_data.clip_end = radius * 40

    light_data = bpy.data.lights.new("key", type="SUN")
    light_data.energy = 4.0
    light = bpy.data.objects.new("key", light_data)
    light.rotation_euler = (math.radians(50), 0, math.radians(35))
    scene.collection.objects.link(light)

    bpy.ops.render.render(write_still=True)
    print("bounds", tuple(round(v, 2) for v in lo), tuple(round(v, 2) for v in hi))
    print("wrote", scene.render.filepath)
    return 0


if __name__ == "__main__":
    sys.exit(main())
