import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2fmt import prs_decompress

p = sys.argv[1]
off = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
ln = int(sys.argv[3], 0) if len(sys.argv) > 3 else 256
with open(p, 'rb') as f:
    data = prs_decompress(f.read())
print(f'{os.path.basename(p)}  decompressed size = {len(data)}')
chunk = data[off:off+ln]
for i in range(0, len(chunk), 16):
    c = chunk[i:i+16]
    print(f'{off+i:08x}  ' + ' '.join(f'{b:02x}' for b in c).ljust(48) +
          '  |' + ''.join(chr(b) if 32 <= b < 127 else '.' for b in c) + '|')
