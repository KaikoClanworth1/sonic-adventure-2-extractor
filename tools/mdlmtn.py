"""SA2 PC character model (*mdl.prs) and motion (*mtn.prs) containers.

Both are big-endian with **direct file offsets** (no rebasing).

MDL:  repeat { u32 index; u32 objectOffset }  until index == 0xFFFFFFFF
MTN:  repeat { i16 index; u16 nodeCount; u32 motionOffset } until index == -1

NJS_MOTION:
    0x00 u32 mdata pointer
    0x04 i32 frameCount
    0x08 u16 type      (AnimFlags: 1=Position 2=Rotation 4=Scale ...)
    0x0A u16 inp_fn    (low nibble = channels per node, bits 6-7 = interpolation)

mdata, per animated node: one u32 pointer per set channel bit (in AnimFlags bit
order) followed by one i32 count per set channel bit.

Keyframes: position/scale = { i32 frame; f32 x,y,z }
           rotation       = { i32 frame; i32 x,y,z }  (BAMS, 0x10000 = 360 deg)
"""
import struct, math
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eventmodel import NinjaFile

BAMS = math.pi * 2.0 / 65536.0

AF_POSITION = 0x0001
AF_ROTATION = 0x0002
AF_SCALE    = 0x0004
AF_VECTOR   = 0x0008
AF_VERTEX   = 0x0010
AF_NORMAL   = 0x0020
AF_TARGET   = 0x0040
AF_ROLL     = 0x0080
AF_ANGLE    = 0x0100
AF_COLOR    = 0x0200
AF_INTENS   = 0x0400
AF_SPOT     = 0x0800
AF_POINT    = 0x1000
AF_QUAT     = 0x2000
AF_SHAPE    = 0x4000
AF_EVENT    = 0x8000

FLAG_ORDER = [AF_POSITION, AF_ROTATION, AF_SCALE, AF_VECTOR, AF_VERTEX, AF_NORMAL,
              AF_TARGET, AF_ROLL, AF_ANGLE, AF_COLOR, AF_INTENS, AF_SPOT,
              AF_POINT, AF_QUAT, AF_SHAPE, AF_EVENT]


def read_mdl_table(data):
    """Return [(index, objectOffset)] from an MDL container."""
    out = []
    o = 0
    n = len(data)
    while o + 8 <= n:
        idx, ptr = struct.unpack_from(">II", data, o)
        if idx == 0xFFFFFFFF:
            break
        if ptr == 0 or ptr >= n:
            o += 8
            continue
        out.append((idx, ptr))
        o += 8
    return out


def read_mtn_table(data):
    """Return [(index, nodeCount, motionOffset)] from an MTN container."""
    out = []
    o = 0
    n = len(data)
    while o + 8 <= n:
        idx, cnt, ptr = struct.unpack_from(">hHI", data, o)
        if idx == -1:
            break
        if ptr == 0 or ptr >= n:
            o += 8
            continue
        out.append((idx, cnt, ptr))
        o += 8
    return out


class Motion:
    __slots__ = ("frame_count", "type", "inp_fn", "node_count",
                 "pos", "rot", "scale", "interpolation")

    def __init__(self):
        self.pos = {}     # node index -> [(frame, (x,y,z))]
        self.rot = {}     # node index -> [(frame, (rx,ry,rz)) in radians]
        self.scale = {}


def read_motion(data, off, node_count):
    n = len(data)
    if off + 16 > n:
        return None
    mdata_ptr, frames, mtype, inp_fn = struct.unpack_from(">IiHH", data, off)
    if mdata_ptr == 0 or mdata_ptr >= n:
        return None
    if frames <= 0 or frames > 100000:
        return None
    m = Motion()
    m.frame_count = frames
    m.type = mtype
    m.inp_fn = inp_fn
    m.node_count = node_count
    m.interpolation = (inp_fn >> 6) & 3

    chans = [f for f in FLAG_ORDER if mtype & f]
    if not chans:
        return None
    stride = len(chans) * 8            # one ptr + one count per channel
    for i in range(node_count):
        base = mdata_ptr + i * stride
        if base + stride > n:
            break
        ptrs = []
        for k in range(len(chans)):
            ptrs.append(struct.unpack_from(">I", data, base + k * 4)[0])
        counts = []
        for k in range(len(chans)):
            counts.append(struct.unpack_from(">i", data, base + len(chans) * 4 + k * 4)[0])
        for ci, ch in enumerate(chans):
            p, c = ptrs[ci], counts[ci]
            if p == 0 or c <= 0 or p >= n or c > 65535:
                continue
            keys = []
            if ch in (AF_POSITION, AF_SCALE):
                if p + c * 16 > n:
                    continue
                for k in range(c):
                    fr, x, y, z = struct.unpack_from(">ifff", data, p + k * 16)
                    keys.append((fr, (x, y, z)))
                (m.pos if ch == AF_POSITION else m.scale)[i] = keys
            elif ch == AF_ROTATION:
                if p + c * 16 > n:
                    continue
                for k in range(c):
                    fr, x, y, z = struct.unpack_from(">iiii", data, p + k * 16)
                    keys.append((fr, (x * BAMS, y * BAMS, z * BAMS)))
                m.rot[i] = keys
    return m


def count_animated(nodes):
    """Ninja counts nodes that are not flagged NoAnimate (bit 6 = 0x40)."""
    return sum(1 for nd in nodes if not (nd.flags & 0x40))
