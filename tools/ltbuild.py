"""Try building every landtable candidate in a REL, both formats."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from relflat import RelImage
from gcmodel import GCReader, parse_gc_attach
from eventmodel import NinjaFile
from stagebuild import find_landtables
from build import build_model as build_chunk

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC"

path = os.path.join(ROOT, sys.argv[1])
raw = open(path, "rb").read()
img = RelImage(raw); img.apply_relocations()
d = img.data(); n = len(d); base = img.vbase
r = GCReader(d, base)
nf = NinjaFile(d, base)

lts = find_landtables(d, n, base)
print(f"{sys.argv[1]}: {len(lts)} candidates")
for lt in lts[:12]:
    cbase = lt["colptr"] - base
    gc_tris = chunk_tris = 0
    ncols = min(lt["colcount"], 3000)
    for i in range(ncols):
        e = cbase + i * 0x20
        obj = struct.unpack_from(">I", d, e + 0x10)[0]
        if not r.ok(obj):
            continue
        vis = (lt["vis"] < 0) or (i < lt["vis"])
        if not vis:
            continue
        # GC attempt
        nodes = nf.read_tree(obj)
        for nd in nodes:
            if not r.ok(nd.attach_ptr):
                continue
            att = parse_gc_attach(r, nd.attach_ptr)
            if att:
                for dr in att["draws"]:
                    gc_tris += len(dr.tris)
        # chunk attempt
        try:
            _, parts = build_chunk(nf, obj)
            for p in parts:
                chunk_tris += len(p.indices) // 3
        except Exception:
            pass
    print(f"  @0x{lt['off']:06x} cols={lt['colcount']:5d} vis={lt['vis']:5d} "
          f"tex='{lt['texname']:10s}' | gc_tris={gc_tris} chunk_tris={chunk_tris}")
