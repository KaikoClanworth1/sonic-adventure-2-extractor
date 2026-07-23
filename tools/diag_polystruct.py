"""Dump each node's poly-chunk TYPE sequence for Sonic model-0, in tree order,
revealing CacheList(4)/DrawList(5) structure and where strips actually live."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress
from eventmodel import NinjaFile
from mdlmtn import read_mdl_table, read_mtn_table
from build import valid_attach
from chunk import _u16be, _u32be

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC"


def scan_poly_types(data, off):
    """Return list of (head, flag, size_bytes) for a poly chunk stream."""
    n = len(data); out = []
    for _ in range(8192):
        if off + 2 > n: break
        w = _u16be(data, off); head = w & 0xFF; flag = w >> 8
        if head == 0xFF: break
        if head == 0:
            off += 2; continue
        if 1 <= head <= 5:                 # bits (incl CacheList=4, DrawList=5)
            out.append((head, flag, 2)); off += 2; continue
        if head in (8, 9):
            out.append((head, flag, 4)); off += 4; continue
        if 16 <= head <= 31:
            size = _u16be(data, off + 2); out.append((head, flag, 4 + size*2)); off += 4 + size*2; continue
        if 56 <= head <= 58:
            size = _u16be(data, off + 2); out.append((head, flag, 4 + size*2)); off += 4 + size*2; continue
        if 64 <= head <= 75:
            size = _u16be(data, off + 2)
            hdr = _u16be(data, off + 4); nb = hdr & 0x3FFF
            out.append((head, flag, 4 + size*2, nb)); off += 4 + size*2; continue
        size = _u16be(data, off + 2); out.append((head, flag, 4 + size*2)); off += 4 + size*2
    return out


def label(head):
    if head == 4: return "CACHE"
    if head == 5: return "DRAW"
    if 1 <= head <= 3: return "bits"
    if head in (8, 9): return "tex"
    if 16 <= head <= 31: return "mat"
    if 64 <= head <= 75: return "STRIP"
    return f"t{head}"


if __name__ == "__main__":
    mdl = bytes(prs_decompress(open(os.path.join(ROOT, "sonicmdl.prs"), "rb").read()))
    nf = NinjaFile(mdl, base=0)
    mtbl = read_mdl_table(mdl)
    want = read_mtn_table(bytes(prs_decompress(open(os.path.join(ROOT,"sonicmtn.prs"),"rb").read())))[0][1]
    # model 0 (matching, most nodes==want)
    ptr0 = next(ptr for idx, ptr in mtbl
                if len(nf.read_tree(ptr)) == want or
                sum(1 for nd in nf.read_tree(ptr) if not (nd.flags & 0x40)) == want)
    nodes = nf.read_tree(ptr0)
    # traversal order (pre-order)
    order = []
    def visit(nd):
        order.append(nd)
        for ch in nd.children: visit(ch)
    for r in [n for n in nodes if n.parent is None]: visit(r)

    print(f"root node index={order[0].index}, {len(nodes)} nodes")
    print(f"{'ord':>3} {'node':>4} {'par':>4} {'vtx':>4}  poly-chunks")
    for oi, nd in enumerate(order):
        has_v = "-"; ntypes = ""
        if valid_attach(nf, nd.attach_ptr):
            vl = nf.u32(nf.off(nd.attach_ptr))
            pl = nf.u32(nf.off(nd.attach_ptr) + 4)
            has_v = "V" if (0 < vl < len(nf.d)) else "-"
            if 0 < pl < len(nf.d):
                seq = scan_poly_types(nf.d, nf.off(pl))
                parts = []
                for e in seq:
                    h, fl = e[0], e[1]
                    if h in (4, 5): parts.append(f"{label(h)}:{fl}")
                    elif h >= 64: parts.append(f"{label(h)}({e[3]})")
                    elif 16 <= h <= 31 or h in (8,9): parts.append(label(h))
                ntypes = " ".join(parts)
        par = nd.parent.index if nd.parent else -1
        if ntypes or has_v == "V":
            print(f"{oi:>3} {nd.index:>4} {par:>4} {has_v:>4}  {ntypes}")
