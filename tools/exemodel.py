"""Find and build Ninja models embedded (little-endian) in sonic2app.exe.
Validation prototype: rigid positions + triangles only, to confirm the candidate
roots are real models before porting the extraction to C++."""
import sys, os, struct, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pe as pemod
from build import mat_from_srt, mat_mul, xform_point

ONE = 0x3F800000


class LE:
    def __init__(self, flat, base):
        self.d = flat; self.base = base; self.n = len(flat)
    def off(self, va): return va - self.base
    def ok(self, va): return self.base < va < self.base + self.n - 4
    def u32(self, o): return struct.unpack_from("<I", self.d, o)[0]
    def s32(self, o): return struct.unpack_from("<i", self.d, o)[0]
    def u16(self, o): return struct.unpack_from("<H", self.d, o)[0]
    def s16(self, o): return struct.unpack_from("<h", self.d, o)[0]
    def f32(self, o): return struct.unpack_from("<f", self.d, o)[0]


def read_tree(b, root):
    nodes = []
    def rec(va, parent, depth):
        if not b.ok(va) or depth > 80 or len(nodes) > 8192:
            return
        o = b.off(va)
        nd = dict(va=va, flags=b.u32(o), attach=b.u32(o + 4),
                  pos=(b.f32(o + 8), b.f32(o + 12), b.f32(o + 16)),
                  rot=(b.s32(o + 0x14), b.s32(o + 0x18), b.s32(o + 0x1C)),
                  scale=(b.f32(o + 0x20), b.f32(o + 0x24), b.f32(o + 0x28)),
                  parent=parent)
        idx = len(nodes); nodes.append(nd)
        child = b.u32(o + 0x2C); sib = b.u32(o + 0x30)
        if b.ok(child): rec(child, idx, depth + 1)
        if b.ok(sib): rec(sib, parent, depth)
    rec(root, -1, 0)
    for nd in nodes:
        loc = mat_from_srt(nd["pos"], nd["rot"], nd["scale"], bool(nd["flags"] & 0x20))
        nd["world"] = loc if nd["parent"] < 0 else mat_mul(loc, nodes[nd["parent"]]["world"])
    return nodes


def parse_verts_le(b, off):
    """LE vertex chunks -> {cache_index: (x,y,z)}."""
    out = {}
    o = off
    for _ in range(4096):
        if not (0 < o < b.n - 8): break
        w = b.u32(o); head = w & 0xFF; size = w >> 16
        if head == 0xFF: break
        if head == 0: o += 4; continue
        if not (32 <= head <= 50): break
        w2 = b.u32(o + 4); index_off = w2 & 0xFFFF; nb = w2 >> 16
        stride = (size - 1) // nb if nb > 0 and size >= 1 else 4
        if stride >= 3 and nb > 0:
            for i in range(nb):
                vo = o + 8 + i * stride * 4
                if vo + 12 > b.n: break
                out[index_off + i] = (b.f32(vo), b.f32(vo + 4), b.f32(vo + 8))
        o += 4 + size * 4
    return out


