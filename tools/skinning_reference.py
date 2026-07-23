"""Vertices pre-order, polygons post-order (after the node's children).

A skinned mesh stores its polygon on a parent/container node while the deforming
vertices live on the child bones; reading the polygon after descending the
children lets those vertices be in the cache. Self-contained rigid meshes (verts
+ polys on one leaf node) still read their own fresh vertices."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress
from eventmodel import NinjaFile
from mdlmtn import read_mdl_table
from build import valid_attach, mat_from_srt, mat_mul, xform_point, xform_dir
from chunk import parse_poly_chunks, parse_vchunks


def build(nf, root_ptr):
    nodes = nf.read_tree(root_ptr)
    for nd in nodes:
        local = mat_from_srt(nd.pos, nd.rot, nd.scale, bool(nd.flags & 0x20))
        nd.world = local if nd.parent is None else mat_mul(local, nd.parent.world)

    cache = {}
    verts_out, faces_out, node_out = [], [], []
    stats = {"dropped": 0, "kept": 0}

    def accumulate(nd):
        if not valid_attach(nf, nd.attach_ptr):
            return
        vl = nf.u32(nf.off(nd.attach_ptr))
        if not (0 < vl < len(nf.d)):
            return
        for vc in parse_vchunks(nf.d, nf.off(vl)):
            for (lp, ln, cidx, weight) in vc["verts"]:
                p = xform_point(nd.world, lp)
                npv = xform_dir(nd.world, ln) if ln else (0.0, 0.0, 0.0)
                if vc["status"] == 0:
                    cache[cidx] = [p[0]*weight, p[1]*weight, p[2]*weight,
                                   npv[0]*weight, npv[1]*weight, npv[2]*weight,
                                   weight, weight, nd.index]
                else:
                    c = cache.get(cidx)
                    if c is None:
                        cache[cidx] = [p[0]*weight, p[1]*weight, p[2]*weight,
                                       npv[0]*weight, npv[1]*weight, npv[2]*weight,
                                       weight, weight, nd.index]
                    else:
                        c[0]+=p[0]*weight; c[1]+=p[1]*weight; c[2]+=p[2]*weight
                        c[3]+=npv[0]*weight; c[4]+=npv[1]*weight; c[5]+=npv[2]*weight
                        c[6]+=weight
                        if weight > c[7]:
                            c[7]=weight; c[8]=nd.index

    def resolve(idx):
        c = cache.get(idx)
        if c is None:
            return None
        s = c[6] if c[6] != 0 else 1.0
        return (c[0]/s, c[1]/s, c[2]/s), c[8]

    def read_polys(nd):
        if not valid_attach(nf, nd.attach_ptr):
            return
        pl = nf.u32(nf.off(nd.attach_ptr) + 4)
        if not (0 < pl < len(nf.d)):
            return
        for pc in parse_poly_chunks(nf.d, nf.off(pl)):
            for st in pc.strips:
                idx = st.indices
                for k in range(len(idx) - 2):
                    tri = [idx[k], idx[k+1], idx[k+2]]
                    if k % 2 == 1:
                        tri[0], tri[1] = tri[1], tri[0]
                    if st.reversed:
                        tri[0], tri[1] = tri[1], tri[0]
                    if len(set(tri)) != 3:
                        continue
                    rs = [resolve(t) for t in tri]
                    if any(r is None for r in rs):
                        stats["dropped"] += 1
                        continue
                    stats["kept"] += 1
                    base = len(verts_out)
                    for r in rs:
                        verts_out.append(r[0]); node_out.append(r[1])
                    faces_out.append((base, base+1, base+2))

    def visit(nd):
        accumulate(nd)               # pre-order: vertices
        for ch in nd.children:
            visit(ch)
        read_polys(nd)               # post-order: polygons

    # roots (the tree can have several top-level siblings)
    roots = [n for n in nodes if n.parent is None]
    for r in roots:
        visit(r)
    print(f"  triangles kept={stats['kept']} dropped={stats['dropped']}")
    return verts_out, faces_out, node_out


if __name__ == "__main__":
    ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC"
    fn = sys.argv[1] if len(sys.argv) > 1 else "sonicmdl.prs"
    mi = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    out = sys.argv[3] if len(sys.argv) > 3 else r"F:\ClaudeProjects\Sonic Adventure 2\build\skinned3.obj"
    data = bytes(prs_decompress(open(os.path.join(ROOT, fn), "rb").read()))
    nf = NinjaFile(data, base=0)
    idx, ptr = read_mdl_table(data)[mi]
    v, f, nd = build(nf, ptr)
    print(f"{fn} model[{mi}]: verts={len(v)} tris={len(f)}")
    with open(out, "w") as fh:
        for p in v:
            fh.write(f"v {p[0]:.4f} {p[1]:.4f} {p[2]:.4f}\n")
        by = {}
        for (a, b, c) in f:
            by.setdefault(nd[a], []).append((a, b, c))
        for k in sorted(by):
            fh.write(f"o node_{k}\n")
            for a, b, c in by[k]:
                fh.write(f"f {a+1} {b+1} {c+1}\n")
    print("wrote", out)
