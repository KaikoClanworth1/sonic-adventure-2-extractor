import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress

BASE = 0x8125FE60
ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC\event"

fn = sys.argv[1]
data = bytes(prs_decompress(open(os.path.join(ROOT, fn), "rb").read()))
n = len(data)
def U8(o):  return data[o]
def U16(o): return struct.unpack_from(">H", data, o)[0]
def S16(o): return struct.unpack_from(">h", data, o)[0]
def U32(o): return struct.unpack_from(">I", data, o)[0]
def F32(o): return struct.unpack_from(">f", data, o)[0]

VTX = {32:"VertexSH",33:"VertexNormalSH",34:"Vertex",35:"VertexD8",36:"VertexUF",
       37:"VertexNF",38:"VertexDS5",39:"VertexDS4",40:"VertexDS16",41:"VertexNormal",
       42:"VertexNormalD8",43:"VertexNormalUF",44:"VertexNormalNF",45:"VertexNormalDS5",
       46:"VertexNormalDS4",47:"VertexNormalDS16",48:"VertexNormalX",49:"VertexNormalXD8",
       50:"VertexNormalXUF"}
STRIP = {64:"Strip",65:"StripUVN",66:"StripUVH",67:"StripNormal",68:"StripUVNNormal",
         69:"StripUVHNormal",70:"StripColor",71:"StripUVNColor",72:"StripUVHColor",
         73:"Strip2",74:"StripUVN2",75:"StripUVH2"}

def dump_vlist(o, limit=14):
    print(f"  VLIST @0x{o:06x}")
    i = 0
    while i < limit:
        head = U8(o); flags = U8(o+1); size = U16(o+2)
        if head == 255:
            print(f"    [end]"); break
        if head == 0:
            print(f"    Null"); o += 4; i += 1; continue
        name = VTX.get(head, f"?{head}")
        idxoff = U16(o+4); nbvtx = U16(o+6)
        print(f"    type={head:3d} {name:18s} flags={flags:02x} size={size:5d} "
              f"idxOff={idxoff:5d} nbVtx={nbvtx:5d}")
        # first vertex
        vo = o + 8
        vals = [F32(vo+k*4) for k in range(3)]
        print(f"       v0 pos=({vals[0]:.4f},{vals[1]:.4f},{vals[2]:.4f})")
        if head in (32, 33):
            w = U32(vo+12)
            print(f"       v0 extra=0x{w:08x} (weight/index for skinning)")
        o += 8 + size*4
        i += 1

def dump_plist(o, limit=24):
    print(f"  PLIST @0x{o:06x}")
    i = 0
    while i < limit:
        head = U8(o); flags = U8(o+1)
        if head == 255:
            print(f"    [end]"); break
        if head == 0:
            print(f"    Null"); o += 2; i += 1; continue
        if 1 <= head <= 5:
            print(f"    Bits type={head} flags={flags:02x}"); o += 2
        elif 8 <= head <= 9:
            print(f"    Tiny(TextureID) texid={U16(o+2)&0x1FFF} flags={flags:02x}"); o += 4
        elif 16 <= head <= 31:
            size = U16(o+2)
            print(f"    Material type={head} flags={flags:02x} size={size}")
            o += 4 + size*2
        elif 64 <= head <= 75:
            size = U16(o+2); ufo = U16(o+4)
            nstrip = ufo & 0x3FFF; uflags = (ufo >> 14) & 3
            print(f"    Strip type={head} {STRIP.get(head,'?'):16s} flags={flags:02x} "
                  f"size={size} nbStrip={nstrip} userFlags={uflags}")
            o += 4 + size*2
        else:
            size = U16(o+2)
            print(f"    ??? type={head} flags={flags:02x} size={size}")
            o += 4 + size*2
        i += 1

if len(sys.argv) > 2:
    att = int(sys.argv[2], 0) - BASE
    print(f"ATTACH @0x{att:06x}")
    vl = U32(att); pl = U32(att+4)
    c = [F32(att+8), F32(att+12), F32(att+16)]; r = F32(att+20)
    print(f"  vlist=0x{vl:08x} plist=0x{pl:08x} center=({c[0]:.3f},{c[1]:.3f},{c[2]:.3f}) r={r:.3f}")
    if BASE <= vl < BASE+n: dump_vlist(vl-BASE)
    if BASE <= pl < BASE+n: dump_plist(pl-BASE)
