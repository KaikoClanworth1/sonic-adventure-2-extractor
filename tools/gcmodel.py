"""GC "Ginja" model parser for SA2 stage geometry.

Layout (in a relocated REL image everything is big-endian; the GX display list
is big-endian everywhere):

GCAttach (0x24):
    0x00 u32 vertexSetArray   0x04 u32 skin(=0)   0x08 u32 opaqueMeshes
    0x0C u32 translucentMeshes 0x10 u16 opaqueCount 0x12 u16 transCount
    0x14 f32[3] centre         0x20 f32 radius

GCVertexSet (16B, array ends at attribute 0xFF or 0):
    0x00 u8 attribute (1=Pos, 2=Normal, 3=Color0, 5=Tex0)
    0x01 u8 stride    0x02 u16 count
    0x04 u32 structure (structType = &0xF, dataType = (>>4)&0xF)
    0x08 u32 dataPtr  0x0C u32 dataLen

GCMesh (16B):
    0x00 u32 paramPtr 0x04 i32 paramCount 0x08 u32 primPtr 0x0C u32 primSize

GCParameter (8B): u8 type (+3 pad), u32 data
    0=VtxAttrFmt 1=IndexFlags 2=Lighting 4=BlendAlpha 5=Colour 8=Texture
    9=Unknown 10=TexCoordGen

Display list (BIG-ENDIAN): u8 opcode, u16 count, then per-vertex indices whose
presence/width come from the *sticky* IndexAttributeFlags (2 bits per attribute:
bit 2k = 16-bit index, bit 2k+1 = present; k over GX slots
0 PosMtxIdx,1 Pos,2 Normal,3 Color0,4 Color1,5..12 Tex0..7).
"""
import struct

# GX attribute slot order for the index descriptor
SLOTS = ["PosMtxIdx", "Position", "Normal", "Color0", "Color1",
         "Tex0", "Tex1", "Tex2", "Tex3", "Tex4", "Tex5", "Tex6", "Tex7"]

PRIM_TRIANGLES = 0x90
PRIM_TRISTRIP = 0x98
PRIM_TRIFAN = 0xA0


class GCReader:
    """Reader over a relocated image; pointers are file offsets when base=0."""

    def __init__(self, data, base=0):
        self.d = data
        self.base = base
        self.n = len(data)

    def ok(self, p):
        return p != 0 and self.base <= p < self.base + self.n

    def off(self, p):
        return p - self.base

    def u8(self, o): return self.d[o]
    def u16(self, o): return struct.unpack_from(">H", self.d, o)[0]
    def s16(self, o): return struct.unpack_from(">h", self.d, o)[0]
    def u32(self, o): return struct.unpack_from(">I", self.d, o)[0]
    def i32(self, o): return struct.unpack_from(">i", self.d, o)[0]
    def f32(self, o): return struct.unpack_from(">f", self.d, o)[0]


class GCVertexSet:
    __slots__ = ("attribute", "stride", "count", "struct_type", "data_type",
                 "data_ptr", "values")


def parse_vertex_sets(r, ptr):
    """Return {attribute: GCVertexSet}. attribute 1=pos, 2=normal, 3=col, 5=uv."""
    sets = {}
    o = r.off(ptr)
    for _ in range(32):
        if o + 16 > r.n:
            break
        attr = r.u8(o)
        if attr == 0xFF:
            break
        if attr == 0:
            break
        stride = r.u8(o + 1)
        count = r.u16(o + 2)
        structure = r.u32(o + 4)
        data_ptr = r.u32(o + 8)
        data_len = r.u32(o + 0x0C)
        vs = GCVertexSet()
        vs.attribute = attr
        vs.stride = stride
        vs.count = count
        vs.struct_type = structure & 0x0F
        vs.data_type = (structure >> 4) & 0x0F
        vs.data_ptr = data_ptr
        vs.values = []
        if r.ok(data_ptr) and 0 < count <= 65535 and stride > 0:
            base = r.off(data_ptr)
            for i in range(count):
                e = base + i * stride
                if e + stride > r.n:
                    break
                if attr == 1:      # Position, XYZ float
                    vs.values.append((r.f32(e), r.f32(e + 4), r.f32(e + 8)))
                elif attr == 2:    # Normal, XYZ float
                    vs.values.append((r.f32(e), r.f32(e + 4), r.f32(e + 8)))
                elif attr == 3:    # Color0, RGBA8 (stored R,G,B,A)
                    vs.values.append((r.d[e], r.d[e + 1], r.d[e + 2], r.d[e + 3]))
                elif attr == 5:    # Tex0, ST int16 / 256
                    vs.values.append((r.s16(e) / 256.0, r.s16(e + 4) / 256.0)
                                     if stride >= 4 else
                                     (r.s16(e) / 256.0, 0.0))
        sets[attr] = vs
        o += 16
    return sets


class GCMeshDraw:
    __slots__ = ("texture_id", "tris", "index_flags", "double_sided",
                 "use_alpha", "ambient", "ignore_light", "env_map",
                 "blend_src", "blend_dst")

    def __init__(self):
        self.tris = []          # list of (posIdx, normIdx, colIdx, uvIdx) triples
        self.texture_id = -1
        self.index_flags = 0
        self.double_sided = False
        self.use_alpha = False
        self.ambient = None
        self.ignore_light = False
        self.env_map = False
        self.blend_src = 4
        self.blend_dst = 5


def _corner_readers(index_flags):
    """Return (list of (slot, is16) for present attributes, total corner bytes)."""
    readers = []
    size = 0
    for k, name in enumerate(SLOTS):
        present = (index_flags >> (2 * k + 1)) & 1
        if not present:
            continue
        is16 = (index_flags >> (2 * k)) & 1
        readers.append((name, bool(is16)))
        size += 2 if is16 else 1
    return readers, size


