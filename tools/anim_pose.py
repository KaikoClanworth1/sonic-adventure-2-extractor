"""Apply an SA2 motion to a character model and export a posed frame.

Motions bind to a model when the model's animated-node count (nodes without the
NoAnimate flag 0x40, in tree order) equals the motion's node count. At a frame,
each animated node's local pos/rot is replaced by the interpolated keyframe,
world matrices recomputed, and the skinned mesh rebuilt."""
import sys, os, struct, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress
from eventmodel import NinjaFile
from mdlmtn import read_mdl_table, read_mtn_table, read_motion, BAMS
from build import valid_attach, mat_from_srt, mat_mul, xform_point, xform_dir
from chunk import parse_poly_chunks, parse_vchunks

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC"


def count_animated(nodes):
    return sum(1 for nd in nodes if not (nd.flags & 0x40))


def lerp_key(keys, frame):
    """Linear interpolation over [(frame, (x,y,z))] keyframes."""
    if not keys:
        return None
    if frame <= keys[0][0]:
        return keys[0][1]
    if frame >= keys[-1][0]:
        return keys[-1][1]
    for i in range(1, len(keys)):
        f0, v0 = keys[i - 1]
        f1, v1 = keys[i]
        if frame <= f1:
            t = (frame - f0) / (f1 - f0) if f1 != f0 else 0.0
            return tuple(v0[k] + (v1[k] - v0[k]) * t for k in range(3))
    return keys[-1][1]


def pose_and_build(nf, root_ptr, motion=None, frame=0.0):
    nodes = nf.read_tree(root_ptr)
    # map animated-node index -> node (skip NoAnimate)
    anim_nodes = [nd for nd in nodes if not (nd.flags & 0x40)]

    # per-node local pos/rot, overridden by the motion where present
    pos_over = {}
    rot_over = {}
    if motion is not None:
        for ai, nd in enumerate(anim_nodes):
            pk = motion.pos.get(ai)
            rk = motion.rot.get(ai)
            if pk:
                p = lerp_key(pk, frame)
                if p:
                    pos_over[nd.index] = p
            if rk:
                r = lerp_key(rk, frame)   # radians
                if r:
                    rot_over[nd.index] = r

    # world matrices with overrides (rot override is in radians -> BAMS ints)
    for nd in nodes:
        p = pos_over.get(nd.index, nd.pos)
        if nd.index in rot_over:
            rr = rot_over[nd.index]
            rot = tuple(int(round(a / BAMS)) for a in rr)
        else:
            rot = nd.rot
        local = mat_from_srt(p, rot, nd.scale, bool(nd.flags & 0x20))
        nd.world = local if nd.parent is None else mat_mul(local, nd.parent.world)

    # skinned build (verts pre-order, polys post-order)
    cache = {}
    verts_out, faces_out = [], []

    def accumulate(nd):
        if not valid_attach(nf, nd.attach_ptr):
            return
        vl = nf.u32(nf.off(nd.attach_ptr))
        if not (0 < vl < len(nf.d)):
            return
        for vc in parse_vchunks(nf.d, nf.off(vl)):
            for (lp, ln, cidx, weight) in vc["verts"]:
                p = xform_point(nd.world, lp)
                np_ = xform_dir(nd.world, ln) if ln else (0, 0, 0)
                if vc["status"] == 0 or cidx not in cache:
                    cache[cidx] = [p[0]*weight, p[1]*weight, p[2]*weight, weight]
                else:
                    c = cache[cidx]
                    c[0]+=p[0]*weight; c[1]+=p[1]*weight; c[2]+=p[2]*weight; c[3]+=weight

    def resolve(idx):
        c = cache.get(idx)
        if not c:
            return None
        s = c[3] if c[3] else 1.0
        return (c[0]/s, c[1]/s, c[2]/s)

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
                    if k % 2 == 1: tri[0], tri[1] = tri[1], tri[0]
                    if st.reversed: tri[0], tri[1] = tri[1], tri[0]
                    if len(set(tri)) != 3: continue
                    rs = [resolve(t) for t in tri]
                    if any(r is None for r in rs): continue
                    b = len(verts_out)
                    verts_out.extend(rs)
                    faces_out.append((b, b+1, b+2))

    def visit(nd):
        accumulate(nd)
        for ch in nd.children:
            visit(ch)
        read_polys(nd)

    for r in [n for n in nodes if n.parent is None]:
        visit(r)
    return verts_out, faces_out, len(anim_nodes)


def write_obj(v, f, path):
    with open(path, "w") as fh:
        for p in v:
            fh.write(f"v {p[0]:.4f} {p[1]:.4f} {p[2]:.4f}\n")
        for a, b, c in f:
            fh.write(f"f {a+1} {b+1} {c+1}\n")


if __name__ == "__main__":
    mdl = bytes(prs_decompress(open(os.path.join(ROOT, "sonicmdl.prs"), "rb").read()))
    mtn = bytes(prs_decompress(open(os.path.join(ROOT, "sonicmtn.prs"), "rb").read()))
    nf = NinjaFile(mdl, base=0)
    mtnf = NinjaFile(mtn, base=0)

    mtbl = read_mdl_table(mdl)
    # CountAnimated + total nodes for ALL models, find one matching a motion
    mtn_tbl = read_mtn_table(mtn)
    want = mtn_tbl[0][1]
    print(f"motions node count = {want}")
    best = None
    best_tris = -1
    for idx, ptr in mtbl:
        nodes = nf.read_tree(ptr)
        ca = count_animated(nodes)
        if ca == want or len(nodes) == want:
            _, ff, _ = pose_and_build(nf, ptr, None)
            print(f"  MATCH model idx={idx} nodes={len(nodes)} countAnimated={ca} tris={len(ff)}")
            if len(ff) > best_tris:      # prefer the highest-detail matching model
                best_tris = len(ff)
                best = (idx, ptr, ca)
    if best is None:
        # fall back to the largest model
        big = max(mtbl, key=lambda e: len(nf.read_tree(e[1])))
        best = (big[0], big[1], count_animated(nf.read_tree(big[1])))
        print(f"  no exact match; using largest model idx={best[0]} "
              f"nodes={len(nf.read_tree(best[1]))}")

    target_idx, target_ptr, target_ca = best
    frame = float(sys.argv[1]) if len(sys.argv) > 1 else 0.0
    # pick the LONGEST motion (most likely the idle / a full cycle)
    longest = None
    for mi, ncount, moff in mtn_tbl:
        m = read_motion(mtn, moff, ncount)
        if m and (longest is None or m.frame_count > longest[1].frame_count):
            longest = ((mi, ncount, moff), m)
    (mi, ncount, moff), mo = longest
    print(f"applying motion idx={mi} nodeCount={ncount} frames={mo.frame_count} "
          f"frame={frame} to model {target_idx}")
    v, f, na = pose_and_build(nf, target_ptr, mo, frame=frame)
    write_obj(v, f, r"F:\ClaudeProjects\Sonic Adventure 2\build\posed.obj")
    print(f"posed: verts={len(v)} tris={len(f)} animNodes={na} -> build/posed.obj")
    vb, fb, _ = pose_and_build(nf, target_ptr, None)
    write_obj(vb, fb, r"F:\ClaudeProjects\Sonic Adventure 2\build\bind0.obj")
    print(f"bind (model {target_idx}): tris={len(fb)} -> build/bind0.obj")
