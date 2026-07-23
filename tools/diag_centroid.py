"""Per-node world centroid + triangle count for Sonic model-0, bind and posed.
Finds where the big body mesh (node 49) actually lands vs the head."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress
from eventmodel import NinjaFile
from mdlmtn import read_mdl_table, read_mtn_table, read_motion, BAMS
from build import valid_attach, mat_from_srt, mat_mul, xform_point, xform_dir
from chunk import parse_poly_chunks, parse_vchunks
from anim_pose import lerp_key

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC"


def analyze(nf, root_ptr, motion=None, frame=0.0):
    nodes = nf.read_tree(root_ptr)
    anim_nodes = [nd for nd in nodes if not (nd.flags & 0x40)]
    pos_over, rot_over = {}, {}
    if motion is not None:
        for ai, nd in enumerate(anim_nodes):
            pk = motion.pos.get(ai); rk = motion.rot.get(ai)
            if pk:
                p = lerp_key(pk, frame)
                if p: pos_over[nd.index] = p
            if rk:
                r = lerp_key(rk, frame)
                if r: rot_over[nd.index] = r
    for nd in nodes:
        p = pos_over.get(nd.index, nd.pos)
        if nd.index in rot_over:
            rot = tuple(int(round(a / BAMS)) for a in rot_over[nd.index])
        else:
            rot = nd.rot
        local = mat_from_srt(p, rot, nd.scale, bool(nd.flags & 0x20))
        nd.world = local if nd.parent is None else mat_mul(local, nd.parent.world)

    cache = {}
    def accumulate(nd):
        if not valid_attach(nf, nd.attach_ptr): return
        vl = nf.u32(nf.off(nd.attach_ptr))
        if not (0 < vl < len(nf.d)): return
        for vc in parse_vchunks(nf.d, nf.off(vl)):
            for (lp, ln, cidx, weight) in vc["verts"]:
                p = xform_point(nd.world, lp)
                if vc["status"] == 0 or cidx not in cache:
                    cache[cidx] = [p[0]*weight, p[1]*weight, p[2]*weight, weight]
                else:
                    c = cache[cidx]
                    c[0]+=p[0]*weight; c[1]+=p[1]*weight; c[2]+=p[2]*weight; c[3]+=weight
    def resolve(idx):
        c = cache.get(idx)
        if not c: return None
        s = c[3] if c[3] else 1.0
        return (c[0]/s, c[1]/s, c[2]/s)

    rows = []
    def read_polys(nd):
        if not valid_attach(nf, nd.attach_ptr): return
        pl = nf.u32(nf.off(nd.attach_ptr) + 4)
        if not (0 < pl < len(nf.d)): return
        pts = []
        for pc in parse_poly_chunks(nf.d, nf.off(pl)):
            for st in pc.strips:
                idx = st.indices
                for k in range(len(idx) - 2):
                    tri = [idx[k], idx[k+1], idx[k+2]]
                    if len(set(tri)) != 3: continue
                    for t in tri:
                        r = resolve(t)
                        if r: pts.append(r)
        if pts:
            n = len(pts)
            cx = sum(p[0] for p in pts)/n; cy = sum(p[1] for p in pts)/n; cz = sum(p[2] for p in pts)/n
            xs=[p[0] for p in pts]; ys=[p[1] for p in pts]; zs=[p[2] for p in pts]
            rows.append((nd.index, n//3, (cx,cy,cz),
                         (max(xs)-min(xs), max(ys)-min(ys), max(zs)-min(zs))))
    def visit(nd):
        accumulate(nd)
        for ch in nd.children: visit(ch)
        read_polys(nd)
    for r in [n for n in nodes if n.parent is None]: visit(r)
    return rows


if __name__ == "__main__":
    mdl = bytes(prs_decompress(open(os.path.join(ROOT, "sonicmdl.prs"), "rb").read()))
    mtn = bytes(prs_decompress(open(os.path.join(ROOT, "sonicmtn.prs"), "rb").read()))
    nf = NinjaFile(mdl, base=0)
    mtbl = read_mdl_table(mdl); mtn_tbl = read_mtn_table(mtn); want = mtn_tbl[0][1]
    # model idx=0 (matching, most tris)
    ptr0 = None; best_t=-1
    for idx, ptr in mtbl:
        nodes = nf.read_tree(ptr)
        ca = sum(1 for nd in nodes if not (nd.flags & 0x40))
        if ca == want or len(nodes)==want:
            rr = analyze(nf, ptr)
            t = sum(r[1] for r in rr)
            if t>best_t: best_t=t; ptr0=ptr
    for label, mo, fr in [("BIND", None, 0.0)]:
        rows = analyze(nf, ptr0, mo, fr)
        print(f"=== {label} ===  ({sum(r[1] for r in rows)} tris over {len(rows)} meshes)")
        print(f"{'node':>4} {'tris':>5}   {'centroid(x,y,z)':>26}   {'bbox(w,h,d)':>22}")
        for ni, tris, ctr, box in sorted(rows, key=lambda r:-r[1]):
            print(f"{ni:>4} {tris:>5}   ({ctr[0]:8.2f},{ctr[1]:8.2f},{ctr[2]:8.2f})   "
                  f"({box[0]:6.2f},{box[1]:6.2f},{box[2]:6.2f})")