def parse_mesh_array(r, ptr, count, index_flags_start):
    """Parse `count` GCMesh records; IndexAttributeFlags persists across them."""
    draws = []
    index_flags = index_flags_start
    cur_tex = -1
    ambient = None
    double_sided = False
    use_alpha = False
    ignore_light = False
    env_map = False
    blend_src, blend_dst = 4, 5
    mo = r.off(ptr)
    for m in range(count):
        base = mo + m * 16
        if base + 16 > r.n:
            break
        param_ptr = r.u32(base)
        param_count = r.i32(base + 4)
        prim_ptr = r.u32(base + 8)
        prim_size = r.u32(base + 0x0C)

        # ---- parameters (sticky) ----
        if r.ok(param_ptr) and 0 <= param_count < 100000:
            po = r.off(param_ptr)
            for p in range(param_count):
                pe = po + p * 8
                if pe + 8 > r.n:
                    break
                ptype = r.u8(pe)
                data = r.u32(pe + 4)
                if ptype == 1:            # IndexAttributeFlags
                    index_flags = data
                elif ptype == 8:          # Texture
                    cur_tex = data & 0xFFFF
                elif ptype == 5:          # Ambient/diffuse colour (ARGB packed)
                    ambient = data
                elif ptype == 2:          # Lighting / strip flags
                    strip = (data >> 8) & 0xFF
                    ignore_light = bool(strip & 0x01)
                elif ptype == 4:          # BlendAlpha
                    blend_dst = (data >> 8) & 7
                    blend_src = (data >> 11) & 7
                    use_alpha = bool(data & 0x4000)
                elif ptype == 10:         # TexCoordGen: src==Normal => env map
                    src = (data >> 4) & 0xFF
                    env_map = (src == 1)

        # ---- display list ----
        readers, corner_size = _corner_readers(index_flags)
        # find which output slot each reader is (we only keep pos/nrm/col/uv)
        want = {"Position": 0, "Normal": 1, "Color0": 2, "Tex0": 3}
        draw = GCMeshDraw()
        draw.texture_id = cur_tex
        draw.index_flags = index_flags
        draw.ambient = ambient
        draw.double_sided = double_sided
        draw.use_alpha = use_alpha
        draw.ignore_light = ignore_light
        draw.env_map = env_map
        draw.blend_src, draw.blend_dst = blend_src, blend_dst

        if r.ok(prim_ptr) and corner_size > 0:
            p = r.off(prim_ptr)
            end = min(p + prim_size, r.n)
            while p + 3 <= end:
                op = r.u8(p)
                if op == 0:
                    break                 # padding
                vtx_count = r.u16(p + 1)
                p += 3
                corners = []
                bad = False
                for _v in range(vtx_count):
                    if p + corner_size > end:
                        bad = True
                        break
                    slot_idx = {"Position": -1, "Normal": -1,
                                "Color0": -1, "Tex0": -1}
                    for name, is16 in readers:
                        if is16:
                            val = r.u16(p); p += 2
                        else:
                            val = r.u8(p); p += 1
                        if name in slot_idx:
                            slot_idx[name] = val
                    corners.append((slot_idx["Position"], slot_idx["Normal"],
                                    slot_idx["Color0"], slot_idx["Tex0"]))
                if bad:
                    break
                _emit_triangles(op, corners, draw.tris)
        if draw.tris:
            draws.append(draw)
    return draws, index_flags


def _emit_triangles(op, corners, out):
    if op == PRIM_TRIANGLES:
        for i in range(0, len(corners) - 2, 3):
            out.append((corners[i], corners[i + 1], corners[i + 2]))
    elif op == PRIM_TRISTRIP:
        for i in range(2, len(corners)):
            if i % 2 == 0:
                tri = (corners[i - 2], corners[i - 1], corners[i])
            else:
                tri = (corners[i - 1], corners[i - 2], corners[i])
            if tri[0] != tri[1] and tri[1] != tri[2] and tri[0] != tri[2]:
                out.append(tri)
    elif op == PRIM_TRIFAN:
        for i in range(1, len(corners) - 1):
            out.append((corners[0], corners[i], corners[i + 1]))


def parse_gc_attach(r, ptr):
    """Return dict with vertex sets + list of GCMeshDraw, or None."""
    a = r.off(ptr)
    if a + 0x24 > r.n:
        return None
    vtx_ptr = r.u32(a)
    skin = r.u32(a + 4)
    opaque_ptr = r.u32(a + 8)
    trans_ptr = r.u32(a + 0x0C)
    opaque_cnt = r.u16(a + 0x10)
    trans_cnt = r.u16(a + 0x12)
    radius = r.f32(a + 0x20)
    if skin != 0 or not r.ok(vtx_ptr):
        return None
    if opaque_cnt > 4096 or trans_cnt > 4096:
        return None
    sets = parse_vertex_sets(r, vtx_ptr)
    if 1 not in sets or not sets[1].values:
        return None
    draws = []
    flags = 0
    if opaque_cnt and r.ok(opaque_ptr):
        d, flags = parse_mesh_array(r, opaque_ptr, opaque_cnt, flags)
        draws.extend(d)
    if trans_cnt and r.ok(trans_ptr):
        d, flags = parse_mesh_array(r, trans_ptr, trans_cnt, flags)
        for x in d:
            x.use_alpha = True
        draws.extend(d)
    return {"sets": sets, "draws": draws, "radius": radius,
            "opaque": opaque_cnt, "trans": trans_cnt}
