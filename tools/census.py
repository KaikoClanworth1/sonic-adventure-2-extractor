"""Census of magic bytes across every file in the SA2 game folder."""
import os, sys, collections, binascii

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2\resource\gd_PC"

def printable(b):
    return ''.join(chr(c) if 32 <= c < 127 else '.' for c in b)

by_ext = collections.defaultdict(collections.Counter)
sizes = collections.defaultdict(list)

for dirpath, dirnames, filenames in os.walk(ROOT):
    for fn in filenames:
        p = os.path.join(dirpath, fn)
        ext = os.path.splitext(fn)[1].lower() or '(none)'
        try:
            with open(p, 'rb') as f:
                head = f.read(16)
        except Exception as e:
            continue
        sz = os.path.getsize(p)
        magic = printable(head[:8]) + ' | ' + binascii.hexlify(head[:8]).decode()
        by_ext[ext][magic] += 1
        sizes[ext].append(sz)

for ext in sorted(by_ext, key=lambda e: -sum(by_ext[e].values())):
    total = sum(by_ext[ext].values())
    print(f"\n=== {ext}  ({total} files) ===")
    for magic, cnt in by_ext[ext].most_common(12):
        print(f"  {cnt:5d}  {magic}")
