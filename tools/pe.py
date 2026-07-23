"""Minimal PE (Portable Executable) reader for sonic2app.exe, plus a scan for
Ninja NJS_OBJECT model roots embedded in its data sections.

An NJS_OBJECT is 0x34 bytes:
  0x00 u32 flags   0x04 u32 attach*  0x08 float pos[3]  0x14 int32 rot[3]
  0x20 float scale[3]  0x2C u32 child*  0x30 u32 sibling*
Pointers are absolute virtual addresses (image_base + RVA). find_model_roots
keys on scale == (1,1,1) plus a valid attach pointer; we test both endiannesses
because the PC port may or may not have byte-swapped the original GC data."""
import sys, os, struct


class PE:
    def __init__(self, data):
        self.d = data
        assert data[:2] == b"MZ", "not MZ"
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        assert data[e_lfanew:e_lfanew + 4] == b"PE\0\0", "not PE"
        coff = e_lfanew + 4
        self.machine = struct.unpack_from("<H", data, coff)[0]
        nsec = struct.unpack_from("<H", data, coff + 2)[0]
        opt_size = struct.unpack_from("<H", data, coff + 16)[0]
        opt = coff + 20
        magic = struct.unpack_from("<H", data, opt)[0]  # 0x10b PE32, 0x20b PE32+
        if magic == 0x10b:
            self.image_base = struct.unpack_from("<I", data, opt + 28)[0]
        else:
            self.image_base = struct.unpack_from("<Q", data, opt + 24)[0]
        sect = opt + opt_size
        self.sections = []
        for i in range(nsec):
            o = sect + i * 40
            name = data[o:o + 8].rstrip(b"\0").decode("latin1")
            vsize = struct.unpack_from("<I", data, o + 8)[0]
            vaddr = struct.unpack_from("<I", data, o + 12)[0]
            rsize = struct.unpack_from("<I", data, o + 16)[0]
            roff = struct.unpack_from("<I", data, o + 20)[0]
            chars = struct.unpack_from("<I", data, o + 36)[0]
            self.sections.append(dict(name=name, vaddr=vaddr, vsize=vsize,
                                      roff=roff, rsize=rsize, chars=chars))

    def flat_image(self):
        """A contiguous buffer where each section sits at its RVA, so a virtual
        address `va` maps to flat[va - image_base] (matches NinjaBlob's model)."""
        top = max(s["vaddr"] + max(s["vsize"], s["rsize"]) for s in self.sections)
        buf = bytearray(top)
        for s in self.sections:
            raw = self.d[s["roff"]:s["roff"] + s["rsize"]]
            buf[s["vaddr"]:s["vaddr"] + len(raw)] = raw
        return bytes(buf)

    def va_to_off(self, va):
        rva = va - self.image_base
        for s in self.sections:
            if s["vaddr"] <= rva < s["vaddr"] + max(s["vsize"], s["rsize"]):
                fo = s["roff"] + (rva - s["vaddr"])
                if fo < s["roff"] + s["rsize"]:
                    return fo
        return None

    def valid_va(self, va):
        return self.va_to_off(va) is not None


def scan_objects(pe, le=True):
    """Count 0x34-aligned NJS_OBJECT candidates: scale==(1,1,1), valid child or
    attach pointer, finite pos. Returns (count, sample_vas)."""
    d = pe.d
    u32 = "<I" if le else ">I"
    one = 0x3F800000
    def rd(o): return struct.unpack_from(u32, d, o)[0]
    def f32(o):
        v = rd(o)
        return struct.unpack("<f", struct.pack("<I", v))[0]
    hits = []
    for s in pe.sections:
        if not (s["chars"] & 0x40000000):  # readable data only
            continue
        if s["name"] in (".text", ".reloc"):
            continue
        base, end = s["roff"], s["roff"] + s["rsize"] - 0x34
        o = base
        while o < end:
            if rd(o + 0x20) == one and rd(o + 0x24) == one and rd(o + 0x28) == one:
                attach = rd(o + 4); child = rd(o + 0x2C); sib = rd(o + 0x30)
                okp = lambda p: p == 0 or pe.valid_va(p)
                if okp(attach) and okp(child) and okp(sib) and (attach or child):
                    px, py, pz = f32(o), f32(o + 8), f32(o + 0x0C)  # not exact fields; rough finite check
                    hits.append(pe.image_base + (s["vaddr"] + (o - s["roff"])))
            o += 4
    return len(hits), hits[:8]


if __name__ == "__main__":
    path = sys.argv[1]
    d = open(path, "rb").read()
    pe = PE(d)
    print(f"machine=0x{pe.machine:x} image_base=0x{pe.image_base:x} "
          f"sections={len(pe.sections)}")
    for s in pe.sections:
        print(f"  {s['name']:8} vaddr=0x{s['vaddr']:08x} vsize=0x{s['vsize']:08x} "
              f"roff=0x{s['roff']:08x} rsize=0x{s['rsize']:08x} chars=0x{s['chars']:08x}")
    for le in (True, False):
        n, sample = scan_objects(pe, le)
        print(f"NJS_OBJECT candidates ({'LE' if le else 'BE'}): {n}  "
              f"sample={[hex(v) for v in sample]}")
