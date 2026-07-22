"""Batch regression: run PRS + PAK parsers over EVERY file in the game."""
import os, sys, collections, traceback
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress, pak_parse, gvm_parse, PAK_MAGIC

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC"


def walk(root, exts):
    for dp, dn, fns in os.walk(root):
        for fn in fns:
            if os.path.splitext(fn)[1].lower() in exts:
                yield os.path.join(dp, fn)


def test_pak():
    print("=" * 70)
    print("PAK REGRESSION")
    print("=" * 70)
    ok = fail = 0
    errors = []
    exact = 0
    inner_ext = collections.Counter()
    total_entries = 0
    for p in walk(ROOT, {".pak"}):
        try:
            with open(p, "rb") as f:
                data = f.read()
            r = pak_parse(data, load_data=False)
            # strong check: computed end of data must equal file size
            if r["data_end"] != len(data):
                raise ValueError(
                    f"size mismatch: computed end {r['data_end']} != file size {len(data)}")
            # header mirrors must agree
            if r["data_end"] - r["data_start"] != r["total_data"]:
                raise ValueError("total_data header field mismatch")
            exact += 1
            total_entries += r["count"]
            for e in r["entries"]:
                inner_ext[os.path.splitext(e.short_path)[1].lower()] += 1
            ok += 1
        except Exception as ex:
            fail += 1
            errors.append((p, str(ex)))
    print(f"  parsed OK   : {ok}")
    print(f"  exact size  : {exact}")
    print(f"  failed      : {fail}")
    print(f"  entries     : {total_entries}")
    print(f"  inner types : {dict(inner_ext.most_common(15))}")
    for p, e in errors[:10]:
        print(f"   ! {os.path.basename(p)}: {e}")
    return fail == 0


def test_prs():
    print("=" * 70)
    print("PRS REGRESSION")
    print("=" * 70)
    ok = fail = 0
    errors = []
    magics = collections.Counter()
    total_in = total_out = 0
    for p in walk(ROOT, {".prs"}):
        try:
            with open(p, "rb") as f:
                data = f.read()
            out = prs_decompress(data)
            if len(out) == 0:
                raise ValueError("empty output")
            total_in += len(data)
            total_out += len(out)
            m = bytes(out[:4])
            asc = "".join(chr(c) if 32 <= c < 127 else "." for c in m)
            magics[f"{asc}  {m.hex()}"] += 1
            ok += 1
        except Exception as ex:
            fail += 1
            errors.append((p, str(ex)))
    print(f"  decompressed OK : {ok}")
    print(f"  failed          : {fail}")
    if total_in:
        print(f"  {total_in/1e6:.1f} MB -> {total_out/1e6:.1f} MB "
              f"(ratio {total_out/total_in:.2f}x)")
    print("  output magics:")
    for m, c in magics.most_common(20):
        print(f"    {c:5d}  {m}")
    for p, e in errors[:15]:
        print(f"   ! {os.path.basename(p)}: {e}")
    return fail == 0


def test_gvm():
    print("=" * 70)
    print("GVM (inside PRS) REGRESSION")
    print("=" * 70)
    ok = fail = 0
    errors = []
    fmts = collections.Counter()
    ntex = 0
    for p in walk(ROOT, {".prs"}):
        try:
            with open(p, "rb") as f:
                data = f.read()
            out = bytes(prs_decompress(data))
            if out[:4] != b"GVMH":
                continue
            r = gvm_parse(out)
            ntex += r["count"]
            for e in r["entries"]:
                if e["format"]:
                    fmts[f"{e['format'][0]:02x}{e['format'][1]:02x}"] += 1
            ok += 1
        except Exception as ex:
            fail += 1
            errors.append((p, str(ex)))
    print(f"  GVM archives OK : {ok}")
    print(f"  failed          : {fail}")
    print(f"  textures        : {ntex}")
    print(f"  formats (fmt1fmt2): {dict(fmts.most_common(20))}")
    for p, e in errors[:10]:
        print(f"   ! {os.path.basename(p)}: {e}")
    return fail == 0


if __name__ == "__main__":
    a = test_pak()
    b = test_prs()
    c = test_gvm()
    print()
    print("RESULT:", "ALL PASS" if (a and b and c) else "FAILURES PRESENT")
