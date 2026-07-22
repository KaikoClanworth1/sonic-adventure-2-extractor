"""Ninja Chunk model parser for Sonic Adventure 2 (GameCube/PC big-endian data).

Endianness note
---------------
SA2B/SA2PC data is the original little-endian Dreamcast data converted to
big-endian *per declared field size*, matching NJS_CNK_MODEL:

    typedef struct { Sint32 *vlist; Sint16 *plist; NJS_POINT3 center; Float r; }

so the **vertex list is byte-swapped in 4-byte units** and the **polygon list
in 2-byte units**.  Concretely:

  vertex chunk header  : BE u32 W -> head = W & 0xFF, flag = (W>>8)&0xFF,
                                     size = W >> 16   (size counted in u32s)
  vertex chunk index hd: BE u32 W -> indexOffset = W & 0xFFFF, nbVertex = W>>16
  poly chunk header    : BE u16 w -> head = w & 0xFF, flag = w >> 8
                         BE u16   -> size (counted in u16s)

Verified against the shipped event files.
"""
import struct

# ----------------------------------------------------------------- enums
VERTEX_TYPES = {
    32: "VertexSH", 33: "VertexNormalSH", 34: "Vertex", 35: "VertexDiffuse8",
    36: "VertexUserFlags", 37: "VertexNinjaFlags", 38: "VertexDiffuseSpecular5",
    39: "VertexDiffuseSpecular4", 40: "VertexDiffuseSpecular16",
    41: "VertexNormal", 42: "VertexNormalDiffuse8", 43: "VertexNormalUserFlags",
    44: "VertexNormalNinjaFlags", 45: "VertexNormalDiffuseSpecular5",
    46: "VertexNormalDiffuseSpecular4", 47: "VertexNormalDiffuseSpecular16",
    48: "VertexNormalX", 49: "VertexNormalXDiffuse8", 50: "VertexNormalXUserFlags",
}

STRIP_TYPES = {
    64: "Strip", 65: "StripUVN", 66: "StripUVH", 67: "StripNormal",
    68: "StripUVNNormal", 69: "StripUVHNormal", 70: "StripColor",
    71: "StripUVNColor", 72: "StripUVHColor", 73: "Strip2",
    74: "StripUVN2", 75: "StripUVH2",
}

# Fallback words-per-vertex, used only when a chunk declares nbVertex == 0.
# The real stride is derived from the chunk itself: (size - 1) / nbVertex,
# which is self-validating and immune to table mistakes.
VERTEX_SIZE_U32 = {
    32: 4, 33: 8,
    34: 4,   # pos(3) + pad(1)
    35: 4, 36: 4, 37: 4, 38: 4, 39: 4, 40: 4,
    41: 6,   # pos(3) + normal(3)
    42: 7, 43: 7, 44: 7, 45: 7, 46: 7, 47: 7,
    48: 4, 49: 5, 50: 5,
}

# which chunk types actually carry a per-vertex normal
HAS_NORMAL = {33, 41, 42, 43, 44, 45, 46, 47}


class Vertex:
    __slots__ = ("pos", "normal", "diffuse", "weight", "node_index", "cache_index")

    def __init__(self, pos, normal=None, diffuse=None, weight=1.0,
                 node_index=0, cache_index=0):
        self.pos = pos
        self.normal = normal
        self.diffuse = diffuse
        self.weight = weight
        self.node_index = node_index
        self.cache_index = cache_index


class Strip:
    __slots__ = ("indices", "uvs", "normals", "colors", "reversed")

    def __init__(self):
        self.indices = []
        self.uvs = []
        self.normals = []
        self.colors = []
        self.reversed = False


class PolyChunk:
    """A strip chunk together with the material state in force when it appears."""
    __slots__ = ("strips", "texture_id", "flags", "diffuse", "ambient",
                 "specular", "chunk_type", "uv_scale", "blend_src", "blend_dst",
                 "tex_flags")

    def __init__(self):
        self.strips = []
        self.texture_id = -1
        self.flags = 0
        self.diffuse = None
        self.ambient = None
        self.specular = None
        self.chunk_type = 0
        self.uv_scale = 256.0
        self.blend_src = 0
        self.blend_dst = 0
        self.tex_flags = 0


# ----------------------------------------------------------------- readers
def _u32be(d, o):
    return struct.unpack_from(">I", d, o)[0]


def _u16be(d, o):
    return struct.unpack_from(">H", d, o)[0]


def _s16be(d, o):
    return struct.unpack_from(">h", d, o)[0]


def _f32_from_u32(w):
    return struct.unpack("<f", struct.pack("<I", w))[0]


