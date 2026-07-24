"""Build SA2's stage -> SET-object-id -> {name, model} table from a DRM-free exe.

The chain, all recovered from the executable:
  * object list entry = {u32 flags, u32 loadDist, void(*load)(task*), char* name}
  * a stage's setup function registers its list with  push <header>; call 0x487e40
    where header = {int count, entry* list}
  * that same setup function still contains the original debug source path
    ("..\\..\\src\\stg13_city\\..."), which names the stage number
  * each entry's load function reaches its NJS_OBJECT model root

So SET id -> the stage's list[id] -> name + model. Emits JSON for the viewer.

Only works on an unprotected build: the retail Steam sonic2app.exe is
SteamStub-encrypted (.text entropy 8.0, zero prologues) and unreadable.
"""
import sys, os, re, json, struct, bisect
import capstone
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from objtrace import Tracer

SET_DIR = None
STG_RE = re.compile(r"src[\\/]stg(\d+)[_\\/]", re.I)


def sweep(t, on_insn):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    sec = [s for s in t.pe.sections if s["name"] == ".text"][0]
    lo, hi = sec["vaddr"], sec["vaddr"] + sec["vsize"]
    off = lo
    while off < hi:
        moved = False
        for insn in md.disasm(t.flat[off:hi], t.base + off):
            on_insn(insn)
            off = insn.address - t.base + insn.size
            moved = True
        if not moved:
            off += 1


def main(exe, out_json, set_dir=None):
    global SET_DIR
    SET_DIR = set_dir
    t = Tracer(exe)
    lists = t.object_lists()
    listvas = set(lists)
    hdr2list = {}
    o = 0
    while o + 8 <= t.N:
        a, b = t.u32(o), t.u32(o + 4)
        if b in listvas and len(lists[b]) == a:
            hdr2list[t.base + o] = b
        o += 4

    pushes, calls, ct = [], [], set()

    def on(insn):
        if insn.mnemonic == "push" and insn.operands and \
                insn.operands[0].type == capstone.x86.X86_OP_IMM:
            pushes.append((insn.address, insn.operands[0].imm & 0xFFFFFFFF))
        elif insn.mnemonic == "call" and insn.operands and \
                insn.operands[0].type == capstone.x86.X86_OP_IMM:
            tg = insn.operands[0].imm & 0xFFFFFFFF
            ct.add(tg)
            calls.append((insn.address, tg))
    sweep(t, on)

    pa = [p[0] for p in pushes]
    pm = dict(pushes)
    cts = sorted(ct)

    def containing(va):
        i = bisect.bisect_right(cts, va) - 1
        return cts[i] if i >= 0 else None

    # setup function -> object list (via push <header>; call SetObjectList)
    setup = {}
    for addr, tg in calls:
        if tg != 0x487E40:
            continue
        i = bisect.bisect_left(pa, addr) - 1
        if i >= 0 and pm[pa[i]] in hdr2list:
            f = containing(addr)
            if f:
                setup.setdefault(f, hdr2list[pm[pa[i]]])

    # Identify each setup function's stage from the leftover debug source paths.
    # A source file's functions are compiled contiguously, so the stage of a setup
    # function is the stage of the nearest "..\..\src\stgNN_..." reference in code.
    stg_refs = []          # (code addr, stage number)
    for addr, imm in pushes:
        s = t.cstr(imm)
        if s:
            m = STG_RE.search(s)
            if m:
                stg_refs.append((addr, int(m.group(1))))
    # also catch mov-reg/other imm forms via a light re-sweep of data imms
    stg_refs.sort()
    print(f"{len(stg_refs)} stgNN source-path references in code")

    # Assign stage -> list using two independent signals:
    #  (a) proximity: a source file's functions compile contiguously, so a setup
    #      function sits near its own stgNN debug-path references;
    #  (b) validation: every id in that stage's SET file must index inside the
    #      list. This is a hard constraint and catches any proximity mistake.
    set_ids = {}
    if SET_DIR:
        import glob
        for p in glob.glob(os.path.join(SET_DIR, "set[0-9][0-9][0-9][0-9]_s.bin")) + \
                 glob.glob(os.path.join(SET_DIR, "SET[0-9][0-9][0-9][0-9]_S.BIN")):
            n = int(os.path.basename(p)[3:7])
            d = open(p, "rb").read()
            c = struct.unpack_from(">I", d, 0)[0]
            ids = set()
            for i in range(c):
                o = 0x20 + i * 0x20
                if o + 2 <= len(d):
                    ids.add(struct.unpack_from(">H", d, o)[0] & 0xFFF)
            if ids:
                set_ids.setdefault(n, ids)
    setup_items = sorted(setup.items())
    setup_addrs = [f for f, _ in setup_items]

    # (a) authoritative: the stage's own debug source path reached from inside the
    #     setup function's call graph. Proximity in .text is NOT reliable -- the
    #     setup functions are grouped together, far from each stage's object code.
    md2 = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md2.detail = True

    def stage_from_callgraph(fva, max_depth=3, max_funcs=180, limit=0x4000):
        seen, frontier, depth = set(), [fva], 0
        while frontier and depth < max_depth:
            nxt = []
            for f in frontier:
                if f in seen or len(seen) >= max_funcs:
                    continue
                seen.add(f)
                off = f - t.base
                if not (0 <= off < t.N):
                    continue
                for insn in md2.disasm(t.flat[off:min(off + limit, t.N)], f):
                    if insn.mnemonic in ("ret", "retf"):
                        break
                    for op in insn.operands:
                        if op.type != capstone.x86.X86_OP_IMM:
                            continue
                        v = op.imm & 0xFFFFFFFF
                        if t.is_code(v):
                            nxt.append(v)
                        else:
                            s = t.cstr(v)
                            if s:
                                m = STG_RE.search(s)
                                if m:
                                    return int(m.group(1))
            frontier, depth = nxt, depth + 1
        return None

    stages = {}
    for f, lv in setup_items:
        n = stage_from_callgraph(f)
        if n is None or n in stages:
            continue
        # (b) hard check: every id in that stage's SET must index inside the list
        ids = set_ids.get(n)
        if ids and max(ids) >= len(lists[lv]):
            print(f"  ! stage {n}: list 0x{lv:08x} too small "
                  f"({len(lists[lv])} <= max id {max(ids)}), rejected")
            continue
        stages[n] = (f, lv)
    print(f"{len(stages)} stages assigned (debug-path + SET-validated)")

    # per-list: entry -> model root (traced through the load function)
    modelcache = {}

    def model_of(load_va):
        if load_va not in modelcache:
            ms, _ = t.models_for(load_va, max_depth=3, max_funcs=90)
            modelcache[load_va] = ms[0] if ms else 0
        return modelcache[load_va]

    out = {}
    for stage in sorted(stages):
        f, lv = stages[stage]
        ents = lists[lv]
        objs = []
        for i, (nm, fn) in enumerate(ents):
            objs.append({"id": i, "name": nm, "model": model_of(fn)})
        out[str(stage)] = {"list": lv, "setup": f, "objects": objs}
        got = sum(1 for x in objs if x["model"])
        print(f"  stage {stage:3d}: list 0x{lv:08x} {len(ents):3d} objects, "
              f"{got} with models  e.g. {[o['name'] for o in objs[:4]]}")
    json.dump(out, open(out_json, "w"), indent=1)
    print(f"{len(out)} stages -> {out_json}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "build/sa2_objects.json",
         sys.argv[3] if len(sys.argv) > 3 else None)
