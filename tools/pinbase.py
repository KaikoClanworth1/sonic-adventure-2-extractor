"""Pin the exact event-file base address using NJS_OBJECT structure validation.

NJS_OBJECT (big-endian GC, 0x34 bytes):
    0x00 u32   evalflags
    0x04 u32   attach pointer
    0x08 f32[3] position
    0x14 s32[3] rotation (BAMS)
    0x20 f32[3] scale
    0x2C u32   child pointer
    0x30 u32   sibling pointer

Scale is overwhelmingly (1,1,1) -> three 0x3F800000 words. Only the correct
base makes pointer targets land on that pattern.
"""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC\event"

def score_base(data, B):
    n = len(data)
    hits = 0
    ptrs = 0
    for o in range(0, n - 3, 4):
        v = struct.unpack_from(">I", data, o)[0]
        if not (B <= v < B + n):
            continue
        ptrs += 1
        t = v - B
        if t + 0x34 > n:
            continue
        try:
            sx, sy, sz = struct.unpack_from(">3I", data, t + 0x20)
        except struct.error:
            continue
        if sx == 0x3F800000 and sy == 0x3F800000 and sz == 0x3F800000:
            hits += 1
    return hits, ptrs

files = sorted(f for f in os.listdir(ROOT)
               if f.lower().endswith(".prs") and "_" not in f
               and "texture" not in f and "texlist" not in f and f.startswith("e"))

agg = collections.Counter()
for fn in files[:8]:
    data = bytes(prs_decompress(open(os.path.join(ROOT, fn), "rb").read()))
    row = []
    for B in range(0x8125FE40, 0x8125FEA4, 4):
        h, p = score_base(data, B)
        row.append((h, B))
    row.sort(reverse=True)
    print(f"{fn:14s} top: " + "  ".join(f"0x{B:08x}:{h}" for h, B in row[:4]))
    for h, B in row[:1]:
        agg[B] += 1

print()
print("winning base votes:", agg.most_common())
