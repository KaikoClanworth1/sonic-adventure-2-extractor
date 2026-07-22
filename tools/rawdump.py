import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC\event"
fn, off, ln = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
p = fn if os.path.isabs(fn) else os.path.join(ROOT, fn)
data = bytes(prs_decompress(open(p, "rb").read())) if p.lower().endswith(".prs") else open(p, "rb").read()
print(f"{os.path.basename(p)} size=0x{len(data):x}")
c = data[off:off+ln]
for i in range(0, len(c), 16):
    r = c[i:i+16]
    be = " ".join(f"{struct.unpack_from('>I', r, k)[0]:08x}" for k in range(0, len(r)-3, 4))
    print(f"{off+i:08x}  " + " ".join(f"{b:02x}" for b in r).ljust(48) +
          "  |" + "".join(chr(b) if 32 <= b < 127 else "." for b in r) + "|")
