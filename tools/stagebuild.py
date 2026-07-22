"""Extract a full SA2 stage from a relocated REL: landtable -> COLs -> GC models."""
import sys, os, struct, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from relflat import RelImage
from gcmodel import GCReader, parse_gc_attach
from eventmodel import NinjaFile
from build import mat_from_srt, mat_mul, xform_point

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC"
ONE = 0x3F800000


def U32(d, o): return struct.unpack_from(">I", d, o)[0]
def I32(d, o): return struct.unpack_from(">i", d, o)[0]
def I16(d, o): return struct.unpack_from(">h", d, o)[0]
def F32(d, o): return struct.unpack_from(">f", d, o)[0]


def read_cstr(d, o):
    e = o
    while e < len(d) and d[e] != 0:
        e += 1
    return d[o:e].decode("latin-1", "replace")


def find_landtables(d, n, base):
    """Signature scan: a LandTable has a valid COL array with sane spheres."""
    out = []
    for o in range(0, n - 0x20, 4):
        colcount = I16(d, o)
        if colcount <= 0 or colcount > 20000:
            continue
        vis = I16(d, o + 2)
        if vis < -1:
            continue
        clip = F32(d, o + 0x0C)
        if not (1.0 <= clip <= 1e7) or clip != clip:
            continue
        colptr = U32(d, o + 0x10)
        if not (base <= colptr < base + n):
            continue
        texname_ptr = U32(d, o + 0x18)
        texlist_ptr = U32(d, o + 0x1C)
        if texname_ptr and not (base <= texname_ptr < base + n):
            continue
        c = colptr - base
        if c + colcount * 0x20 > n:
            continue
        good = 0
        checks = min(colcount, 8)
        for k in range(checks):
            e = c + k * 0x20
            r = F32(d, e + 0x0C)
            obj = U32(d, e + 0x10)
            if not (0.0 <= r < 1e7) or r != r:
                break
            if obj and not (base <= obj < base + n):
                break
            good += 1
        if good < checks:
            continue
        texname = read_cstr(d, texname_ptr - base) if texname_ptr else ""
        out.append(dict(off=o, colcount=colcount, vis=vis, clip=clip,
                        colptr=colptr, texname=texname))
    # keep the landtable with the most COLs (the main stage) plus named ones
    out.sort(key=lambda x: -x["colcount"])
    return out


def build_stage(path, obj_out=None, max_cols=None):
    raw = open(path, "rb").read()
    img = RelImage(raw)
    img.apply_relocations()
    d = img.data()
    n = len(d)
    base = img.vbase
    r = GCReader(d, base)
    nf = NinjaFile(d, base)

    lts = find_landtables(d, n, base)
    if not lts:
        print("no landtable found")
        return
    lt = lts[0]
    print(f"landtable @0x{lt['off']:06x} cols={lt['colcount']} "
          f"visible={lt['vis']} clip={lt['clip']:.0f} tex='{lt['texname']}'")

    colbase = img.vbase + (lt["colptr"] - base)
    total_tris = total_verts = 0
    gc_models = basic_models = 0
    verts_out = []
    faces_out = []

    draws_total = draws_empty = tris_dropped = 0
    ncols = lt["colcount"] if max_cols is None else min(lt["colcount"], max_cols)
    for i in range(ncols):
        e = (lt["colptr"] - base) + i * 0x20
        obj_ptr = U32(d, e + 0x10)
        flags = U32(d, e + 0x1C)
        if not r.ok(obj_ptr):
            continue
        # COL index < visible => GC visual model; >= visible => Basic collision
        is_visual = (lt["vis"] < 0) or (i < lt["vis"])
        if not is_visual:
            basic_models += 1
            continue
        # walk the node tree; each node's attach is a GC attach
        nodes = nf.read_tree(obj_ptr)
        for nd in nodes:
            if not r.ok(nd.attach_ptr):
                continue
            att = parse_gc_attach(r, nd.attach_ptr)
            if att is None:
                continue
            gc_models += 1
            # world transform
            local = mat_from_srt(nd.pos, nd.rot, nd.scale, bool(nd.flags & 0x20))
            world = local if nd.parent is None else mat_mul(local, nd.parent.world)
            nd.world = world
            positions = att["sets"].get(1)
            if not positions or not positions.values:
                continue
            wpos = [xform_point(world, p) for p in positions.values]
            vbase_idx = len(verts_out)
            verts_out.extend(wpos)
            total_verts += len(wpos)
            for draw in att["draws"]:
                draws_total += 1
                if not draw.tris:
                    draws_empty += 1
                for tri in draw.tris:
                    a, b, cc = tri
                    ia, ib, ic = a[0], b[0], cc[0]
                    if ia < 0 or ib < 0 or ic < 0:
                        tris_dropped += 1
                        continue
                    if max(ia, ib, ic) >= len(wpos):
                        tris_dropped += 1
                        continue
                    faces_out.append((vbase_idx + ia, vbase_idx + ib, vbase_idx + ic))
                    total_tris += 1

    print(f"  GC visual models: {gc_models}   Basic collision: {basic_models}")
    print(f"  draws: {draws_total}  empty: {draws_empty}  tris dropped: {tris_dropped}")
    print(f"  vertices: {total_verts:,}   triangles: {total_tris:,}")
    if verts_out:
        lo = [min(v[k] for v in verts_out) for k in range(3)]
        hi = [max(v[k] for v in verts_out) for k in range(3)]
        print(f"  bounds: ({lo[0]:.1f},{lo[1]:.1f},{lo[2]:.1f}) .. "
              f"({hi[0]:.1f},{hi[1]:.1f},{hi[2]:.1f})")

    if obj_out and verts_out:
        with open(obj_out, "w") as f:
            for v in verts_out:
                f.write(f"v {v[0]:.4f} {v[1]:.4f} {v[2]:.4f}\n")
            for a, b, c in faces_out:
                f.write(f"f {a+1} {b+1} {c+1}\n")
        print(f"  wrote {obj_out}")


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "stg13D.rel")
    obj = sys.argv[2] if len(sys.argv) > 2 else None
    build_stage(path, obj)
