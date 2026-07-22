import sys, binascii

def dump(path, off=0, length=512):
    with open(path,'rb') as f:
        f.seek(off)
        data = f.read(length)
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hexs = ' '.join(f'{b:02x}' for b in chunk)
        asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        print(f'{off+i:08x}  {hexs:<48}  |{asc}|')

if __name__ == '__main__':
    p = sys.argv[1]
    off = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    ln = int(sys.argv[3], 0) if len(sys.argv) > 3 else 512
    import os
    print(f'FILE {p}  size={os.path.getsize(p)}')
    dump(p, off, ln)