def parse_vertex_chunks(data, off, limit=None):
    """Parse a vertex chunk stream. Returns {cache_index: Vertex}."""
    verts = {}
    n = len(data)
    guard = 0
    while off + 4 <= n:
        guard += 1
        if guard > 4096:
            raise ValueError("vertex chunk stream runaway")
        w = _u32be(data, off)
        head = w & 0xFF
        flag = (w >> 8) & 0xFF
        size = w >> 16
        if head == 0xFF:                      # End
            break
        if head == 0:                         # Null
            off += 4
            continue
        if head not in VERTEX_TYPES:
            raise ValueError(f"bad vertex chunk type {head} at 0x{off:x}")
        if off + 8 > n:
            break
        w2 = _u32be(data, off + 4)
        index_offset = w2 & 0xFFFF
        nb_vertex = w2 >> 16
        body = off + 8
        # `size` counts u32 words after the 4-byte header; the first of those is
        # the index header, so the vertex payload is (size - 1) words. Derive the
        # stride from the chunk rather than trusting a hardcoded table.
        if nb_vertex > 0 and size >= 1:
            stride = (size - 1) // nb_vertex
        else:
            stride = VERTEX_SIZE_U32.get(head, 4)
        if stride < 3:
            off += 4 + size * 4
            continue
        weight_status = flag & 3
        for i in range(nb_vertex):
            vo = body + i * stride * 4
            if vo + stride * 4 > n or vo + 12 > body + (size - 1) * 4:
                break
            px = _f32_from_u32(_u32be(data, vo))
            py = _f32_from_u32(_u32be(data, vo + 4))
            pz = _f32_from_u32(_u32be(data, vo + 8))
            v = Vertex((px, py, pz))
            v.cache_index = index_offset + i
            if head in (32, 33) and stride >= 4:
                # weighted (skin) vertex: word 3 packs node index + weight
                w4 = _u32be(data, vo + 12)
                v.node_index = (w4 >> 16) & 0xFFFF
                v.weight = (w4 & 0xFFFF) / 255.0
            if head in HAS_NORMAL and stride >= 6:
                noff = vo + (16 if head == 33 else 12)
                if noff + 12 <= n:
                    v.normal = (_f32_from_u32(_u32be(data, noff)),
                                _f32_from_u32(_u32be(data, noff + 4)),
                                _f32_from_u32(_u32be(data, noff + 8)))
                if head == 42 and stride >= 7:
                    v.diffuse = _u32be(data, vo + 24)
            elif head == 35 and stride >= 4:
                v.diffuse = _u32be(data, vo + 12)
            verts[v.cache_index] = v
        off += 4 + size * 4
    return verts


def parse_poly_chunks(data, off):
    """Parse a polygon chunk stream. Returns a list of PolyChunk."""
    out = []
    n = len(data)
    cur_tex = -1
    cur_flags = 0
    diffuse = ambient = specular = None
    guard = 0
    uv_scale = 256.0
    tex_flags = 0
    blend_src = blend_dst = 0
    while off + 2 <= n:
        guard += 1
        if guard > 8192:
            raise ValueError("poly chunk stream runaway")
        w = _u16be(data, off)
        head = w & 0xFF
        flag = w >> 8
        if head == 0xFF:
            break
        if head == 0:
            off += 2
            continue
        if 1 <= head <= 5:                     # Bits
            if head == 1:
                blend_src = (flag >> 3) & 7
                blend_dst = flag & 7
            off += 2
            continue
        if head in (8, 9):                     # Tiny / texture id
            tid = _u16be(data, off + 2)
            cur_tex = tid & 0x1FFF
            tex_flags = flag
            off += 4
            continue
        if 16 <= head <= 31:                   # Material
            size = _u16be(data, off + 2)
            p = off + 4
            # colours are 32-bit ARGB, swapped as u16 pairs
            def argb(q):
                hi = _u16be(data, q)
                lo = _u16be(data, q + 2)
                v = (lo << 16) | hi
                return ((v >> 24) & 0xFF, (v >> 16) & 0xFF,
                        (v >> 8) & 0xFF, v & 0xFF)
            t = head - 16
            if t & 1:
                diffuse = argb(p); p += 4
            if t & 2:
                ambient = argb(p); p += 4
            if t & 4:
                specular = argb(p); p += 4
            off += 4 + size * 2
            continue
        if 56 <= head <= 58:                   # Volume - skip
            size = _u16be(data, off + 2)
            off += 4 + size * 2
            continue
        if 64 <= head <= 75:                   # Strip
            size = _u16be(data, off + 2)
            end = off + 4 + size * 2
            pc = PolyChunk()
            pc.chunk_type = head
            pc.texture_id = cur_tex
            pc.flags = flag
            pc.diffuse = diffuse
            pc.ambient = ambient
            pc.specular = specular
            pc.tex_flags = tex_flags
            pc.blend_src, pc.blend_dst = blend_src, blend_dst
            hdr = _u16be(data, off + 4)
            nb_strip = hdr & 0x3FFF
            user_flags = (hdr >> 14) & 3
            p = off + 6
            has_uv = head in (65, 66, 68, 69, 71, 72, 74, 75)
            uv_hi = head in (66, 69, 72, 75)
            has_normal = head in (67, 68, 69)
            has_color = head in (70, 71, 72)
            limit = min(end, n)
            for _ in range(nb_strip):
                if p + 2 > limit:
                    break
                ln = _s16be(data, p); p += 2
                st = Strip()
                st.reversed = ln < 0
                cnt = abs(ln)
                for k in range(cnt):
                    if p + 2 > limit:
                        break
                    st.indices.append(_u16be(data, p)); p += 2
                    if has_uv:
                        if p + 4 > limit:
                            break
                        u = _s16be(data, p); v = _s16be(data, p + 2); p += 4
                        d = 1024.0 if uv_hi else 256.0
                        st.uvs.append((u / d, v / d))
                    if has_normal:
                        p += 12
                    if has_color:
                        p += 4
                if st.indices:
                    pc.strips.append(st)
            out.append(pc)
            off = end
            continue
        # unknown chunk: use size field to skip
        size = _u16be(data, off + 2)
        off += 4 + size * 2
    return out
