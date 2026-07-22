"""Determine the event-file base address without assuming it.

1. Find NJS_OBJECT candidates purely by byte pattern (scale == 1,1,1 and
   plausible rotation magnitudes).
2. Collect every big-endian pointer-looking word in the file.
3. The true base B maximises |{ p : p - B is an object candidate offset }|.
   Take the mode of (pointer - candidate_offset).
"""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC\event"
ONE = 0x3F800000

def find_objects(data):
    """Offsets where an NJS_OBJECT plausibly starts (scale field == 1,1,1)."""
    n = len(data)
    out = []
    for o in range(0, n - 0x34, 4):
        if (struct.unpack_from(">I", data, o + 0x20)[0] == ONE and
            struct.unpack_from(">I", data, o + 0x24)[0] == ONE and
            struct.unpack_from(">I", data, o + 0x28)[0] == ONE):
            out.append(o)
    return out

def pointers(data):
    n = len(data)
    return [struct.unpack_from(">I", data, o)[0] for o in range(0, n - 3, 4)]

def analyse(fn):
    data = bytes(prs_decompress(open(os.path.join(ROOT, fn), "rb").read()))
    n = len(data)
    objs = find_objects(data)
    objset = set(objs)
    ptrs = [p for p in pointers(data) if 0x81000000 <= p < 0x81800000]
    c = collections.Counter()
    for p in ptrs:
        for o in objs:
            b = p - o
            if 0x81200000 <= b <= 0x81300000:
                c[b] += 1
    top = c.most_common(5)
    print(f"{fn:14s} size=0x{n:06x} objcand={len(objs):5d} ptrs={len(ptrs):5d}")
    for b, k in top:
        print(f"                base 0x{b:08x} -> {k} pointer/object matches")
    return top[0][0] if top else None

votes = collections.Counter()
files = sorted(f for f in os.listdir(ROOT)
               if f.lower().endswith(".prs") and "_" not in f
               and "texture" not in f and "texlist" not in f and f.startswith("e"))
for fn in files[:6]:
    b = analyse(fn)
    if b: votes[b] += 1
print()
print("VOTES:", votes.most_common())
