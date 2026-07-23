"""Probe a Chao-stage .prs (BE Ninja data, file-relative pointers): find
NJS_OBJECT roots by the scale==(1,1,1) signature, build rigid geometry, export
the merged result. Determines whether Chao World is a set of Ninja models."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress
from build import mat_from_srt, mat_mul, xform_point
from chunk import parse_vchunks, parse_poly_chunks, resolve_active_polys

ONE = 0x3F800000


class BE:
    def __init__(self, d): self.d = d; self.n = len(d)
    def ok(self, va): return 0 < va < self.n - 4
    def u32(self, o): return struct.unpack_from(">I", self.d, o)[0]
    def s32(self, o): return struct.unpack_from(">i", self.d, o)[0]
    def f32(self, o):
        return struct.unpack("<f", struct.pack("<I", self.u32(o)))[0]


def read_tree(b, root):
    nodes = []
    def rec(va, parent, depth):
        if not b.ok(va) or depth > 80 or len(nodes) > 8192: return
        flags = b.u32(va); attach = b.u32(va + 4)
        pos = (b.f32(va + 8), b.f32(va + 12), b.f32(va + 16))
        rot = (b.s32(va + 0x14), b.s32(va + 0x18), b.s32(va + 0x1C))
        scale = (b.f32(va + 0x20), b.f32(va + 0x24), b.f32(va + 0x28))
        idx = len(nodes)
        nodes.append(dict(va=va, flags=flags, attach=attach, pos=pos, rot=rot,
                          scale=scale, parent=parent))
        child = b.u32(va + 0x2C); sib = b.u32(va + 0x30)
        if b.ok(child): rec(child, idx, depth + 1)
        if b.ok(sib): rec(sib, parent, depth)
    rec(root, -1, 0)
    for nd in nodes:
        loc = mat_from_srt(nd["pos"], nd["rot"], nd["scale"], bool(nd["flags"] & 0x20))
        nd["world"] = loc if nd["parent"] < 0 else mat_mul(loc, nodes[nd["parent"]]["world"])
    return nodes


def candidates(b):
    out = []
    for o in range(0, b.n - 0x34, 4):
        if b.u32(o + 0x20) == ONE and b.u32(o + 0x24) == ONE and b.u32(o + 0x28) == ONE:
            attach = b.u32(o + 4); child = b.u32(o + 0x2C); sib = b.u32(o + 0x30)
            okp = lambda p: p == 0 or b.ok(p)
            if okp(attach) and okp(child) and okp(sib) and (attach or child):
                out.append(o)
    return out


def build(b, root):
    nodes = read_tree(b, root)
    verts, faces = [], []
    for nd in nodes:
        at = nd["attach"]
        if not b.ok(at): continue
        vl = b.u32(at); pl = b.u32(at + 4)
        cache = {}
        if b.ok(vl):
            for vc in parse_vchunks(b.d, vl):
                for (lp, ln, ci, w) in vc["verts"]:
                    cache[ci] = xform_point(nd["world"], lp)
        if b.ok(pl):
            pcache = {}
            for pc in resolve_active_polys(parse_poly_chunks(b.d, pl), pcache):
                for st in pc.strips:
                    idx = st.indices
                    for k in range(len(idx) - 2):
                        t = [idx[k], idx[k+1], idx[k+2]]
                        if k % 2 == 1: t[0], t[1] = t[1], t[0]
                        if st.reversed: t[0], t[1] = t[1], t[0]
                        if len(set(t)) != 3: continue
                        if all(x in cache for x in t):
                            base = len(verts)
                            verts += [cache[t[0]], cache[t[1]], cache[t[2]]]
                            faces.append((base, base+1, base+2))
    return nodes, verts, faces


if __name__ == "__main__":
    d = bytes(prs_decompress(open(sys.argv[1], "rb").read()))
    b = BE(d)
    cands = candidates(b)
    cset = set(cands)
    covered = set()
    for va in cands:
        for nd in read_tree(b, va)[1:]:
            covered.add(nd["va"])
    roots = [va for va in cands if va not in covered]
    allv, allf = [], []
    built = []
    for va in roots:
        nds, v, f = build(b, va)
        if len(f) >= 10:
            built.append((va, len(nds), len(f)))
            base = len(allv); allv += v
            allf += [(a+base, c+base, e+base) for (a, c, e) in f]
    built.sort(key=lambda r: -r[2])
    print(f"cands={len(cands)} roots={len(roots)} models>=10tris={len(built)} "
          f"total_tris={len(allf)}")
    for va, nn, ft in built[:10]:
        print(f"  0x{va:06x} nodes={nn} tris={ft}")
    with open(r"F:\ClaudeProjects\Sonic Adventure 2\build\chao.obj", "w") as fh:
        for p in allv: fh.write(f"v {p[0]:.3f} {p[1]:.3f} {p[2]:.3f}\n")
        for a, c, e in allf: fh.write(f"f {a+1} {c+1} {e+1}\n")
    print(f"merged {len(allf)} tris -> build/chao.obj")
