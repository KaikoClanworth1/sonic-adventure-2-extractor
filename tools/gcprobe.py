import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from relflat import RelImage
from gcmodel import GCReader
from eventmodel import NinjaFile
from stagebuild import find_landtables

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC"
path = os.path.join(ROOT, sys.argv[1])
raw = open(path, "rb").read()
img = RelImage(raw); img.apply_relocations()
d = img.data(); n = len(d); base = img.vbase
r = GCReader(d, base)
nf = NinjaFile(d, base)
lt = find_landtables(d, n, base)[0]
cbase = lt["colptr"] - base
shown = 0
for i in range(lt["colcount"]):
    e = cbase + i * 0x20
    obj = struct.unpack_from(">I", d, e + 0x10)[0]
    if not r.ok(obj):
        continue
    for nd in nf.read_tree(obj):
        if not r.ok(nd.attach_ptr):
            continue
        a = r.off(nd.attach_ptr)
        vtx = r.u32(a); skin = r.u32(a+4); ocnt = r.u16(a+0x10); tcnt = r.u16(a+0x12)
        if not r.ok(vtx):
            continue
        v = r.off(vtx)
        attr = d[v]; stride = d[v+1]; cnt = r.u16(v+2); structure = r.u32(v+4)
        print(f"COL {i}: attach@0x{a:06x} skin={skin} ocnt={ocnt} tcnt={tcnt} "
              f"| vset0 attr={attr} stride={stride} cnt={cnt} structure=0x{structure:08x} "
              f"(structType={structure&0xF} dataType={(structure>>4)&0xF})")
        shown += 1
        break
    if shown >= 6:
        break
