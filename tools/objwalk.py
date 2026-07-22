import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress

BASE = 0x8125FE60
ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC\event"

fn = sys.argv[1] if len(sys.argv) > 1 else "e0000.prs"
data = bytes(prs_decompress(open(os.path.join(ROOT, fn), "rb").read()))
n = len(data)

def U32(o): return struct.unpack_from(">I", data, o)[0]
def I32(o): return struct.unpack_from(">i", data, o)[0]
def F32(o): return struct.unpack_from(">f", data, o)[0]
def ok(p):  return BASE <= p < BASE + n
def off(p): return p - BASE

def hexat(o, count=64, indent="  "):
    c = data[o:o+count]
    for i in range(0, len(c), 16):
        r = c[i:i+16]
        print(f"{indent}{o+i:08x}  " + " ".join(f"{b:02x}" for b in r).ljust(48) +
              "  |" + "".join(chr(b) if 32 <= b < 127 else "." for b in r) + "|")

def dump_object(p, depth=0, seen=None, maxdepth=3):
    if seen is None: seen = set()
    if not ok(p) or p in seen or depth > maxdepth: return
    seen.add(p)
    o = off(p)
    flags = U32(o)
    attach = U32(o+4)
    pos = [F32(o+8), F32(o+12), F32(o+16)]
    rot = [I32(o+0x14), I32(o+0x18), I32(o+0x1C)]
    scl = [F32(o+0x20), F32(o+0x24), F32(o+0x28)]
    child = U32(o+0x2C)
    sib = U32(o+0x30)
    ind = "  " * depth
    print(f"{ind}OBJ @0x{o:06x} flags={flags:08x} attach=0x{attach:08x} "
          f"pos=({pos[0]:.2f},{pos[1]:.2f},{pos[2]:.2f}) "
          f"rot=({rot[0]:#x},{rot[1]:#x},{rot[2]:#x}) "
          f"scl=({scl[0]:.2f},{scl[1]:.2f},{scl[2]:.2f})")
    if ok(attach):
        ao = off(attach)
        print(f"{ind}  ATTACH @0x{ao:06x}:")
        hexat(ao, 48, ind + "    ")
        for k in range(0, 32, 4):
            v = U32(ao+k)
            t = f" -> 0x{off(v):06x}" if ok(v) else ""
            print(f"{ind}    +0x{k:02x}: 0x{v:08x}{t}")
    if ok(child): dump_object(child, depth+1, seen, maxdepth)
    if ok(sib):   dump_object(sib, depth, seen, maxdepth)

if len(sys.argv) > 2:
    dump_object(int(sys.argv[2], 0), maxdepth=int(sys.argv[3]) if len(sys.argv) > 3 else 1)
else:
    lst = off(U32(0x20))
    print(f"root list @0x{lst:06x}")
    for i in range(4):
        p = U32(lst + i*4)
        print(f"\n===== root[{i}] = 0x{p:08x} =====")
        if ok(p): dump_object(p, maxdepth=1)
