"""Census the vertex/poly chunk types actually used, with strict attach validation."""
import os, sys, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress
from eventmodel import NinjaFile, EVENT_BASE
from chunk import VERTEX_TYPES, STRIP_TYPES

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC\event"
ONE = 0x3F800000
VALID_V = set(VERTEX_TYPES)
VALID_P = set(range(1, 6)) | {8, 9} | set(range(16, 32)) | set(range(56, 59)) | set(STRIP_TYPES) | {0, 255}


def valid_attach(nf, ptr):
    """A real NJS_CNK_MODEL: vlist starts with a vertex chunk, plist with a poly chunk."""
    if not nf.ok(ptr):
        return False
    a = nf.off(ptr)
    if a + 0x18 > nf.n:
        return False
    vl = nf.u32(a); pl = nf.u32(a + 4)
    if vl == 0 and pl == 0:
        return False
    if vl:
        if not nf.ok(vl):
            return False
        h = nf.u32(nf.off(vl)) & 0xFF
        if h not in VALID_V and h != 255:
            return False
    if pl:
        if not nf.ok(pl):
            return False
        h = struct.unpack_from(">H", nf.d, nf.off(pl))[0] & 0xFF
        if h not in VALID_P:
            return False
    r = nf.f32(a + 20)
    if not (0.0 <= r < 1e6):
        return False
    return True


def main():
    files = sorted(f for f in os.listdir(ROOT)
                   if f.lower().endswith(".prs") and "_" not in f
                   and "texture" not in f and "texlist" not in f
                   and f.startswith("e") and not f.startswith("me"))
    vt = collections.Counter()
    pt = collections.Counter()
    nverts = collections.Counter()
    total_attach = 0
    for fn in files:
        raw = bytes(prs_decompress(open(os.path.join(ROOT, fn), "rb").read()))
        nf = NinjaFile(raw, EVENT_BASE)
        d, n, base = nf.d, nf.n, nf.base
        seen = set()
        for o in range(0, n - 3, 4):
            v = struct.unpack_from(">I", d, o)[0]
            if not (base <= v < base + n):
                continue
            t = v - base
            if t + 0x34 > n:
                continue
            if not (struct.unpack_from(">I", d, t + 0x20)[0] == ONE and
                    struct.unpack_from(">I", d, t + 0x24)[0] == ONE and
                    struct.unpack_from(">I", d, t + 0x28)[0] == ONE):
                continue
            att = struct.unpack_from(">I", d, t + 4)[0]
            if att in seen or not valid_attach(nf, att):
                continue
            seen.add(att)
            total_attach += 1
            a = nf.off(att)
            vl = nf.u32(a); pl = nf.u32(a + 4)
            # walk vertex chunk heads
            if nf.ok(vl):
                p = nf.off(vl)
                for _ in range(64):
                    if p + 8 > n: break
                    w = nf.u32(p)
                    h = w & 0xFF; size = w >> 16
                    if h == 255: break
                    if h == 0: p += 4; continue
                    if h not in VALID_V: break
                    w2 = nf.u32(p + 4)
                    vt[h] += 1
                    nverts[h] += (w2 >> 16)
                    p += 4 + size * 4
            if nf.ok(pl):
                p = nf.off(pl)
                for _ in range(256):
                    if p + 2 > n: break
                    w = struct.unpack_from(">H", d, p)[0]
                    h = w & 0xFF
                    if h == 255: break
                    if h == 0: p += 2; continue
                    pt[h] += 1
                    if 1 <= h <= 5: p += 2; continue
                    if h in (8, 9): p += 4; continue
                    size = struct.unpack_from(">H", d, p + 2)[0]
                    p += 4 + size * 2

    print(f"valid attaches: {total_attach}")
    print("\nVERTEX chunk types:")
    for h, c in vt.most_common():
        print(f"  {h:3d} {VERTEX_TYPES.get(h,'?'):32s} chunks={c:6d} verts={nverts[h]:9,d}")
    print("\nPOLY chunk types:")
    names = {**{k: STRIP_TYPES[k] for k in STRIP_TYPES}}
    for h, c in pt.most_common():
        nm = names.get(h)
        if nm is None:
            nm = ("Bits" if 1 <= h <= 5 else "Tiny/TexID" if h in (8, 9)
                  else "Material" if 16 <= h <= 31 else "Volume" if 56 <= h <= 58 else "?")
        print(f"  {h:3d} {nm:32s} count={c:6d}")


if __name__ == "__main__":
    main()
