"""Survey every Chao stage: REL-relocate it, then report which geometry format it
uses (GC Ginja vertex sets vs the Lobby's interleaved strips) and what texture
names it references. Decides how each area should be loaded."""
import sys, os, glob, struct, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from chaorel import load, relocate

GD = sys.argv[1]
for path in sorted(glob.glob(os.path.join(GD, "ChaoStg*.prs"))) + \
            sorted(glob.glob(os.path.join(GD, "ChaoStg*.rel"))):
    d = load(path)
    n = len(d)
    try:
        mid, sec, applied = relocate(d)
    except Exception as e:
        print(f"{os.path.basename(path):22s} relocate failed: {e}")
        continue
    u32 = lambda o: struct.unpack_from(">I", d, o)[0]
    u16 = lambda o: struct.unpack_from(">H", d, o)[0]
    f32 = lambda o: struct.unpack_from(">f", d, o)[0]
    ok = lambda p: p != 0 and p < n
    # GC Ginja attaches
    att = 0
    for a in range(0, n - 0x24, 4):
        vtx, skin = u32(a), u32(a + 4)
        oc, tc = u16(a + 0x10), u16(a + 0x12)
        r = f32(a + 0x20)
        if skin != 0 or not ok(vtx) or (oc == 0 and tc == 0): continue
        if oc > 4096 or tc > 4096: continue
        if oc and not ok(u32(a + 8)): continue
        if tc and not ok(u32(a + 0x0C)): continue
        if not math.isfinite(r) or r < 0 or r > 1e7: continue
        v = vtx
        if v + 16 > n or d[v] != 1 or d[v + 1] != 12 or u32(v + 4) != 0x41: continue
        att += 1
    # Lobby-style strip meshes (material word 0x25130004-ish after a 1,1,1,0,0,0 node)
    sig = [0x3F800000] * 3 + [0] * 3
    strips = 0
    for o in range(0, n - 0x30, 4):
        if [u32(o + i * 4) for i in range(6)] != sig: continue
        w = u32(o + 0x18)
        if (w >> 24) in (0x21, 0x25): strips += 1
    # texture names (NJS_TEXLIST of {name_ptr,0,0})
    names = set()
    for a in range(0, n - 8, 4):
        p, c = u32(a), u32(a + 4)
        if not ok(p) or not (0 < c < 256): continue
        good = True
        for i in range(min(c, 3)):
            e = p + i * 12
            if e + 12 > n or not ok(u32(e)): good = False; break
            s = u32(e); t = b""
            while s < n and d[s] and len(t) < 40: t += bytes([d[s]]); s += 1
            if len(t) < 3 or not all(32 <= ch < 127 for ch in t): good = False; break
        if not good: continue
        for i in range(c):
            e = p + i * 12
            if e + 12 > n: break
            s = u32(e); t = b""
            while s < n and d[s] and len(t) < 40: t += bytes([d[s]]); s += 1
            if t: names.add(t.decode("ascii", "replace"))
    kind = "Ginja" if att else ("strips" if strips else "?")
    print(f"{os.path.basename(path):22s} id={mid:<3d} reloc={applied:<5d} "
          f"attaches={att:<4d} stripMeshes={strips:<4d} texnames={len(names):<3d} -> {kind}")
