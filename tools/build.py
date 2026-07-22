"""Turn an NJS_OBJECT tree into renderable meshes.

Ninja chunk models share ONE vertex cache across the whole node tree: each
node's vertex chunk writes into the cache at its `indexOffset`, and a node's
polygon chunks may reference vertices uploaded by an ancestor.  So the tree
must be walked depth-first with a persistent cache, transforming each node's
vertices into model space as they are written.
"""
import math, struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from chunk import parse_vertex_chunks, parse_poly_chunks, VERTEX_TYPES, STRIP_TYPES

BAMS = math.pi * 2.0 / 65536.0
VALID_V = set(VERTEX_TYPES)
VALID_P = (set(range(1, 6)) | {8, 9} | set(range(16, 32))
           | set(range(56, 59)) | set(STRIP_TYPES) | {0, 255})


# ------------------------------------------------------------------ math
def mat_identity():
    return [1.0, 0, 0, 0,  0, 1.0, 0, 0,  0, 0, 1.0, 0,  0, 0, 0, 1.0]


def mat_mul(a, b):
    """Row-vector convention: result = a * b  (apply a, then b)."""
    r = [0.0] * 16
    for i in range(4):
        for j in range(4):
            r[i * 4 + j] = (a[i * 4 + 0] * b[0 * 4 + j] +
                            a[i * 4 + 1] * b[1 * 4 + j] +
                            a[i * 4 + 2] * b[2 * 4 + j] +
                            a[i * 4 + 3] * b[3 * 4 + j])
    return r


def mat_from_srt(pos, rot_bams, scale, zyx=False):
    sx, sy, sz = scale
    rx, ry, rz = (r * BAMS for r in rot_bams)
    cx, sxr = math.cos(rx), math.sin(rx)
    cy, syr = math.cos(ry), math.sin(ry)
    cz, szr = math.cos(rz), math.sin(rz)
    if zyx:
        # SA2 "ZYX" flag actually evaluates Z * X * Y
        m11 = (syr * sxr * szr) + (cy * cz)
        m12 = cx * szr
        m13 = (cy * sxr * szr) - (syr * cz)
        m21 = (syr * sxr * cz) - (cy * szr)
        m22 = cx * cz
        m23 = (cy * sxr * cz) + (syr * szr)
        m31 = syr * cx
        m32 = -sxr
        m33 = cy * cx
    else:
        m11 = cz * cy
        m12 = szr * cy
        m13 = -syr
        m21 = (cz * syr * sxr) - (szr * cx)
        m22 = (szr * syr * sxr) + (cz * cx)
        m23 = cy * sxr
        m31 = (cz * syr * cx) + (szr * sxr)
        m32 = (szr * syr * cx) - (cz * sxr)
        m33 = cy * cx
    return [sx * m11, sx * m12, sx * m13, 0.0,
            sy * m21, sy * m22, sy * m23, 0.0,
            sz * m31, sz * m32, sz * m33, 0.0,
            pos[0],   pos[1],   pos[2],   1.0]


def xform_point(m, p):
    x, y, z = p
    return (x * m[0] + y * m[4] + z * m[8] + m[12],
            x * m[1] + y * m[5] + z * m[9] + m[13],
            x * m[2] + y * m[6] + z * m[10] + m[14])


def xform_dir(m, p):
    x, y, z = p
    return (x * m[0] + y * m[4] + z * m[8],
            x * m[1] + y * m[5] + z * m[9],
            x * m[2] + y * m[6] + z * m[10])


# ------------------------------------------------------------------ validate
def valid_attach(nf, ptr):
    if not nf.ok(ptr):
        return False
    a = nf.off(ptr)
    if a + 0x18 > nf.n:
        return False
    vl = nf.u32(a); pl = nf.u32(a + 4)
    if vl == 0 and pl == 0:
        return False
    if vl:
        if not nf.ok(vl):
            return False
        if (nf.u32(nf.off(vl)) & 0xFF) not in VALID_V | {255}:
            return False
    if pl:
        if not nf.ok(pl):
            return False
        if (struct.unpack_from(">H", nf.d, nf.off(pl))[0] & 0xFF) not in VALID_P:
            return False
    r = nf.f32(a + 20)
    if not (0.0 <= r < 1e7) or r != r:
        return False
    return True


