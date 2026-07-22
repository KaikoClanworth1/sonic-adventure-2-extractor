"""Flatten a GameCube REL into a relocation-fixed virtual image.

Sections are laid out consecutively at a virtual base we choose, then every
self-module ADDR32 relocation is applied so intra-module pointers become
valid addresses in that image. A non-zero base keeps NULL distinguishable.

Relocation stream entries are 8 bytes:
    u16 offset   bytes to advance the write cursor before applying
    u8  type     R_PPC_* / R_DOLPHIN_*
    u8  section  target section index (the addend is relative to it)
    u32 addend
R_DOLPHIN_SECTION(202) sets the cursor to the start of `section`.
R_DOLPHIN_NOP(201) just advances. R_DOLPHIN_END(203) ends the stream.
"""
import struct, collections

VBASE = 0x80000000

R_PPC_NONE = 0
R_PPC_ADDR32 = 1
R_PPC_ADDR24 = 2
R_PPC_ADDR16 = 3
R_PPC_ADDR16_LO = 4
R_PPC_ADDR16_HI = 5
R_PPC_ADDR16_HA = 6
R_PPC_REL24 = 10
R_DOLPHIN_NOP = 201
R_DOLPHIN_SECTION = 202
R_DOLPHIN_END = 203


def _u32(d, o): return struct.unpack_from(">I", d, o)[0]
def _u16(d, o): return struct.unpack_from(">H", d, o)[0]


class RelImage:
    """A flattened, pointer-fixed REL."""

    def __init__(self, data, vbase=VBASE):
        self.vbase = vbase
        self.src = data
        self.sections = []       # (index, file_off, size, exec, img_off)
        self.image = bytearray()
        self.sec_base = {}       # section index -> offset within image
        self.stats = collections.Counter()
        self._build()

    def _build(self):
        d = self.src
        num = _u32(d, 0x0C)
        sinfo = _u32(d, 0x10)
        self.module_id = _u32(d, 0x00)
        self.rel_off = _u32(d, 0x24)
        self.imp_off = _u32(d, 0x28)
        self.imp_size = _u32(d, 0x2C)
        self.bss_size = _u32(d, 0x20)
        align = _u32(d, 0x40) if _u32(d, 0x1C) >= 2 else 32
        if align < 4:
            align = 32

        # lay sections out consecutively, honouring the module alignment
        cur = 0
        for i in range(num):
            raw = _u32(d, sinfo + i * 8)
            size = _u32(d, sinfo + i * 8 + 4)
            off = raw & ~3
            is_exec = bool(raw & 1)
            if size == 0:
                self.sec_base[i] = cur
                self.sections.append((i, off, 0, is_exec, cur))
                continue
            cur = (cur + align - 1) // align * align
            self.sec_base[i] = cur
            self.sections.append((i, off, size, is_exec, cur))
            cur += size
        # bss lives at the end
        self.image = bytearray(cur + self.bss_size + 64)
        for i, off, size, is_exec, img_off in self.sections:
            if size and off:
                self.image[img_off:img_off + size] = d[off:off + size]

    def apply_relocations(self):
        """Apply self-module ADDR32 fixups. Returns the number applied."""
        d = self.src
        n = len(d)
        applied = 0
        for k in range(self.imp_size // 8):
            mod = _u32(d, self.imp_off + k * 8)
            off = _u32(d, self.imp_off + k * 8 + 4)
            # only self-references produce valid intra-image pointers
            self_mod = (mod == self.module_id)
            p = off
            cursor = 0          # offset within the image being written
            guard = 0
            while p + 8 <= n:
                guard += 1
                if guard > 4_000_000:
                    break
                skip = _u16(d, p)
                rtype = d[p + 2]
                sect = d[p + 3]
                addend = _u32(d, p + 4)
                p += 8
                if rtype == R_DOLPHIN_END:
                    break
                if rtype == R_DOLPHIN_SECTION:
                    cursor = self.sec_base.get(sect, 0)
                    continue
                cursor += skip
                if rtype == R_DOLPHIN_NOP:
                    continue
                self.stats[rtype] += 1
                if not self_mod:
                    continue
                target = self.vbase + self.sec_base.get(sect, 0) + addend
                if rtype == R_PPC_ADDR32:
                    if cursor + 4 <= len(self.image):
                        struct.pack_into(">I", self.image, cursor, target & 0xFFFFFFFF)
                        applied += 1
                # 16-bit / branch relocations only matter for code, which we
                # never execute, so they are counted but not applied.
        return applied

    def data(self):
        return bytes(self.image)
