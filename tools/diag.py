import os, sys, struct, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress
from eventmodel import NinjaFile
from mdlmtn import read_mdl_table
from build import valid_attach
from chunk import VERTEX_TYPES

P = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC\cwalkmdl.prs"
data = bytes(prs_decompress(open(P, "rb").read()))
nf = NinjaFile(data, base=0)

for idx, ptr in read_mdl_table(data):
    nodes = nf.read_tree(ptr)
    for nd in nodes:
        if not valid_attach(nf, nd.attach_ptr):
            continue
        m = nf.read_model(nd.attach_ptr)
        if not m:
            continue
        for ci, v in m.vertices.items():
            if any(abs(c) > 1e6 or c != c for c in v.pos):
                a = nf.off(nd.attach_ptr)
                vl = nf.u32(a)
                o = nf.off(vl)
                w = nf.u32(o); w2 = nf.u32(o + 4)
                print(f"model idx={idx} node@0x{nd.offset:x} attach@0x{a:x}")
                print(f"  vlist@0x{o:x} head={w & 0xFF} ({VERTEX_TYPES.get(w & 0xFF)}) "
                      f"flag={(w>>8)&0xFF:02x} size={w>>16}")
                print(f"  idxOff={w2 & 0xFFFF} nbVtx={w2 >> 16}")
                print(f"  bad cache idx {ci}: {v.pos}")
                print("  raw vlist bytes:")
                for r in range(o, min(o + 96, nf.n), 16):
                    print("    " + " ".join(f"{b:02x}" for b in data[r:r+16]))
                sys.exit(0)
print("no bad vertices found")
