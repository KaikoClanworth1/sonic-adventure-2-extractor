import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from relflat import RelImage
from gcmodel import GCReader, parse_gc_attach
from eventmodel import NinjaFile
from stagebuild import find_landtables
from build import valid_attach

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC"

path = os.path.join(ROOT, sys.argv[1])
raw = open(path, "rb").read()
img = RelImage(raw); img.apply_relocations()
d = img.data(); n = len(d); base = img.vbase
r = GCReader(d, base)
nf = NinjaFile(d, base)

lts = find_landtables(d, n, base)
print(f"{sys.argv[1]}: {len(lts)} landtable candidates")
for lt in lts[:8]:
    # count how many visual COLs actually produce a GC attach
    cbase = lt["colptr"] - base
    gc = chunk = neither = 0
    ncheck = min(lt["colcount"], 60)
    for i in range(ncheck):
        e = cbase + i * 0x20
        obj = struct.unpack_from(">I", d, e + 0x10)[0]
        if not r.ok(obj):
            continue
        nodes = nf.read_tree(obj)
        for nd in nodes[:4]:
            if not r.ok(nd.attach_ptr):
                continue
            if parse_gc_attach(r, nd.attach_ptr):
                gc += 1
            elif valid_attach(nf, nd.attach_ptr):
                chunk += 1
            else:
                neither += 1
            break
    print(f"  @0x{lt['off']:06x} cols={lt['colcount']:5d} vis={lt['vis']:5d} "
          f"tex='{lt['texname']:10s}' | of {ncheck}: gc={gc} chunk={chunk} none={neither}")
