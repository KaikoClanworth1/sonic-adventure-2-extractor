"""Chao stage .prs: the file is a packed sequence of vertex blocks (interleaved
pos+normal floats) and u16 index blocks. First proof: pull every vertex (a sane
position immediately followed by a unit-length normal) and dump a point cloud
to confirm the geometry decodes to the lobby's shape."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress

d = bytes(prs_decompress(open(sys.argv[1], "rb").read()))
n = len(d)
f32 = lambda o: struct.unpack_from(">f", d, o)[0]

verts = []
o = 0
while o + 24 <= n:
    px, py, pz = f32(o), f32(o + 4), f32(o + 8)
    nx, ny, nz = f32(o + 12), f32(o + 16), f32(o + 20)
    ln = nx * nx + ny * ny + nz * nz
    sane = all(abs(v) < 3000 and v == v for v in (px, py, pz))
    if sane and 0.95 < ln < 1.05:
        verts.append((px, py, pz))
        o += 24            # stride to the next interleaved vertex
    else:
        o += 4
print(f"vertices found: {len(verts)}")
if verts:
    xs = [v[0] for v in verts]; ys = [v[1] for v in verts]; zs = [v[2] for v in verts]
    print(f"bounds x[{min(xs):.0f},{max(xs):.0f}] "
          f"y[{min(ys):.0f},{max(ys):.0f}] z[{min(zs):.0f},{max(zs):.0f}]")
# emit each vertex as a tiny triangle so a solid renderer shows the cloud
s = 0.3
with open(r"F:\ClaudeProjects\Sonic Adventure 2\build\chao_pts.obj", "w") as fh:
    for (x, y, z) in verts:
        fh.write(f"v {x:.3f} {y:.3f} {z:.3f}\n")
        fh.write(f"v {x+s:.3f} {y:.3f} {z:.3f}\n")
        fh.write(f"v {x:.3f} {y+s:.3f} {z:.3f}\n")
    for i in range(len(verts)):
        b = i * 3 + 1
        fh.write(f"f {b} {b+1} {b+2}\n")
print("wrote build/chao_pts.obj")
