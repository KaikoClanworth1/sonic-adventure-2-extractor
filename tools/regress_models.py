"""Batch regression: parse every chunk model reachable in every event file."""
import os, sys, struct, collections, traceback
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress
from eventmodel import NinjaFile, EVENT_BASE
from chunk import VERTEX_TYPES, STRIP_TYPES

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC\event"
ONE = 0x3F800000


def object_candidates(nf):
    """Pointers that structurally look like NJS_OBJECT roots."""
    d, n, base = nf.d, nf.n, nf.base
    cands = set()
    for o in range(0, n - 3, 4):
        v = struct.unpack_from(">I", d, o)[0]
        if not (base <= v < base + n):
            continue
        t = v - base
        if t + 0x34 > n:
            continue
        if (struct.unpack_from(">I", d, t + 0x20)[0] == ONE and
            struct.unpack_from(">I", d, t + 0x24)[0] == ONE and
            struct.unpack_from(">I", d, t + 0x28)[0] == ONE):
            cands.add(v)
    return cands


def main():
    files = sorted(f for f in os.listdir(ROOT)
                   if f.lower().endswith(".prs") and "_" not in f
                   and "texture" not in f and "texlist" not in f
                   and f.startswith("e") and not f.startswith("me"))
    tot_models = tot_tris = tot_verts = 0
    fail_v = fail_p = 0
    vtypes = collections.Counter()
    ptypes = collections.Counter()
    files_ok = 0
    weighted_models = 0
    errors = []
    per_file = []

    for fn in files:
        try:
            raw = prs_decompress(open(os.path.join(ROOT, fn), "rb").read())
        except Exception as e:
            errors.append((fn, "PRS", str(e)))
            continue
        nf = NinjaFile(bytes(raw), EVENT_BASE)
        cands = object_candidates(nf)
        fm = ft = fv = 0
        seen_attach = set()
        for p in cands:
            try:
                tree = nf.read_tree(p)
            except Exception as e:
                continue
            for nd in tree:
                if not nf.ok(nd.attach_ptr) or nd.attach_ptr in seen_attach:
                    continue
                seen_attach.add(nd.attach_ptr)
                try:
                    m = nf.read_model(nd.attach_ptr)
                except Exception as e:
                    fail_v += 1
                    if len(errors) < 40:
                        errors.append((fn, f"attach 0x{nd.attach_ptr:08x}", str(e)))
                    continue
                if m is None or not m.vertices:
                    continue
                fm += 1
                fv += len(m.vertices)
                maxidx = max(m.vertices) if m.vertices else -1
                has_w = any(v.weight != 1.0 or v.node_index != 0
                            for v in m.vertices.values())
                if has_w:
                    weighted_models += 1
                for pc in m.polys:
                    ptypes[pc.chunk_type] += 1
                    for st in pc.strips:
                        ft += max(0, len(st.indices) - 2)
                        # index range check
                        for i in st.indices:
                            if i > maxidx:
                                fail_p += 1
                                break
                        else:
                            continue
                        break
        tot_models += fm
        tot_tris += ft
        tot_verts += fv
        if fm:
            files_ok += 1
        per_file.append((fn, fm, fv, ft))

    print("=" * 74)
    print("EVENT CHUNK-MODEL REGRESSION")
    print("=" * 74)
    print(f"  event files scanned : {len(files)}")
    print(f"  files with models   : {files_ok}")
    print(f"  models parsed       : {tot_models}")
    print(f"  models w/ weights   : {weighted_models}")
    print(f"  vertices            : {tot_verts:,}")
    print(f"  triangles           : {tot_tris:,}")
    print(f"  vertex-chunk errors : {fail_v}")
    print(f"  index-range errors  : {fail_p}")
    print(f"  strip chunk types   : "
          f"{ {STRIP_TYPES.get(k,k): v for k, v in ptypes.most_common(10)} }")
    print()
    print("  top files:")
    for fn, fm, fv, ft in sorted(per_file, key=lambda r: -r[3])[:12]:
        print(f"    {fn:16s} models={fm:4d} verts={fv:7,d} tris={ft:7,d}")
    if errors:
        print()
        print("  first errors:")
        for e in errors[:12]:
            print(f"    {e}")


if __name__ == "__main__":
    main()
