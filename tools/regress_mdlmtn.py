"""Batch regression over every character MDL and MTN container."""
import os, sys, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress
from eventmodel import NinjaFile
from mdlmtn import read_mdl_table, read_mtn_table, read_motion, count_animated
from chunk import VERTEX_TYPES, STRIP_TYPES

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC"


def find(pat):
    out = []
    for dp, dn, fns in os.walk(ROOT):
        for fn in fns:
            if fn.lower().endswith(pat):
                out.append(os.path.join(dp, fn))
    return sorted(out)


def main():
    print("=" * 78)
    print("MDL (character model) REGRESSION")
    print("=" * 78)
    tot_models = tot_nodes = tot_verts = tot_tris = 0
    weighted_total = 0
    vtypes = collections.Counter()
    mdl_nodecounts = {}
    errs = []
    for p in find("mdl.prs"):
        name = os.path.basename(p)
        data = bytes(prs_decompress(open(p, "rb").read()))
        nf = NinjaFile(data, base=0)
        tbl = read_mdl_table(data)
        nmodels = nnodes = nverts = ntris = nweighted = 0
        for idx, ptr in tbl:
            try:
                nodes = nf.read_tree(ptr)
            except Exception as e:
                errs.append((name, idx, str(e)))
                continue
            if not nodes:
                continue
            nmodels += 1
            nnodes += len(nodes)
            for nd in nodes:
                if not nf.ok(nd.attach_ptr):
                    continue
                try:
                    m = nf.read_model(nd.attach_ptr)
                except Exception as e:
                    errs.append((name, f"node@{nd.offset:x}", str(e)))
                    continue
                if m is None:
                    continue
                nverts += len(m.vertices)
                for v in m.vertices.values():
                    if v.weight != 1.0 or v.node_index != 0:
                        nweighted += 1
                for pc in m.polys:
                    for st in pc.strips:
                        ntris += max(0, len(st.indices) - 2)
        mdl_nodecounts[name] = nnodes
        tot_models += nmodels; tot_nodes += nnodes
        tot_verts += nverts; tot_tris += ntris
        weighted_total += nweighted
        print(f"  {name:20s} entries={len(tbl):4d} trees={nmodels:4d} nodes={nnodes:5d} "
              f"verts={nverts:7,d} tris={ntris:7,d} weightedV={nweighted:6d}")

    print(f"\n  TOTAL: models={tot_models} nodes={tot_nodes} verts={tot_verts:,} "
          f"tris={tot_tris:,} weightedVerts={weighted_total:,}")

    print()
    print("=" * 78)
    print("MTN (motion) REGRESSION")
    print("=" * 78)
    tot_mot = tot_keys = 0
    bad = 0
    interp = collections.Counter()
    types = collections.Counter()
    for p in find("mtn.prs"):
        name = os.path.basename(p)
        data = bytes(prs_decompress(open(p, "rb").read()))
        tbl = read_mtn_table(data)
        nm = nk = 0
        frames_total = 0
        for idx, cnt, ptr in tbl:
            m = read_motion(data, ptr, cnt)
            if m is None:
                bad += 1
                continue
            nm += 1
            frames_total += m.frame_count
            types[m.type] += 1
            interp[m.interpolation] += 1
            for d in (m.pos, m.rot, m.scale):
                for keys in d.values():
                    nk += len(keys)
        tot_mot += nm; tot_keys += nk
        print(f"  {name:20s} entries={len(tbl):4d} motions={nm:4d} keys={nk:7,d} "
              f"frames={frames_total:6d}")
    print(f"\n  TOTAL: motions={tot_mot} keyframes={tot_keys:,} unreadable={bad}")
    print(f"  anim type flags: { {hex(k): v for k, v in types.most_common(8)} }")
    print(f"  interpolation  : {dict(interp)}")
    if errs:
        print(f"\n  errors ({len(errs)}), first 10:")
        for e in errs[:10]:
            print("   ", e)


if __name__ == "__main__":
    main()
