"""Diagnose Sonic model-0 geometry: per-node kept/dropped triangles, and a
per-node-colored OBJ so a render shows which mesh (body/torso) is missing."""
import sys, os, struct, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress
from eventmodel import NinjaFile
from mdlmtn import read_mdl_table, read_mtn_table, read_motion, BAMS
from build import valid_attach, mat_from_srt, mat_mul, xform_point, xform_dir
from chunk import parse_poly_chunks, parse_vchunks

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC"

# distinct-ish palette
PAL = [(0.9,0.2,0.2),(0.2,0.6,0.9),(0.2,0.8,0.3),(0.9,0.8,0.2),(0.8,0.3,0.9),
       (0.3,0.9,0.9),(0.9,0.5,0.2),(0.6,0.6,0.6),(0.5,0.9,0.5),(0.9,0.4,0.6)]


def build(nf, root_ptr):
    nodes = nf.read_tree(root_ptr)
    for nd in nodes:
        local = mat_from_srt(nd.pos, nd.rot, nd.scale, bool(nd.flags & 0x20))
        nd.world = local if nd.parent is None else mat_mul(local, nd.parent.world)

    cache = {}
    verts_out, faces_out, cols_out = [], [], []
    face_node = []  # node.index per face
    stats = []  # (node.index, kept, dropped, nverts_defined)

    def accumulate(nd):
        if not valid_attach(nf, nd.attach_ptr):
            return 0
        vl = nf.u32(nf.off(nd.attach_ptr))
        if not (0 < vl < len(nf.d)):
            return 0
        n = 0
        for vc in parse_vchunks(nf.d, nf.off(vl)):
            for (lp, ln, cidx, weight) in vc["verts"]:
                p = xform_point(nd.world, lp)
                if vc["status"] == 0 or cidx not in cache:
                    cache[cidx] = [p[0]*weight, p[1]*weight, p[2]*weight, weight]
                else:
                    c = cache[cidx]
                    c[0]+=p[0]*weight; c[1]+=p[1]*weight; c[2]+=p[2]*weight; c[3]+=weight
                n += 1
        return n

    def resolve(idx):
        c = cache.get(idx)
        if not c:
            return None
        s = c[3] if c[3] else 1.0
        return (c[0]/s, c[1]/s, c[2]/s)

    def read_polys(nd, color):
        if not valid_attach(nf, nd.attach_ptr):
            return (0, 0)
        pl = nf.u32(nf.off(nd.attach_ptr) + 4)
        if not (0 < pl < len(nf.d)):
            return (0, 0)
        kept = dropped = 0
        for pc in parse_poly_chunks(nf.d, nf.off(pl)):
            for st in pc.strips:
                idx = st.indices
                for k in range(len(idx) - 2):
                    tri = [idx[k], idx[k+1], idx[k+2]]
                    if k % 2 == 1: tri[0], tri[1] = tri[1], tri[0]
                    if st.reversed: tri[0], tri[1] = tri[1], tri[0]
                    if len(set(tri)) != 3: continue
                    rs = [resolve(t) for t in tri]
                    if any(r is None for r in rs):
                        dropped += 1; continue
                    b = len(verts_out)
                    verts_out.extend(rs)
                    cols_out.extend([color, color, color])
                    faces_out.append((b, b+1, b+2))
                    face_node.append(nd.index)
                    kept += 1
        return (kept, dropped)

    order = [0]
    def visit(nd):
        nv = accumulate(nd)
        for ch in nd.children:
            visit(ch)
        col = PAL[order[0] % len(PAL)]; order[0]+=1
        kept, dropped = read_polys(nd, col)
        if nv or kept or dropped:
            stats.append((nd.index, kept, dropped, nv))

    for r in [n for n in nodes if n.parent is None]:
        visit(r)
    return verts_out, faces_out, cols_out, stats, len(nodes), face_node


def write_obj_colored(v, f, cols, path):
    with open(path, "w") as fh:
        for p, c in zip(v, cols):
            fh.write(f"v {p[0]:.4f} {p[1]:.4f} {p[2]:.4f} {c[0]:.3f} {c[1]:.3f} {c[2]:.3f}\n")
        for a, b, c in f:
            fh.write(f"f {a+1} {b+1} {c+1}\n")


def write_obj_grouped(v, f, node_of_face, path):
    """One `o node_<idx>` object per node so a render can color each distinctly."""
    with open(path, "w") as fh:
        for p in v:
            fh.write(f"v {p[0]:.4f} {p[1]:.4f} {p[2]:.4f}\n")
        cur = None
        for (a, b, c), ni in zip(f, node_of_face):
            if ni != cur:
                fh.write(f"o node_{ni:02d}\n"); cur = ni
            fh.write(f"f {a+1} {b+1} {c+1}\n")


if __name__ == "__main__":
    mdl = bytes(prs_decompress(open(os.path.join(ROOT, "sonicmdl.prs"), "rb").read()))
    mtn = bytes(prs_decompress(open(os.path.join(ROOT, "sonicmtn.prs"), "rb").read()))
    nf = NinjaFile(mdl, base=0)
    mtbl = read_mdl_table(mdl)
    mtn_tbl = read_mtn_table(mtn)
    want = mtn_tbl[0][1]

    # pick the model idx=0 equivalent: matching motion node count, most tris
    best = None; best_tris = -1
    for idx, ptr in mtbl:
        nodes = nf.read_tree(ptr)
        ca = sum(1 for nd in nodes if not (nd.flags & 0x40))
        if ca == want or len(nodes) == want:
            r = build(nf, ptr)
            if len(r[1]) > best_tris:
                best_tris = len(r[1]); best = (idx, ptr)
    idx, ptr = best
    v, f, cols, stats, nn, face_node = build(nf, ptr)
    tot_kept = sum(s[1] for s in stats); tot_drop = sum(s[2] for s in stats)
    print(f"model idx={idx} nodes={nn} tris_kept={tot_kept} tris_DROPPED={tot_drop}")
    print(f"{'node':>4} {'kept':>6} {'dropped':>8} {'vdefs':>6}")
    for ni, kept, dropped, nv in stats:
        flag = "  <-- DROPS" if dropped else ""
        print(f"{ni:>4} {kept:>6} {dropped:>8} {nv:>6}{flag}")
    out = r"F:\ClaudeProjects\Sonic Adventure 2\build\diag_body.obj"
    write_obj_colored(v, f, cols, out)
    grp = r"F:\ClaudeProjects\Sonic Adventure 2\build\diag_grouped.obj"
    write_obj_grouped(v, f, face_node, grp)
    print("wrote", out, "and", grp)
