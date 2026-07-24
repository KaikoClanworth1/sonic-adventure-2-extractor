"""Chao files are GameCube REL v2 modules (the .prs ones are just PRS-compressed).
Applying the SELF-module relocation block turns intra-module pointers into file
offsets, which is what fills in the NULL vertex-set data pointers.

Usage: chaorel.py <file.prs|.rel>   -> reports relocation + descriptor results
"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress


def load(path):
    raw = open(path, "rb").read()
    if path.lower().endswith(".prs"):
        return bytearray(prs_decompress(raw))
    return bytearray(raw)


def relocate(d):
    """Apply the self-module relocations in place. Returns (id, sections)."""
    n = len(d)
    u32 = lambda o: struct.unpack_from(">I", d, o)[0]
    u16 = lambda o: struct.unpack_from(">H", d, o)[0]
    mid = u32(0)
    nsec, secoff = u32(0x0C), u32(0x10)
    impoff, impsize = u32(0x28), u32(0x2C)
    # section file offsets (low bits are flags)
    sec = []
    for i in range(nsec):
        a = secoff + i * 8
        sec.append((u32(a) & ~3, u32(a + 4)))
    # find the self-module relocation block
    relptr = None
    for i in range(impsize // 8):
        a = impoff + i * 8
        if u32(a) == mid:
            relptr = u32(a + 4)
    if relptr is None:
        return mid, sec, 0
    applied = 0
    o = relptr
    pos = 0
    cur = 0
    while o + 8 <= n:
        off = u16(o); typ = d[o + 2]; s = d[o + 3]
        add = u32(o + 4)
        o += 8
        if typ == 0xCB:                      # R_DOLPHIN_END
            break
        if typ == 0xCA:                      # R_DOLPHIN_SECTION
            cur = s
            pos = 0
            continue
        pos += off
        if typ == 0xC9:                      # R_DOLPHIN_NOP
            continue
        if cur >= len(sec) or s >= len(sec):
            continue
        site = sec[cur][0] + pos
        value = sec[s][0] + add             # self-module: section base + addend
        if site + 4 > n:
            continue
        if typ == 1:                         # R_PPC_ADDR32
            struct.pack_into(">I", d, site, value & 0xFFFFFFFF)
            applied += 1
        elif typ == 4:                       # ADDR16_LO
            struct.pack_into(">H", d, site, value & 0xFFFF)
            applied += 1
        elif typ == 6:                       # ADDR16_HA
            v = ((value >> 16) + (1 if (value & 0x8000) else 0)) & 0xFFFF
            struct.pack_into(">H", d, site, v)
            applied += 1
        elif typ == 10:                      # REL24 (code, ignore for data)
            pass
    return mid, sec, applied


if __name__ == "__main__":
    d = load(sys.argv[1])
    n = len(d)
    u32 = lambda o: struct.unpack_from(">I", d, o)[0]
    u16 = lambda o: struct.unpack_from(">H", d, o)[0]
    # descriptors BEFORE relocation
    vs = [x for x in range(0, n - 16, 4) if d[x] == 1 and d[x + 1] == 12 and u32(x + 4) == 0x41]
    before = sum(1 for V in vs if u32(V + 8) != 0)
    mid, sec, applied = relocate(d)
    after = sum(1 for V in vs if u32(V + 8) != 0)
    print(f"REL id={mid}, {len(sec)} sections, {applied} relocations applied")
    print(f"GC_POS descriptors: {len(vs)}  data_ptr non-null before={before} after={after}")
    for V in vs[:5]:
        p = u32(V + 8)
        ok = "OK" if 0 < p < n else "??"
        f = struct.unpack_from(">f", d, p)[0] if 0 < p < n - 4 else float("nan")
        print(f"   desc 0x{V:x} count={u16(V+2)} -> data 0x{p:x} {ok} first float {f:.3f}")
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build",
                       os.path.basename(sys.argv[1]).replace(".", "_") + ".reloc.bin")
    open(out, "wb").write(bytes(d))
    print("wrote", os.path.normpath(out))
