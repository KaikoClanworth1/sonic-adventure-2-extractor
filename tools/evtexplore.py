import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress

BASE = 0x8125FE60
ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC\event"

fn = sys.argv[1] if len(sys.argv) > 1 else "e0000.prs"
data = bytes(prs_decompress(open(os.path.join(ROOT, fn), "rb").read()))
n = len(data)
print(f"{fn}: size 0x{n:x}  base 0x{BASE:08x}\n")

def off(p):
    return p - BASE

def show(o, count=48, label=""):
    print(f"--- {label} @ file 0x{o:x} (ptr 0x{o+BASE:08x}) ---")
    c = data[o:o + count]
    for i in range(0, len(c), 16):
        r = c[i:i + 16]
        print(f"  {o+i:08x}  " + " ".join(f"{b:02x}" for b in r).ljust(48) +
              "  |" + "".join(chr(b) if 32 <= b < 127 else "." for b in r) + "|")

# header words
print("HEADER:")
for i in range(0, 0x40, 4):
    v = struct.unpack_from(">I", data, i)[0]
    tag = ""
    if BASE <= v < BASE + n:
        tag = f"-> file 0x{v-BASE:x}"
    print(f"  +0x{i:02x}: 0x{v:08x}  {tag}")

print()
for i in range(0, 0x24, 4):
    v = struct.unpack_from(">I", data, i)[0]
    if BASE <= v < BASE + n:
        show(v - BASE, 64, f"header+0x{i:02x}")
        print()
