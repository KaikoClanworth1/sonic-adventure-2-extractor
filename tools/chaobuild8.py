"""Full Chao stage — robust. Scan every group-count candidate, parse its strips,
map it to the vertex block that follows (after alignment padding), then keep only
the FULLEST mesh per block. A false/partial parse references a sub-range of a
real block, so per-block "most vertices used" discards it automatically."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress

d = bytes(prs_decompress(open(sys.argv[1], "rb").read()))
n = len(d)
f32 = lambda o: struct.unpack_from(">f", d, o)[0]
be16 = lambda o: struct.unpack_from(">H", d, o)[0]
be16s = lambda o: struct.unpack_from(">h", d, o)[0]


def isvert(o):
    if o + 24 > n: return False
    nx, ny, nz = f32(o + 12), f32(o + 16), f32(o + 20)
    px, py, pz = f32(o), f32(o + 4), f32(o + 8)
    return 0.9 < nx*nx + ny*ny + nz*nz < 1.1 and all(abs(v) < 5000 for v in (px, py, pz))


# maximal vertex blocks (start -> size)
blocks = []
o = 0
while o < n - 24:
    if isvert(o):
        s = o
        while s - 24 >= 0 and isvert(s - 24): s -= 24
        e = s
        while isvert(e): e += 24
        blocks.append((s, (e - s) // 24))
        o = max(e, o + 4)
    else:
        o += 4
blocks = sorted(set(blocks))
starts = [b[0] for b in blocks]
size_of = dict(blocks)
import bisect


def block_for(p, need):
    """nearest block start V in [p, p+4096] big enough for `need` verts. isvert
    undercounts blocks (a few interior verts have slightly off-unit normals), so
    allow a small shortfall and trust the index's vertex count."""
    i = bisect.bisect_left(starts, p)
    while i < len(starts) and starts[i] <= p + 4096:
        V = starts[i]
        if size_of[V] >= need * 0.8 and V + need * 24 <= n:
            return V
        i += 1
    return None


def strip_tris(groups):
    tris = []
    for rev, cs in groups:
        for k in range(len(cs) - 2):
            x, y, z = cs[k], cs[k+1], cs[k+2]
            if (k % 2 == 1) ^ rev: x, y = y, x
            if len({x, y, z}) == 3: tris.append((x, y, z))
    return tris


def parse_groups(G):
    gc = be16(G)
    if gc < 1 or gc > 6000: return None
    p = G + 2
    groups, maxpos = [], -1
    for _ in range(gc):
        if p + 2 > n: return None
        sc = be16s(p); p += 2
        cnt = abs(sc)
        if cnt < 1 or cnt > 6000 or p + cnt * 6 > n: return None
        cs = []
        for _ in range(cnt):
            pi = be16(p); p += 6
            cs.append(pi)
            if pi > maxpos: maxpos = pi
        groups.append((sc < 0, cs))
    return groups, maxpos, p


# scan every candidate, map to a block, keep the one with the MOST triangles per
# block (the real full index data; partial/garbage parses make fewer tris)
best = {}                              # V -> (ntris, need, tris)
G = 4
while G < n - 8:
    if 1 <= be16(G) <= 6000 and 1 <= abs(be16s(G + 2)) <= 6000:   # first strip count may be negative
        r = parse_groups(G)
        if r:
            groups, maxpos, p = r
            if maxpos >= 3 and len(groups) >= 1:
                need = maxpos + 1
                V = block_for((p + 3) & ~3, need)
                if V is not None:
                    tris = strip_tris(groups)
                    if V not in best or len(tris) > best[V][0]:
                        best[V] = (len(tris), need, tris)
    G += 2

import statistics


def edge(v, a, b):
    return sum((v[a][k] - v[b][k]) ** 2 for k in range(3)) ** 0.5


allv, allf = [], []
for V in sorted(best):
    ntris, need, tris = best[V]
    verts = [(f32(V + i*24), f32(V + i*24 + 4), f32(V + i*24 + 8)) for i in range(need)]
    # coherence filter: a real strip has short, uniform edges; a garbage parse
    # links distant verts. Drop triangles whose longest edge >> the median.
    if len(tris) >= 8:
        elen = [max(edge(verts, a, b), edge(verts, b, c), edge(verts, a, c))
                for a, b, c in tris]
        med = statistics.median(elen)
        thresh = max(med * 8.0, 1e-3)
        keep = [t for t, e in zip(tris, elen) if e <= thresh]
        # if most triangles are long-edge, the whole parse is garbage -> drop it
        if len(keep) < len(tris) * 0.5:
            print(f"  block@0x{V:x}: {need}v REJECTED (incoherent)")
            continue
        tris = keep
    base = len(allv)
    allv += verts
    allf += [(x + base, y + base, z + base) for x, y, z in tris]
    print(f"  block@0x{V:x}: {need}v {len(tris)}t")

print(f"meshes: {len(best)}, verts: {len(allv)}, tris: {len(allf)}")
out = sys.argv[2] if len(sys.argv) > 2 else r"F:\ClaudeProjects\Sonic Adventure 2\build\chao_full.obj"
with open(out, "w") as fh:
    for x, y, z in allv: fh.write(f"v {x:.3f} {y:.3f} {z:.3f}\n")
    for a, b, c in allf: fh.write(f"f {a+1} {b+1} {c+1}\n")
print("wrote", out)
