import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC\event"

def words_of(data):
    n = len(data)
    return [struct.unpack_from(">I", data, o)[0] for o in range(0, n - 3, 4)]

votes = collections.Counter()
files = [f for f in os.listdir(ROOT)
         if f.lower().endswith(".prs") and "_" not in f
         and "texture" not in f and "texlist" not in f and f.startswith("e")]
files.sort()
print(f"{len(files)} candidate event files")

for fn in files[:20]:
    data = bytes(prs_decompress(open(os.path.join(ROOT, fn), "rb").read()))
    n = len(data)
    ws = [w for w in words_of(data) if 0x81000000 <= w < 0x81800000]
    if not ws:
        continue
    best = (0, 0)
    for B in range(0x8125F000, 0x81261000, 4):
        c = sum(1 for w in ws if B <= w < B + n)
        if c > best[0]:
            best = (c, B)
    # widen: find the full plateau of bases achieving the max
    plateau = [B for B in range(0x8125F000, 0x81261000, 4)
               if sum(1 for w in ws if B <= w < B + n) == best[0]]
    print(f"{fn:16s} size=0x{n:06x} best={best[0]}/{len(ws)} "
          f"base plateau 0x{min(plateau):08x}..0x{max(plateau):08x}")
    votes[(min(plateau), max(plateau))] += 1

print()
print("plateau votes:", votes.most_common(6))
