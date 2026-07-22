"""Disambiguate the event base address using Ninja chunk-stream validity.

For a candidate base B: find NJS_OBJECTs, follow attach -> NJS_CNK_MODEL,
and require vlist[0] to be a vertex chunk type (32..50) and plist[0] to be a
valid poly chunk type. Only the true base satisfies this en masse.
"""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC\event"
VTX_OK = set(range(32, 51))
POLY_OK = set(range(1, 6)) | {8, 9} | set(range(16, 32)) | set(range(56, 59)) | set(range(64, 76)) | {255, 0}

def score(data, B):
    n = len(data)
    good = bad = 0
    for o in range(0, n - 3, 4):
        v = struct.unpack_from(">I", data, o)[0]
        if not (B <= v < B + n):
            continue
        t = v - B
        if t + 0x34 > n:
            continue
        sx, sy, sz = struct.unpack_from(">3I", data, t + 0x20)
        if not (sx == sy == sz == 0x3F800000):
            continue
        att = struct.unpack_from(">I", data, t + 4)[0]
        if not (B <= att < B + n):
            continue
        a = att - B
        if a + 0x18 > n:
            continue
        vl, pl = struct.unpack_from(">2I", data, a)
        if not (B <= vl < B + n and B <= pl < B + n):
            continue
        vh = data[vl - B]
        ph = data[pl - B]
        if vh in VTX_OK and ph in POLY_OK:
            good += 1
        else:
            bad += 1
    return good, bad

files = sorted(f for f in os.listdir(ROOT)
               if f.lower().endswith(".prs") and "_" not in f
               and "texture" not in f and "texlist" not in f and f.startswith("e"))
agg = collections.Counter()
for fn in files[:6]:
    data = bytes(prs_decompress(open(os.path.join(ROOT, fn), "rb").read()))
    res = []
    for B in range(0x8125FE00, 0x8125FF00, 4):
        g, b = score(data, B)
        if g:
            res.append((g, -b, B))
    res.sort(reverse=True)
    print(f"{fn:14s} " + "  ".join(f"0x{B:08x}:{g}g/{-b}b" for g, b, B in res[:4]))
    if res:
        agg[res[0][2]] += 1
print()
print("VOTES:", agg.most_common())
