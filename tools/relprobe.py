"""Probe GameCube REL modules: sections, imports, relocation stream."""
import sys, os, struct, collections

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC"

def U32(d, o): return struct.unpack_from(">I", d, o)[0]
def U16(d, o): return struct.unpack_from(">H", d, o)[0]

REL_TYPES = {0: "NONE", 1: "ADDR32", 2: "ADDR24", 3: "ADDR16", 4: "ADDR16_LO",
             5: "ADDR16_HI", 6: "ADDR16_HA", 10: "REL24", 11: "REL14",
             201: "DOLPHIN_NOP", 202: "DOLPHIN_SECTION", 203: "DOLPHIN_END",
             204: "DOLPHIN_MRKREF"}


def probe(path, verbose=True):
    d = open(path, "rb").read()
    n = len(d)
    hdr = {
        "id": U32(d, 0x00), "next": U32(d, 0x04), "prev": U32(d, 0x08),
        "numSections": U32(d, 0x0C), "sectionInfoOffset": U32(d, 0x10),
        "nameOffset": U32(d, 0x14), "nameSize": U32(d, 0x18),
        "version": U32(d, 0x1C), "bssSize": U32(d, 0x20),
        "relOffset": U32(d, 0x24), "impOffset": U32(d, 0x28),
        "impSize": U32(d, 0x2C),
        "prologSection": d[0x30], "epilogSection": d[0x31],
        "unresolvedSection": d[0x32], "bssSection": d[0x33],
        "prolog": U32(d, 0x34), "epilog": U32(d, 0x38),
        "unresolved": U32(d, 0x3C),
    }
    if hdr["version"] >= 2:
        hdr["align"] = U32(d, 0x40)
        hdr["bssAlign"] = U32(d, 0x44)
    if hdr["version"] >= 3:
        hdr["fixSize"] = U32(d, 0x48)

    if verbose:
        print(f"=== {os.path.basename(path)}  ({n:,} bytes) ===")
        for k, v in hdr.items():
            print(f"  {k:20s} {v if isinstance(v,int) and v < 256 else hex(v)}")

    # sections
    secs = []
    so = hdr["sectionInfoOffset"]
    for i in range(hdr["numSections"]):
        raw = U32(d, so + i * 8)
        size = U32(d, so + i * 8 + 4)
        exec_ = bool(raw & 1)
        off = raw & ~3
        secs.append({"i": i, "off": off, "size": size, "exec": exec_})
    if verbose:
        print(f"  -- {len(secs)} sections --")
        for s in secs:
            kind = "exec" if s["exec"] else ("bss " if s["off"] == 0 and s["size"] else "data")
            if s["off"] or s["size"]:
                print(f"    [{s['i']:2d}] off=0x{s['off']:08x} size=0x{s['size']:08x} {kind}")

    # imports
    imps = []
    io_ = hdr["impOffset"]
    for i in range(hdr["impSize"] // 8):
        mod = U32(d, io_ + i * 8)
        off = U32(d, io_ + i * 8 + 4)
        imps.append((mod, off))
    if verbose:
        print(f"  -- {len(imps)} imports --")
        for mod, off in imps:
            print(f"    module {mod:5d} relocs@0x{off:08x}")

    # relocation stream stats
    counts = collections.Counter()
    self_addr32 = 0
    for mod, off in imps:
        p = off
        cur_sec = 0
        guard = 0
        while p + 8 <= n:
            guard += 1
            if guard > 4_000_000:
                break
            skip = U16(d, p)
            rtype = d[p + 2]
            sect = d[p + 3]
            addend = U32(d, p + 4)
            counts[REL_TYPES.get(rtype, f"?{rtype}")] += 1
            if rtype == 203:      # DOLPHIN_END
                break
            if rtype == 1 and mod == hdr["id"]:
                self_addr32 += 1
            p += 8
    if verbose:
        print(f"  -- relocations --")
        for t, c in counts.most_common():
            print(f"    {t:18s} {c:,}")
        print(f"    self-module ADDR32: {self_addr32:,}")
    return hdr, secs, imps, counts


if __name__ == "__main__":
    if len(sys.argv) > 1:
        probe(sys.argv[1])
    else:
        # summary across all stage RELs
        files = sorted(f for f in os.listdir(ROOT) if f.lower().endswith(".rel"))
        print(f"{len(files)} REL files\n")
        probe(os.path.join(ROOT, "stg01D.rel" if "stg01D.rel" in files else files[0]))
        print()
        print("=== versions / section counts across all RELs ===")
        vers = collections.Counter()
        nsec = collections.Counter()
        for f in files:
            d = open(os.path.join(ROOT, f), "rb").read(0x50)
            vers[U32(d, 0x1C)] += 1
            nsec[U32(d, 0x0C)] += 1
        print("  versions:", dict(vers))
        print("  section counts:", dict(nsec))
