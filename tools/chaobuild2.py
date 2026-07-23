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

# index data starts after the per-mesh header; find the first run of >=6 valid
# in-range u16 (that's the strip), collect the u16s until a run of invalids.
ISTART = int(sys.argv[4], 0) if len(sys.argv) > 4 else vend
idx = []
o = ISTART
bad = 0
while o + 2 <= min(IEND, n):
    v = be16(o)
    if v < vcount:
        idx.append(v); bad = 0
    else:
        idx.append(-1); bad += 1
        if bad > 8 and len(idx) > 30: break   # end of this strip block
    o += 2
# triangle STRIP: -1 marks a restart; skip degenerate triangles; flip odd winding
tris = []
run = []
for v in idx + [-1]:
    if v < 0:
        for k in range(len(run) - 2):
            a, b, c = run[k], run[k+1], run[k+2]
            if k % 2 == 1: a, b = b, a
            if len({a, b, c}) == 3: tris.append((a, b, c))
        run = []
    else:
        run.append(v)
print(f"triangles (strip interp): {len(tris)} from {len(idx)} u16 at 0x{ISTART:x}")

with open(r"F:\ClaudeProjects\Sonic Adventure 2\build\chao_m.obj", "w") as fh:
    for (x, y, z) in verts:
        fh.write(f"v {x:.3f} {y:.3f} {z:.3f}\n")
    for a, b, c in tris:
        fh.write(f"f {a+1} {b+1} {c+1}\n")
print("wrote build/chao_m.obj")
