"""Build a stage's REAL object layout: every SET-placed object, as its actual
model, at its actual position/rotation. Uses the stage->id->{name,model} table
from objmap.py plus the stage's setNNNN_s.bin.

    setscene.py <exe> <sa2_objects.json> <setNNNN_s.bin> <stage> <out.obj>
"""
import sys, os, json, struct, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pe as pemod
import exemodel as em

exe, mapjson, setfile, stage, out = sys.argv[1:6]
pe = pemod.PE(open(exe, "rb").read())
b = em.LE(pe.flat_image(), pe.image_base)
table = json.load(open(mapjson))[str(int(stage))]
objs = {o["id"]: o for o in table["objects"]}

d = open(setfile, "rb").read()
count = struct.unpack_from(">I", d, 0)[0]


def bams(v):
    return v * math.pi / 0x8000


verts, faces = [], []
placed, missing = 0, {}
cache = {}
for i in range(count):
    o = 0x20 + i * 0x20
    if o + 0x20 > len(d):
        break
    oid = struct.unpack_from(">H", d, o)[0] & 0xFFF
    rx, ry, rz = (struct.unpack_from(">h", d, o + 2 + k * 2)[0] for k in range(3))
    px, py, pz = (struct.unpack_from(">f", d, o + 8 + k * 4)[0] for k in range(3))
    ent = objs.get(oid)
    if not ent or not ent["model"]:
        missing[ent["name"] if ent else f"id{oid}"] = missing.get(
            ent["name"] if ent else f"id{oid}", 0) + 1
        continue
    root = ent["model"]
    if root not in cache:
        try:
            _, vs, ts = em.build(b, root)
        except Exception:
            vs, ts = [], []
        cache[root] = (vs, ts)
    vs, ts = cache[root]
    if not vs:
        continue
    # SA2 rotation order ZXY, angles are BAMS
    ax, ay, az = bams(rx), bams(ry), bams(rz)
    ca, sa = math.cos(ax), math.sin(ax)
    cb, sb = math.cos(ay), math.sin(ay)
    cc, sc = math.cos(az), math.sin(az)
    base_i = len(verts)
    for (vx, vy, vz) in vs:
        # Z, then X, then Y
        x1, y1, z1 = vx * cc - vy * sc, vx * sc + vy * cc, vz
        x2, y2, z2 = x1, y1 * ca - z1 * sa, y1 * sa + z1 * ca
        x3, y3, z3 = x2 * cb + z2 * sb, y2, -x2 * sb + z2 * cb
        verts.append((x3 + px, y3 + py, z3 + pz))
    for (a, bb, c) in ts:
        faces.append((base_i + a, base_i + bb, base_i + c))
    placed += 1

with open(out, "w") as fh:
    for v in verts:
        fh.write(f"v {v[0]:.3f} {v[1]:.3f} {v[2]:.3f}\n")
    for f in faces:
        fh.write(f"f {f[0]+1} {f[1]+1} {f[2]+1}\n")
print(f"{count} SET entries: placed {placed} real object models "
      f"({len(verts)} verts, {len(faces)} tris) -> {out}")
top = sorted(missing.items(), key=lambda kv: -kv[1])[:8]
print(f"no model for: {top}")
