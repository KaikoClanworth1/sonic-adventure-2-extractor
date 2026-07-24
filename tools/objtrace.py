"""Trace SA2 object load functions to the models they draw (DRM-free exe only).

Each object-list entry is {u32 flags, u32 loadDistance, void(*load)(task*),
char* name}. The load function installs sub-functions on the task (Main/Display)
and sets up the model; the model pointer -- an NJS_OBJECT root -- appears as an
imm32 somewhere in that call graph. We disassemble the load function with
capstone, follow call targets and any code pointers it stores (the sub-functions),
and collect every imm32 that resolves to a valid model root.

NOTE: this only works on an unprotected executable. The retail Steam build is
SteamStub-encrypted (.text entropy 8.0, no prologues) and cannot be read.
"""
import sys, os, struct
import capstone
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pe as pemod


class Tracer:
    def __init__(self, exe_path):
        self.pe = pemod.PE(open(exe_path, "rb").read())
        self.flat = self.pe.flat_image()
        self.base = self.pe.image_base
        self.N = len(self.flat)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        t = [s for s in self.pe.sections if s["name"] == ".text"][0]
        self.TLO = self.base + t["vaddr"]
        self.THI = self.base + t["vaddr"] + t["vsize"]

    def u32(self, o):
        return struct.unpack_from("<I", self.flat, o)[0] if 0 <= o + 4 <= self.N else 0

    def f32(self, o):
        return struct.unpack_from("<f", self.flat, o)[0] if 0 <= o + 4 <= self.N else 0

    def is_code(self, va):
        return self.TLO <= va < self.THI

    def is_data(self, va):
        return self.base + 0x1000 <= va < self.base + self.N and not self.is_code(va)

    def cstr(self, va):
        o = va - self.base
        if not (0 <= o < self.N):
            return None
        s = b""
        while o < self.N and self.flat[o] and len(s) < 32:
            c = self.flat[o]
            if not (0x20 <= c < 0x7F):
                return None
            s += bytes([c]); o += 1
        return s.decode().strip() if 2 <= len(s) <= 32 else None

    def is_model_root(self, va):
        """NJS_OBJECT: small flags, valid attach, ~unit scale, sane child/sibling."""
        o = va - self.base
        if not (0 <= o and o + 0x34 <= self.N):
            return False
        fl, att = self.u32(o), self.u32(o + 4)
        sx, sy, sz = self.f32(o + 0x20), self.f32(o + 0x24), self.f32(o + 0x28)
        child, sib = self.u32(o + 0x2C), self.u32(o + 0x30)
        if fl >= 0x8000 or not self.is_data(att):
            return False
        if not (0.01 < abs(sx) < 100 and 0.01 < abs(sy) < 100 and 0.01 < abs(sz) < 100):
            return False
        if (child and not self.is_data(child)) or (sib and not self.is_data(sib)):
            return False
        return True

    def scan(self, fva, limit=0x800):
        """Return (data imm32s, code imm32s) referenced by the function at fva."""
        o = fva - self.base                     # flat is RVA-indexed
        if not (0 <= o < self.N):
            return set(), set()
        data, code = set(), set()
        for insn in self.md.disasm(self.flat[o:min(o + limit, self.N)], fva):
            if insn.mnemonic in ("ret", "retf"):
                break
            for op in insn.operands:
                if op.type == capstone.x86.X86_OP_IMM:
                    v = op.imm & 0xFFFFFFFF
                    if self.is_code(v):
                        code.add(v)
                    elif self.is_data(v):
                        data.add(v)
        return data, code

    def models_for(self, load_va, max_depth=3, max_funcs=60):
        seen, frontier, models = set(), [load_va], []
        depth = 0
        while frontier and depth < max_depth and len(seen) < max_funcs:
            nxt = []
            for f in frontier:
                if f in seen or len(seen) >= max_funcs:
                    continue
                seen.add(f)
                data, code = self.scan(f)
                for v in data:
                    if self.is_model_root(v) and v not in models:
                        models.append(v)
                nxt += [c for c in code if c not in seen]
            frontier, depth = nxt, depth + 1
        return models, len(seen)

    def object_lists(self):
        """{list VA: [(name, loadFunc), ...]}"""
        out = {}
        o = 0
        while o + 16 <= self.N:
            if self.is_code(self.u32(o + 8)) and self.cstr(self.u32(o + 12)):
                s, ents = o, []
                while o + 16 <= self.N and self.is_code(self.u32(o + 8)) and \
                        self.cstr(self.u32(o + 12)):
                    ents.append((self.cstr(self.u32(o + 12)), self.u32(o + 8)))
                    o += 16
                if len(ents) >= 8:
                    out[self.base + s] = ents
            else:
                o += 4
        return out


if __name__ == "__main__":
    t = Tracer(sys.argv[1])
    lists = t.object_lists()
    print(f"{len(lists)} object lists")
    lv = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x00b06750
    ents = lists[lv]
    print(f"list 0x{lv:08x}: {len(ents)} entries")
    for i, (nm, fn) in enumerate(ents[:14]):
        models, nf = t.models_for(fn)
        print(f"  [{i:3d}] {nm:14s} load 0x{fn:08x} funcs={nf:3d} models={[hex(m) for m in models[:3]]}")
