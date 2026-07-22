import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress
from eventmodel import NinjaFile
from mdlmtn import read_mdl_table
from build import build_model, valid_attach, mat_from_srt, mat_mul

P = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC\sonicmdl.prs"
data = bytes(prs_decompress(open(P, "rb").read()))
nf = NinjaFile(data, base=0)
tbl = read_mdl_table(data)
idx, ptr = tbl[0]
print(f"model index {idx} @0x{ptr:x}")
nodes = nf.read_tree(ptr)
print(f"{len(nodes)} nodes\n")
print(f"{'i':>3} {'par':>4} {'flags':>8} {'attach':>8}  {'local pos':>26}  {'world pos':>26}")
for nd in nodes:
    local = mat_from_srt(nd.pos, nd.rot, nd.scale, bool(nd.flags & 0x20))
    nd.world = local if nd.parent is None else mat_mul(local, nd.parent.world)
    w = (nd.world[12], nd.world[13], nd.world[14])
    has = "yes" if valid_attach(nf, nd.attach_ptr) else "-"
    print(f"{nd.index:3d} {nd.parent.index if nd.parent else -1:4d} "
          f"{nd.flags:08x} {has:>8}  "
          f"({nd.pos[0]:7.2f},{nd.pos[1]:7.2f},{nd.pos[2]:7.2f})  "
          f"({w[0]:7.2f},{w[1]:7.2f},{w[2]:7.2f})")
    if nd.index > 24:
        print("   ...")
        break
