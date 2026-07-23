"""Do exe model roots and texlists get paired by an object-definition struct?
For a few known model roots, find data references to them and dump the nearby
words, flagging which look like texlist pointers, model roots or name strings."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pe as pemod
from exetex import find_texlists

ROOTS = [0x00a7e6ac, 0x0101dab4, 0x01701e44, 0x00caea5c, 0x013f62f8]

pe = pemod.PE(open(sys.argv[1], "rb").read())
flat = pe.flat_image()
base = pe.image_base
texlist_vas = set(va for va, cnt, names in find_texlists(pe, flat))
print(f"{len(texlist_vas)} texlist VAs known")


def classify(va):
    if va in texlist_vas: return "TEXLIST"
    off = pe.va_to_off(va)
    if off is None: return ""
    # ascii string?
    s = flat[va - base: va - base + 8]
    if all(0x20 <= c <= 0x7E for c in s[:3]): return "str?"
    return "ptr"


def refs_in_data(target):
    hits = []
    for s in pe.sections:
        if s["name"] == ".text" or not (s["chars"] & 0x40000000): continue
        lo, hi = s["vaddr"], s["vaddr"] + s["rsize"] - 4
        o = lo
        while o < hi:
            if struct.unpack_from("<I", flat, o)[0] == target:
                hits.append(base + o)
            o += 4
    return hits


for root in ROOTS:
    hits = refs_in_data(root)
    print(f"\nmodel 0x{root:08x}: {len(hits)} data refs")
    for h in hits[:3]:
        o = h - base   # flat is indexed by RVA
        print(f"  ref@0x{h:08x}:")
        for j in range(-4, 8):
            w = struct.unpack_from("<I", flat, o + j * 4)[0]
            mark = "  <-- ref (=model root)" if j == 0 else ""
            print(f"    [{j*4:+3d}] 0x{w:08x} {classify(w):8}{mark}")
