"""Scan sonic2app.exe for NJS_TEXLIST structures and their texture names.

NJS_TEXLIST  { NJS_TEXNAME* textures; Uint32 nbTexture; }             (8 bytes)
NJS_TEXNAME  { char* filename; void* texaddr; Uint32 attr; }         (12 bytes)

A texlist is findable as a {ptr, count} pair whose ptr points at `count`
12-byte entries, each starting with a pointer to a short ASCII texture name."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pe as pemod


def read_cstr(flat, off, maxlen=40):
    end = off
    while end < len(flat) and end - off < maxlen and flat[end] != 0:
        c = flat[end]
        if c < 0x20 or c > 0x7E:
            return None
        end += 1
    if end == off or (end < len(flat) and flat[end] != 0):
        return None
    return flat[off:end].decode("latin1")


def find_texlists(pe, flat):
    base = pe.image_base
    def ok(va): return pe.va_to_off(va) is not None
    def u32(o): return struct.unpack_from("<I", flat, o)[0]
    lists = []
    for s in pe.sections:
        if not (s["chars"] & 0x40000000) or s["name"] == ".text":
            continue
        lo, hi = s["vaddr"], s["vaddr"] + s["rsize"] - 8
        o = lo
        while o < hi:
            ptr = u32(o); cnt = u32(o + 4)
            if 1 <= cnt <= 255 and ok(ptr):
                arr = ptr - base
                names = []
                good = True
                for i in range(cnt):
                    eo = arr + i * 12
                    if eo + 12 > len(flat): good = False; break
                    nptr = u32(eo)
                    if nptr == 0:
                        names.append(""); continue
                    if not ok(nptr): good = False; break
                    nm = read_cstr(flat, nptr - base)
                    if nm is None or len(nm) < 1: good = False; break
                    names.append(nm)
                if good and any(names) and sum(1 for n in names if n) >= max(1, cnt // 2):
                    lists.append((base + o, cnt, names))
                    o += 8
                    continue
            o += 4
    return lists


if __name__ == "__main__":
    pe = pemod.PE(open(sys.argv[1], "rb").read())
    flat = pe.flat_image()
    lists = find_texlists(pe, flat)
    print(f"texlists found: {len(lists)}")
    total_names = set()
    for va, cnt, names in lists:
        for n in names:
            if n: total_names.add(n.lower())
    print(f"distinct texture names: {len(total_names)}")
    for va, cnt, names in lists[:12]:
        shown = ", ".join(n for n in names[:6] if n)
        print(f"  0x{va:08x} n={cnt}: {shown}")
    print("sample names:", sorted(list(total_names))[:30])
