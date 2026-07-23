"""Corrected SA2 chunk skinning: pre-order traversal, honoring CacheList(4)/
DrawList(5) so a mesh cached on one bone is drawn by a later bone once the
weighted vertex cache is complete. Validates model-0 (Sonic body)."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress
from eventmodel import NinjaFile
from mdlmtn import read_mdl_table, read_mtn_table, read_motion, BAMS
from build import valid_attach, mat_from_srt, mat_mul, xform_point, xform_dir
from chunk import parse_poly_chunks, parse_vchunks, resolve_active_polys
from anim_pose import lerp_key

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC"


def build(nf, root_ptr, motion=None, frame=0.0):
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
        rot = (tuple(int(round(a / BAMS)) for a in rot_over[nd.index])
               if nd.index in rot_over else nd.rot)
        local = mat_from_srt(p, rot, nd.scale, bool(nd.flags & 0x20))
        nd.world = local if nd.parent is None else mat_mul(local, nd.parent.world)

    vcache = {}                 # cache_index -> [wx, wy, wz, wsum]
    poly_cache = {}             # slot -> [PolyChunk], persists across nodes
    verts_out, faces_out, face_node = [], [], []

    def accumulate(nd):
        if not valid_attach(nf, nd.attach_ptr): return
        vl = nf.u32(nf.off(nd.attach_ptr))
        if not (0 < vl < len(nf.d)): return
        for vc in parse_vchunks(nf.d, nf.off(vl)):
            for (lp, ln, cidx, weight) in vc["verts"]:
                p = xform_point(nd.world, lp)
                if vc["status"] == 0 or cidx not in vcache:
                    vcache[cidx] = [p[0]*weight, p[1]*weight, p[2]*weight, weight]
                else:
                    c = vcache[cidx]
                    c[0]+=p[0]*weight; c[1]+=p[1]*weight; c[2]+=p[2]*weight; c[3]+=weight

    def resolve(idx):
        c = vcache.get(idx)
        if not c: return None
        s = c[3] if c[3] else 1.0
        return (c[0]/s, c[1]/s, c[2]/s)

    def draw(nd):
        if not valid_attach(nf, nd.attach_ptr): return
        pl = nf.u32(nf.off(nd.attach_ptr) + 4)
        if not (0 < pl < len(nf.d)): return
        chunks = parse_poly_chunks(nf.d, nf.off(pl))
        for pc in resolve_active_polys(chunks, poly_cache):
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
                    face_node.append(nd.index)

    def visit(nd):
        accumulate(nd)
        draw(nd)
        for ch in nd.children: visit(ch)

    for r in [n for n in nodes if n.parent is None]: visit(r)
    return verts_out, faces_out, face_node, len(anim_nodes)


def write_obj(v, f, path):
    with open(path, "w") as fh:
        for p in v: fh.write(f"v {p[0]:.4f} {p[1]:.4f} {p[2]:.4f}\n")
        for a, b, c in f: fh.write(f"f {a+1} {b+1} {c+1}\n")


def write_grouped(v, f, fn, path):
    with open(path, "w") as fh:
        for p in v: fh.write(f"v {p[0]:.4f} {p[1]:.4f} {p[2]:.4f}\n")
        cur = None
        for (a, b, c), ni in zip(f, fn):
            if ni != cur: fh.write(f"o node_{ni:02d}\n"); cur = ni
            fh.write(f"f {a+1} {b+1} {c+1}\n")


if __name__ == "__main__":
    mdl = bytes(prs_decompress(open(os.path.join(ROOT, "sonicmdl.prs"), "rb").read()))
    mtn = bytes(prs_decompress(open(os.path.join(ROOT, "sonicmtn.prs"), "rb").read()))
    nf = NinjaFile(mdl, base=0)
    mtbl = read_mdl_table(mdl); mtn_tbl = read_mtn_table(mtn); want = mtn_tbl[0][1]
    ptr0 = None; best = -1
    for idx, ptr in mtbl:
        nodes = nf.read_tree(ptr)
        ca = sum(1 for nd in nodes if not (nd.flags & 0x40))
        if ca == want or len(nodes) == want:
            r = build(nf, ptr)
            if len(r[1]) > best: best = len(r[1]); ptr0 = ptr
    frame = float(sys.argv[1]) if len(sys.argv) > 1 else 0.0
    v, f, fn, na = build(nf, ptr0)
    write_obj(v, f, r"F:\ClaudeProjects\Sonic Adventure 2\build\bind_fixed.obj")
    write_grouped(v, f, fn, r"F:\ClaudeProjects\Sonic Adventure 2\build\bind_fixed_grp.obj")
    print(f"BIND fixed: tris={len(f)} animNodes={na}")
    # posed with the longest motion
    longest = None
    for mi, ncount, moff in mtn_tbl:
        m = read_motion(mtn, moff, ncount)
        if m and (longest is None or m.frame_count > longest.frame_count): longest = m
    v2, f2, fn2, _ = build(nf, ptr0, longest, frame)
    write_obj(v2, f2, r"F:\ClaudeProjects\Sonic Adventure 2\build\posed_fixed.obj")
    print(f"POSED fixed (frame {frame}): tris={len(f2)}")
