"""Regression v2: strict validation + shared vertex cache + geometry sanity."""
import os, sys, struct, collections, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress
from eventmodel import NinjaFile, EVENT_BASE
from mdlmtn import read_mdl_table, read_mtn_table, read_motion
from build import build_model, valid_attach

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC"


def find(suffix, sub=""):
    base = os.path.join(ROOT, sub) if sub else ROOT
    out = []
    for dp, dn, fns in os.walk(base):
        for fn in fns:
            if fn.lower().endswith(suffix):
                out.append(os.path.join(dp, fn))
    return sorted(out)


def bbox(parts):
    lo = [1e30] * 3; hi = [-1e30] * 3
    n = 0
    for p in parts:
        for v in p.positions:
            n += 1
            for i in range(3):
                if v[i] < lo[i]: lo[i] = v[i]
                if v[i] > hi[i]: hi[i] = v[i]
    if n == 0:
        return None
    return lo, hi


def main():
    print("=" * 78)
    print("CHARACTER MDL — strict parse")
    print("=" * 78)
    grand_tris = grand_verts = grand_models = 0
    bad_geo = 0
    for p in find("mdl.prs"):
        name = os.path.basename(p)
        data = bytes(prs_decompress(open(p, "rb").read()))
        nf = NinjaFile(data, base=0)
        tbl = read_mdl_table(data)
        tris = verts = 0
        nmodels = 0
        allparts = []
        for idx, ptr in tbl:
            try:
                nodes, parts = build_model(nf, ptr)
            except Exception as e:
                continue
            if not parts:
                continue
            nmodels += 1
            allparts.extend(parts)
            for pt in parts:
                verts += len(pt.positions)
                tris += len(pt.indices) // 3
        bb = bbox(allparts)
        ok = "ok"
        if bb:
            lo, hi = bb
            span = max(hi[i] - lo[i] for i in range(3))
            if span > 5000 or span <= 0 or any(math.isnan(x) for x in lo + hi):
                ok = "SUSPECT"
                bad_geo += 1
            bbs = f"span={span:8.2f}"
        else:
            bbs = "no geometry"
        grand_tris += tris; grand_verts += verts; grand_models += nmodels
        print(f"  {name:20s} models={nmodels:3d} verts={verts:7,d} tris={tris:7,d}  {bbs}  {ok}")
    print(f"\n  TOTAL models={grand_models} verts={grand_verts:,} tris={grand_tris:,} "
          f"suspect={bad_geo}")

    print()
    print("=" * 78)
    print("EVENT files — strict parse")
    print("=" * 78)
    ev = [f for f in find(".prs", "event")
          if os.path.basename(f).startswith("e")
          and "_" not in os.path.basename(f)
          and "texture" not in os.path.basename(f)
          and "texlist" not in os.path.basename(f)
          and not os.path.basename(f).startswith("me")]
    et = ev_v = 0
    ev_models = 0
    ONE = 0x3F800000
    for p in ev[:20]:
        name = os.path.basename(p)
        data = bytes(prs_decompress(open(p, "rb").read()))
        nf = NinjaFile(data, EVENT_BASE)
        # roots = object pointers whose attach validates
        roots = set()
        d, n, base = nf.d, nf.n, nf.base
        for o in range(0, n - 3, 4):
            v = struct.unpack_from(">I", d, o)[0]
            if not (base <= v < base + n): continue
            t = v - base
            if t + 0x34 > n: continue
            if not (struct.unpack_from(">I", d, t + 0x20)[0] == ONE and
                    struct.unpack_from(">I", d, t + 0x24)[0] == ONE and
                    struct.unpack_from(">I", d, t + 0x28)[0] == ONE):
                continue
            if valid_attach(nf, struct.unpack_from(">I", d, t + 4)[0]):
                roots.add(v)
        tris = verts = 0
        used = set()
        cnt = 0
        for r in sorted(roots):
            if r in used: continue
            try:
                nodes, parts = build_model(nf, r)
            except Exception:
                continue
            for nd in nodes: used.add(nd.ptr)
            if not parts: continue
            cnt += 1
            for pt in parts:
                verts += len(pt.positions); tris += len(pt.indices) // 3
        et += tris; ev_v += verts; ev_models += cnt
        print(f"  {name:16s} roots={len(roots):4d} models={cnt:4d} "
              f"verts={verts:7,d} tris={tris:7,d}")
    print(f"\n  TOTAL (first 20) models={ev_models} verts={ev_v:,} tris={et:,}")


if __name__ == "__main__":
    main()
