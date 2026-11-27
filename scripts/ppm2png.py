#!/usr/bin/env python3
"""ppm2png.py in.ppm out.png — binary P6 (maxval 255) to RGB PNG, stdlib only."""
import sys, zlib, struct

def read_ppm(path):
    d = open(path, 'rb').read()
    toks, i = [], 0
    while len(toks) < 4:
        while d[i:i+1].isspace(): i += 1
        if d[i:i+1] == b'#':
            while d[i:i+1] not in (b'\n', b''): i += 1
            continue
        j = i
        while not d[j:j+1].isspace(): j += 1
        toks.append(d[i:j]); i = j
    i += 1
    if toks[0] != b'P6' or int(toks[3]) != 255: raise SystemExit('need P6 maxval 255')
    w, h = int(toks[1]), int(toks[2])
    return w, h, d[i:i + w*h*3]

def write_png(path, w, h, rgb):
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    row = w * 3
    raw = b''.join(b'\0' + rgb[y*row:(y+1)*row] for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

if __name__ == '__main__':
    w, h, px = read_ppm(sys.argv[1]); write_png(sys.argv[2], w, h, px); print(f'{sys.argv[2]} {w}x{h}')
