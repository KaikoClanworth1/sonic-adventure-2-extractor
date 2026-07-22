"""Prototype parsers for Sonic Adventure 2 PC formats.

Validated empirically against the real Steam game files.
"""
import struct, io, os


# ---------------------------------------------------------------- PRS
def prs_decompress(data: bytes) -> bytearray:
    """SEGA PRS (LZ77-ish) decompressor.

    Control bits are read LSB-first from lazily-refilled control bytes.
      1        -> literal byte
      0 0 b b  -> short copy, size = bb + 2, 8-bit negative offset
      0 1      -> long copy, 16-bit LE word: offset = (w>>3) - 8192,
                  size = w & 7; if size == 0 an extra byte gives size + 1,
                  else size += 2.  w == 0 terminates the stream.
    """
    out = bytearray()
    pos = 0
    ctrl = 0
    ctrl_bits = 0
    n = len(data)

    def flag():
        nonlocal ctrl, ctrl_bits, pos
        if ctrl_bits == 0:
            if pos >= n:
                return -1
            ctrl = data[pos]
            pos += 1
            ctrl_bits = 8
        b = ctrl & 1
        ctrl >>= 1
        ctrl_bits -= 1
        return b

    while True:
        f = flag()
        if f < 0:
            break
        if f == 1:
            if pos >= n:
                break
            out.append(data[pos])
            pos += 1
            continue

        f = flag()
        if f < 0:
            break
        if f == 0:
            # short copy: two more control bits give the size
            b1 = flag()
            b0 = flag()
            if b1 < 0 or b0 < 0:
                break
            size = ((b1 << 1) | b0) + 2
            if pos >= n:
                break
            offset = data[pos] - 256
            pos += 1
        else:
            # long copy: 16-bit little-endian descriptor
            if pos + 1 >= n:
                break
            word = data[pos] | (data[pos + 1] << 8)
            pos += 2
            if word == 0:
                break                      # end-of-stream marker
            size = word & 7
            offset = (word >> 3) - 8192
            if size == 0:
                if pos >= n:
                    break
                size = data[pos] + 1
                pos += 1
            else:
                size += 2

        start = len(out) + offset
        if start < 0:
            raise ValueError(f"PRS back-reference before start of output "
                             f"(start={start}, off={offset})")
        for i in range(size):
            out.append(out[start + i])

    return out


def is_prs(data: bytes) -> bool:
    """Heuristic: try a short decompress and see if it produces sane output."""
    try:
        return len(prs_decompress(data[:4096])) > 0
    except Exception:
        return False


# ---------------------------------------------------------------- PAK
PAK_MAGIC = b"\x01pak"


class PakEntry:
    __slots__ = ("long_path", "short_path", "length", "length2", "offset", "data")

    def __init__(self, long_path, short_path, length, length2):
        self.long_path = long_path
        self.short_path = short_path
        self.length = length
        self.length2 = length2
        self.offset = 0
        self.data = None

    @property
    def name(self):
        return self.short_path.replace("\\", "/")

    def __repr__(self):
        return f"<PakEntry {self.short_path!r} len={self.length}>"


def _u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def pak_parse(data: bytes, load_data=True):
    """Parse an SA2PC '\\x01pak' archive.

    Layout (little-endian, verified against all 230 shipped .pak files):
        0x00  char[4]  magic  = 01 'p' 'a' 'k'
        0x04  byte[33] zero padding
        0x25  u32      entry count (mirror)
        0x29  u32      total size of all file data
        0x2D  u32      total size of all file data (mirror)
        0x31  u32      zero
        0x35  u32      zero
        0x39  u32      entry count
        0x3D  entry[]  metadata table
        ...   raw file data, concatenated in entry order

    Each entry:
        u32 n; char longPath[n]      (original build path, e.g. ..\\..\\sonic2\\...)
        u32 m; char shortPath[m]     (archive-relative path)
        u32 length
        u32 length2                  (mirror of length)
    """
    if data[:4] != PAK_MAGIC:
        raise ValueError("not a PAK archive")

    count = _u32(data, 0x39)
    total_data = _u32(data, 0x29)
    pos = 0x3D
    entries = []
    for _ in range(count):
        n = _u32(data, pos); pos += 4
        long_path = data[pos:pos + n].decode("latin-1"); pos += n
        m = _u32(data, pos); pos += 4
        short_path = data[pos:pos + m].decode("latin-1"); pos += m
        length = _u32(data, pos); pos += 4
        length2 = _u32(data, pos); pos += 4
        entries.append(PakEntry(long_path, short_path, length, length2))

    data_start = pos
    off = data_start
    for e in entries:
        e.offset = off
        if load_data:
            e.data = data[off:off + e.length]
        off += e.length

    return {
        "count": count,
        "total_data": total_data,
        "entries": entries,
        "data_start": data_start,
        "data_end": off,
    }


# ---------------------------------------------------------------- GVM
def gvm_parse(data: bytes):
    """GVM (GameCube texture archive) directory.

        0x00  char[4] 'GVMH'
        0x04  u32     header size (bytes after this field)
        0x08  u16     flags  (bit0 names, bit1 formats, bit2 dimensions, bit3 global index)
        0x0A  u16     entry count
        0x0C  entry[] each: u16 index, char[28] name, [u8 fmt1, u8 fmt2],
                            [u16 dimensions], [u32 global index]
    """
    if data[:4] != b"GVMH":
        raise ValueError("not a GVM archive")
    hdr_size = struct.unpack_from("<I", data, 4)[0]
    flags = struct.unpack_from("<H", data, 8)[0]
    count = struct.unpack_from("<H", data, 10)[0]

    has_names = bool(flags & 0x8)
    has_formats = bool(flags & 0x4)
    has_dims = bool(flags & 0x2)
    has_gbix = bool(flags & 0x1)

    pos = 0x0C
    entries = []
    for _ in range(count):
        idx = struct.unpack_from("<H", data, pos)[0]; pos += 2
        name = ""
        if has_names:
            name = data[pos:pos + 28].split(b"\0")[0].decode("latin-1"); pos += 28
        fmt = None
        if has_formats:
            fmt = (data[pos], data[pos + 1]); pos += 2
        dims = None
        if has_dims:
            dims = struct.unpack_from("<H", data, pos)[0]; pos += 2
        gbix = None
        if has_gbix:
            gbix = struct.unpack_from("<I", data, pos)[0]; pos += 4
        entries.append({"index": idx, "name": name, "format": fmt,
                        "dims": dims, "gbix": gbix})

    # texture blobs follow the header
    tex_start = hdr_size + 8
    return {"count": count, "flags": flags, "entries": entries,
            "tex_start": tex_start, "header_size": hdr_size}
