"""Map a Chao stage file: find every run of 24-byte pos+normal vertices, so we
see exactly where each mesh's vertex block is and what lies between them."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress

d = bytes(prs_decompress(open(sys.argv[1], "rb").read()))
n = len(d)
f32 = lambda o: struct.unpack_from(">f", d, o)[0]


def isvert(o):
    if o + 24 > n: return False
    nx, ny, nz = f32(o + 12), f32(o + 16), f32(o + 20)
    px, py, pz = f32(o), f32(o + 4), f32(o + 8)
    return 0.9 < nx*nx + ny*ny + nz*nz < 1.1 and all(abs(v) < 5000 for v in (px, py, pz))


o = 0
prev_end = 0
while o < n - 24:
    if isvert(o):
        start = o
        cnt = 0
        while isvert(o):
            o += 24; cnt += 1
        gap = start - prev_end
        print(f"  gap 0x{prev_end:x}-0x{start:x} ({gap}B)  |  VERTS 0x{start:x}: {cnt} verts -> 0x{o:x}")
        prev_end = o
    else:
        o += 4
print(f"file size 0x{n:x}")