# ------------------------------------------------------------------ build
class MeshPart:
    """One drawable: triangles sharing a texture/material."""
    __slots__ = ("positions", "normals", "uvs", "colors", "indices",
                 "texture_id", "node_index", "node_weights", "double_sided",
                 "blend_src", "blend_dst", "diffuse", "ignore_light", "use_alpha")

    def __init__(self):
        self.positions = []
        self.normals = []
        self.uvs = []
        self.colors = []
        self.indices = []
        self.texture_id = -1
        self.node_index = 0
        self.node_weights = []
        self.double_sided = False
        self.blend_src = 4
        self.blend_dst = 5
        self.diffuse = None
        self.ignore_light = False
        self.use_alpha = False


def build_model(nf, root_ptr, max_nodes=4096):
    """Walk the tree and return (nodes, parts).

    `parts` holds model-space geometry; each vertex records the node that
    supplied it so an FBX skin cluster can bind it.
    """
    nodes = nf.read_tree(root_ptr)
    if not nodes or len(nodes) > max_nodes:
        return nodes, []

    # world matrix per node
    for nd in nodes:
        local = mat_from_srt(nd.pos, nd.rot, nd.scale, bool(nd.flags & 0x20))
        nd_parent = nd.parent
        nd.world = local if nd_parent is None else mat_mul(local, nd_parent.world)

    cache = {}          # cache index -> (pos, normal, node_index, diffuse)
    parts = []
    for nd in nodes:
        if not valid_attach(nf, nd.attach_ptr):
            continue
        try:
            m = nf.read_model(nd.attach_ptr)
        except Exception:
            continue
        if m is None:
            continue
        # upload this node's vertices into the shared cache, in model space
        for ci, v in m.vertices.items():
            wp = xform_point(nd.world, v.pos)
            wn = xform_dir(nd.world, v.normal) if v.normal else None
            cache[ci] = (wp, wn, nd.index, v.diffuse)
        # emit this node's strips
        for pc in m.polys:
            part = MeshPart()
            part.texture_id = pc.texture_id
            part.node_index = nd.index
            part.double_sided = bool(pc.flags & 0x80)
            part.blend_src, part.blend_dst = pc.blend_src, pc.blend_dst
            part.diffuse = pc.diffuse
            remap = {}
            for st in pc.strips:
                idx = st.indices
                uvs = st.uvs
                for k in range(len(idx) - 2):
                    tri = (idx[k], idx[k + 1], idx[k + 2])
                    tuv = (k, k + 1, k + 2)
                    # alternate winding along the strip
                    flip = (k % 2 == 1)
                    if st.reversed:
                        flip = not flip
                    if flip:
                        tri = (tri[1], tri[0], tri[2])
                        tuv = (tuv[1], tuv[0], tuv[2])
                    if tri[0] == tri[1] or tri[1] == tri[2] or tri[0] == tri[2]:
                        continue
                    ok = True
                    out = []
                    for c, uvi in zip(tri, tuv):
                        ent = cache.get(c)
                        if ent is None:
                            ok = False
                            break
                        uv = uvs[uvi] if uvi < len(uvs) else (0.0, 0.0)
                        key = (c, uv)
                        vi = remap.get(key)
                        if vi is None:
                            vi = len(part.positions)
                            remap[key] = vi
                            part.positions.append(ent[0])
                            part.normals.append(ent[1] or (0.0, 1.0, 0.0))
                            part.uvs.append(uv)
                            part.colors.append(ent[3])
                            part.node_weights.append(ent[2])
                        out.append(vi)
                    if ok:
                        part.indices.extend(out)
            if part.indices:
                parts.append(part)
    return nodes, parts
