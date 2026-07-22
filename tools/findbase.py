"""Find the GameCube load base address of an SA2 event file.

Pointers in the file are absolute GC RAM addresses. We recover the base by
choosing B that maximises the number of 4-aligned BE u32 words that land
inside [B, B+filesize).
"""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress

def analyse(data, label=""):
    n = len(data)
    words = []
    for o in range(0, n - 3, 4):
        v = struct.unpack_from(">I", data, o)[0]
        if 0x80000000 <= v < 0x81800000:
            words.append(v)
    if not words:
        print(f"{label}: no GC pointers found")
        return None
    lo, hi = min(words), max(words)
    # Candidate bases: assume the highest pointer is near end of file and the
    # lowest is near the start.  Score each candidate aligned base.
    best = None
    cand = set()
    for w in words:
        for delta in (0, 4, 8, 0x20):
            cand.add((w - delta) & ~0x1F)
    scored = []
    for B in cand:
        if B < 0x80000000:
            continue
        c = sum(1 for w in words if B <= w < B + n)
        scored.append((c, B))
    scored.sort(reverse=True)
    top = scored[0]
    print(f"{label}: size=0x{n:x} ptrs={len(words)} range=0x{lo:08x}..0x{hi:08x} "
          f"span=0x{hi-lo:x}")
    print(f"   best base 0x{top[1]:08x} covers {top[0]}/{len(words)} pointers")
    # the most useful base is simply hi_bound: base such that all fit
    B2 = (lo) & ~0xF
    c2 = sum(1 for w in words if B2 <= w < B2 + n)
    print(f"   base=min&~0xF 0x{B2:08x} covers {c2}/{len(words)}")
    return top[1]

if __name__ == "__main__":
    for p in sys.argv[1:]:
        with open(p, "rb") as f:
            raw = f.read()
        if p.lower().endswith(".prs"):
            raw = bytes(prs_decompress(raw))
        analyse(raw, os.path.basename(p))
        print()
