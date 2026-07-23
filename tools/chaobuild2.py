"""Build one Chao mesh: vertex block (pos+normal) followed by a u16 triangle-list
index block. Proves the [vertices][indices] mesh layout by rendering a solid."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress

d = bytes(prs_decompress(open(sys.argv[1], "rb").read()))
n = len(d)
f32 = lambda o: struct.unpack_from(">f", d, o)[0]
be16 = lambda o: struct.unpack_from(">H", d, o)[0]

VBLK = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x98c8
IEND = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x10000

# read the vertex block (interleaved pos+normal, unit normal)
verts = []
o = VBLK
while o + 24 <= n:
    nx, ny, nz = f32(o + 12), f32(o + 16), f32(o + 20)
    px, py, pz = f32(o), f32(o + 4), f32(o + 8)
    if 0.9 < nx*nx + ny*ny + nz*nz < 1.1 and all(abs(v) < 3000 for v in (px, py, pz)):
        verts.append((px, py, pz)); o += 24
    else:
        break
vcount = len(verts)
vend = o
print(f"vertex block 0x{VBLK:x}: {vcount} verts, ends 0x{vend:x}")

# Index data is a run of triangle groups: {s16 triCount, |triCount|*3 u16};
# negative count = reversed winding (like Ninja strips).
be16s = lambda o: struct.unpack_from(">h", d, o)[0]
ISTART = int(sys.argv[4], 0) if len(sys.argv) > 4 else vend
tris = []
o = ISTART
groups = 0
while o + 2 <= min(IEND, n):
    sc = be16s(o); o += 2
    cnt = abs(sc)
    if cnt == 0 or cnt > 4096 or o + cnt * 6 > n:
        break
    ok = True
    grp = []
    for t in range(cnt):
        a, b, c = be16(o), be16(o + 2), be16(o + 4); o += 6
        if a >= vcount or b >= vcount or c >= vcount: ok = False; break
        if sc < 0: a, b = b, a
        if len({a, b, c}) == 3: grp.append((a, b, c))
    if not ok: break
    tris += grp; groups += 1
print(f"triangles: {len(tris)} in {groups} groups, ends 0x{o:x}")

with open(r"F:\ClaudeProjects\Sonic Adventure 2\build\chao_m.obj", "w") as fh:
    for (x, y, z) in verts:
        fh.write(f"v {x:.3f} {y:.3f} {z:.3f}\n")
    for a, b, c in tris:
        fh.write(f"f {a+1} {b+1} {c+1}\n")
print("wrote build/chao_m.obj")
