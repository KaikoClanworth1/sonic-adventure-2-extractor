import os, sys, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress, gvm_parse

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC"

def gvm_parse_be(data):
    """GVM directory: LE header size, BE flags/count/entries."""
    assert data[:4] == b"GVMH"
    hdr = struct.unpack_from("<I", data, 4)[0]
    flags = struct.unpack_from(">H", data, 8)[0]
    count = struct.unpack_from(">H", data, 10)[0]
    has_names = bool(flags & 8); has_fmt = bool(flags & 4)
    has_dims = bool(flags & 2);  has_gbix = bool(flags & 1)
    pos = 12
    ents = []
    for _ in range(count):
        idx = struct.unpack_from(">H", data, pos)[0]; pos += 2
        name = ""
        if has_names:
            name = data[pos:pos+28].split(b"\0")[0].decode("latin-1").strip(); pos += 28
        fmt = None
        if has_fmt:
            fmt = (data[pos], data[pos+1]); pos += 2
        dims = None
        if has_dims:
            dims = struct.unpack_from(">H", data, pos)[0]; pos += 2
        gbix = None
        if has_gbix:
            gbix = struct.unpack_from(">I", data, pos)[0]; pos += 4
        ents.append(dict(index=idx, name=name, fmt=fmt, dims=dims, gbix=gbix))
    return dict(header_size=hdr, flags=flags, count=count, entries=ents,
                tex_start=hdr + 8)

fmts = collections.Counter()
dfmts = collections.Counter()
count_arch = 0
total_tex = 0
samples = []
for dp, dn, fns in os.walk(ROOT):
    for fn in fns:
        if not fn.lower().endswith(".prs"): continue
        p = os.path.join(dp, fn)
        try:
            out = bytes(prs_decompress(open(p, "rb").read()))
        except Exception:
            continue
        if out[:4] != b"GVMH": continue
        try:
            g = gvm_parse_be(out)
        except Exception as e:
            continue
        count_arch += 1
        total_tex += g["count"]
        # walk texture blobs
        o = g["tex_start"]
        for i in range(g["count"]):
            if o + 32 > len(out): break
            if out[o:o+4] == b"GBIX":
                gl = struct.unpack_from("<I", out, o+4)[0]
                o2 = o + 8 + gl
            else:
                o2 = o
            if out[o2:o2+4] != b"GVRT":
                break
            dlen = struct.unpack_from("<I", out, o2+4)[0]
            # GVR header after GVRT+len : u16 pad, u8 fmt(palette/flags), u8 datafmt,
            #                             u16 width, u16 height   (big-endian)
            pal_fmt = out[o2+10]
            data_fmt = out[o2+11]
            w = struct.unpack_from(">H", out, o2+12)[0]
            h = struct.unpack_from(">H", out, o2+14)[0]
            fmts[data_fmt] += 1
            dfmts[(pal_fmt, data_fmt)] += 1
            if len(samples) < 8:
                samples.append((fn, g["entries"][i]["name"] if i < len(g["entries"]) else "?",
                                pal_fmt, data_fmt, w, h, dlen))
            o = o2 + 8 + dlen

print(f"GVM archives: {count_arch}   textures: {total_tex}")
print("\nGVR data formats:")
GVRFMT = {0:"Intensity4",1:"Intensity8",2:"IntensityA4",3:"IntensityA8",
          4:"RGB565",5:"RGB5A3",6:"ARGB8888",8:"Index4",9:"Index8",
          10:"Index14X2",14:"DXT1"}
for f, c in fmts.most_common():
    print(f"  {f:3d} {GVRFMT.get(f,'?'):14s} {c}")
print("\n(paletteFmt, dataFmt) combos:", dfmts.most_common(12))
print("\nsamples:")
for s in samples:
    print("  ", s)