def parse_tris_le(b, off, cache):
    """LE poly chunks -> list of (i0,i1,i2) into cache."""
    tris = []
    o = off
    for _ in range(8192):
        if not (0 < o < b.n - 2): break
        w = b.u16(o); head = w & 0xFF
        if head == 0xFF: break
        if head == 0: o += 2; continue
        if 1 <= head <= 5: o += 2; continue
        if head in (8, 9): o += 4; continue
        if 16 <= head <= 31: o += 4 + b.u16(o + 2) * 2; continue
        if 56 <= head <= 58: o += 4 + b.u16(o + 2) * 2; continue
        if 64 <= head <= 75:
            size = b.u16(o + 2); end = o + 4 + size * 2
            hdr = b.u16(o + 4); nstr = hdr & 0x3FFF; uf = (hdr >> 14) & 3
            p = o + 6
            has_uv = head in (65, 66, 68, 69, 71, 72, 74, 75)
            has_nrm = 67 <= head <= 69; has_col = 70 <= head <= 72
            for _s in range(nstr):
                if p + 2 > end: break
                ln = b.s16(p); p += 2; cnt = abs(ln); rev = ln < 0
                strip = []
                for k in range(cnt):
                    if p + 2 > end: break
                    strip.append(b.u16(p)); p += 2
                    if has_uv: p += 4
                    if has_nrm: p += 12
                    if has_col: p += 4
                    if uf: p += uf * 2
                for k in range(len(strip) - 2):
                    a, bb, c = strip[k], strip[k + 1], strip[k + 2]
                    if k % 2 == 1: a, bb = bb, a
                    if rev: a, bb = bb, a
                    if len({a, bb, c}) == 3: tris.append((a, bb, c))
            o = end; continue
        o += 4 + b.u16(o + 2) * 2
    return tris


def build(b, root):
    nodes = read_tree(b, root)
    verts_out, faces_out = [], []
    for nd in nodes:
        at = nd["attach"]
        if not b.ok(at): continue
        vl = b.u32(b.off(at)); pl = b.u32(b.off(at) + 4)
        cache_local = {}
        if b.ok(vl):
            for ci, lp in parse_verts_le(b, b.off(vl)).items():
                cache_local[ci] = xform_point(nd["world"], lp)
        if b.ok(pl):
            for (a, bb, c) in parse_tris_le(b, b.off(pl), cache_local):
                if a in cache_local and bb in cache_local and c in cache_local:
                    base = len(verts_out)
                    verts_out += [cache_local[a], cache_local[bb], cache_local[c]]
                    faces_out.append((base, base + 1, base + 2))
    return nodes, verts_out, faces_out


if __name__ == "__main__":
    path = sys.argv[1]
    pe = pemod.PE(open(path, "rb").read())
    flat = pe.flat_image()
    b = LE(flat, pe.image_base)
    n, cand = pemod.scan_objects(pe, le=True)
    # full candidate list (rescan, keep all)
    cands = []
    for s in pe.sections:
        if not (s["chars"] & 0x40000000) or s["name"] in (".text", ".reloc"): continue
        base, end = s["roff"], s["roff"] + s["rsize"] - 0x34
        o = base
        while o < end:
            if (struct.unpack_from("<I", flat if False else pe.d, o + 0x20)[0] == ONE and
                    struct.unpack_from("<I", pe.d, o + 0x24)[0] == ONE and
                    struct.unpack_from("<I", pe.d, o + 0x28)[0] == ONE):
                va = pe.image_base + s["vaddr"] + (o - s["roff"])
                cands.append(va)
            o += 4
    cset = set(cands)
    # roots: candidates never reached as a descendant of another candidate
    covered = set()
    for va in cands:
        nds = read_tree(b, va)
        for nd in nds[1:]:
            covered.add(nd["va"])
    roots = [va for va in cands if va not in covered]
    # rank roots by triangle count
    built = []
    for va in roots:
        nds, v, f = build(b, va)
        if len(f) >= 30:
            built.append((va, len(nds), len(f)))
    built.sort(key=lambda r: -r[2])
    print(f"candidates={len(cands)} roots={len(roots)} models>=30tris={len(built)}")
    for va, nn, ft in built[:15]:
        print(f"  root=0x{va:08x} nodes={nn} tris={ft}")
    # export the biggest for a look
    if built:
        va = built[0][0]
        nds, v, f = build(b, va)
        with open(r"F:\ClaudeProjects\Sonic Adventure 2\build\exemodel.obj", "w") as fh:
            for p in v: fh.write(f"v {p[0]:.3f} {p[1]:.3f} {p[2]:.3f}\n")
            for a, bb, c in f: fh.write(f"f {a+1} {bb+1} {c+1}\n")
        print(f"wrote biggest (0x{va:08x}, {len(f)} tris) -> build/exemodel.obj")
