"""Apply REL relocations, then hunt for LandTables / models in the fixed image."""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from relflat import RelImage, VBASE
from eventmodel import NinjaFile
from build import valid_attach

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC"
ONE = 0x3F800000


def U32(d, o): return struct.unpack_from(">I", d, o)[0]
def I32(d, o): return struct.unpack_from(">i", d, o)[0]
def I16(d, o): return struct.unpack_from(">h", d, o)[0]
def F32(d, o): return struct.unpack_from(">f", d, o)[0]


def gc_attach_valid(d, n, base, ptr):
    """GC 'Ginja' attach, 0x24 bytes:
       0x00 vertexSets, 0x04 skin(always 0 in SA2), 0x08 opaqueMeshes,
       0x0C translucentMeshes, 0x10 u16 opaqueCount, 0x12 u16 transCount,
       0x14 f32[3] centre, 0x20 f32 radius
    """
    if not (base <= ptr < base + n):
        return False
    a = ptr - base
    if a + 0x24 > n:
        return False
    vtx = U32(d, a)
    skin = U32(d, a + 4)
    opq = U32(d, a + 8)
    trn = U32(d, a + 0x0C)
    nopq = struct.unpack_from(">H", d, a + 0x10)[0]
    ntrn = struct.unpack_from(">H", d, a + 0x12)[0]
    r = F32(d, a + 0x20)
    if skin != 0:
        return False
    if not (base <= vtx < base + n):
        return False
    if nopq == 0 and ntrn == 0:
        return False
    if nopq > 4096 or ntrn > 4096:
        return False
    if nopq and not (base <= opq < base + n):
        return False
    if ntrn and not (base <= trn < base + n):
        return False
    if not (0.0 <= r < 1e7):
        return False
    # first vertex set must be a Position descriptor: attr=1, stride=12
    v = vtx - base
    if v + 16 > n:
        return False
    if d[v] != 1 or d[v + 1] != 12:
        return False
    if U32(d, v + 4) != 0x41:      # PositionXYZ / Float32
        return False
    return True


def landtable_candidates(d, n, base):
    """SA2 LandTable: i16 colCount, i16 chunkModelCount, ..., u32 COL array."""
    out = []
    for o in range(0, n - 0x24, 4):
        colcount = I16(d, o)
        if colcount <= 0 or colcount > 8192:
            continue
        colptr = U32(d, o + 0x10)
        if not (base <= colptr < base + n):
            continue
        c = colptr - base
        if c + 0x20 * min(colcount, 4) > n:
            continue
        # validate the first few COL entries
        good = 0
        for k in range(min(colcount, 6)):
            e = c + k * 0x20
            r = F32(d, e + 0x0C)
            obj = U32(d, e + 0x10)
            if not (0.0 <= r < 1e7):
                break
            if obj and not (base <= obj < base + n):
                break
            good += 1
        if good >= min(colcount, 4):
            out.append((o, colcount, I16(d, o + 2), colptr))
    return out


def scan(path, verbose=True):
    raw = open(path, "rb").read()
    img = RelImage(raw)
    applied = img.apply_relocations()
    d = img.data()
    n = len(d)
    base = img.vbase

    # count structural hits
    chunk_hits = gc_hits = 0
    nf = NinjaFile(d, base)
    obj_cand = 0
    for o in range(0, n - 3, 4):
        v = U32(d, o)
        if not (base <= v < base + n):
            continue
        t = v - base
        if t + 0x34 > n:
            continue
        if not (U32(d, t + 0x20) == ONE and U32(d, t + 0x24) == ONE and
                U32(d, t + 0x28) == ONE):
            continue
        obj_cand += 1
        att = U32(d, t + 4)
        if att == 0:
            continue
        if gc_attach_valid(d, n, base, att):
            gc_hits += 1
        elif valid_attach(nf, att):
            chunk_hits += 1

    lts = landtable_candidates(d, n, base)
    if verbose:
        print(f"=== {os.path.basename(path)} ===")
        print(f"  image {n:,} bytes, {applied:,} ADDR32 relocs applied")
        print(f"  reloc types: { {k: v for k, v in img.stats.most_common()} }")
        print(f"  NJS_OBJECT candidates : {obj_cand}")
        print(f"  GC (Ginja) attaches   : {gc_hits}")
        print(f"  Chunk attaches        : {chunk_hits}")
        print(f"  LandTable candidates  : {len(lts)}")
        for o, cc, cmc, cp in lts[:6]:
            print(f"    @0x{o:06x} colCount={cc} chunkModelCount={cmc} "
                  f"COL@0x{cp - base:06x}")
    return dict(name=os.path.basename(path), objs=obj_cand, gc=gc_hits,
                chunk=chunk_hits, lts=len(lts), applied=applied)


if __name__ == "__main__":
    if len(sys.argv) > 1:
        scan(sys.argv[1])
    else:
        for f in ["stg01D.rel", "stg13D.rel", "stg03D.rel"]:
            p = os.path.join(ROOT, f)
            if os.path.exists(p):
                scan(p)
                print()
