# Print the layout of Trauma Team .tpx files (containers of GX TPL texture files).
import struct, sys
def be(fmt, d, o): return struct.unpack_from('>' + fmt, d, o)
def tpx_entries(d):
    magic, nsprites, sprite_tab, n = be('IIII', d, 0)   # header: sprite count + table, texture count
    out = []
    for i in range(n):
        idx, off, size = be('III', d, 0x20 + 0x20 * i)
        name = d[0x2C + 0x20 * i: 0x40 + 0x20 * i].split(b'\0')[0].decode('latin1')
        out.append((idx, off, size, name))
    return out
def tpl_images(d, base):
    magic, n, tab = be('III', d, base)
    assert magic == 0x0020AF30, hex(magic)
    imgs = []
    for i in range(n):
        ih, ph = be('II', d, base + tab + 8 * i)
        h, w, fmt, data, ws, wt, minf, magf, lod, edge, mn, mx, un = be('HHIIIIIIfBBBB', d, base + ih)
        pal = None
        if ph:
            ne, unp, pad, pfmt, pdata = be('HBBII', d, base + ph)
            pal = (ne, pfmt, pdata)
        imgs.append(dict(w=w, h=h, fmt=fmt, data=data, pal=pal, mips=mx))
    return imgs
if __name__ == '__main__':
    for f in sys.argv[1:]:
        d = open(f, 'rb').read()
        for idx, off, size, name in tpx_entries(d):
            for im in tpl_images(d, off):
                print(f, idx, name, im)

def decode_ci(d, base, im):
    """Decode a CI4/CI8 image to palette indices (row-major)."""
    w, h, fmt = im['w'], im['h'], im['fmt']
    bw, bh = (8, 8) if fmt == 8 else (8, 4)
    out = [0] * (w * h)
    p = base + im['data']
    for by in range(0, h, bh):
        for bx in range(0, w, bw):
            for y in range(bh):
                for x in range(bw):
                    if fmt == 8:
                        k = y * 8 + x; v = d[p + k // 2]; v = (v >> 4) if k % 2 == 0 else (v & 15)
                    else:
                        v = d[p + y * 8 + x]
                    if bx + x < w and by + y < h: out[(by + y) * w + bx + x] = v
            p += 32
    return out

M64 = (1 << 64) - 1
def hash_bytes(b, h):
    """Same as hash_bytes() in recomp/runtime/video.c (little-endian 8-byte words)."""
    n = len(b); i = 0
    while i + 8 <= n:
        v = struct.unpack_from('<Q', b, i)[0]
        h = ((h ^ v) * 0x100000001B3) & M64; h ^= h >> 29; i += 8
    while i < n:
        h = ((h ^ b[i]) * 0x100000001B3) & M64; i += 1
    return h
def tex_bytes(im):
    bw, bh = (8, 8) if im['fmt'] == 8 else (8, 4)
    return ((im['w'] + bw - 1) // bw) * ((im['h'] + bh - 1) // bh) * 32
def pair_hashes(d, off, im):
    """Runtime hashes of the two halves (palette A = R,G; palette B = B,A) of a dual-palette CI image."""
    data = hash_bytes(d[off + im['data']: off + im['data'] + tex_bytes(im)], 0x9E3779B97F4A7C15)
    ne, pf, pd = im['pal']; n = ne * 2
    pa = d[off + pd: off + pd + n]; pb = d[off + pd + n: off + pd + 2 * n]
    return hash_bytes(pa, data), hash_bytes(pb, data)

def pair_key(a, b):
    """Same as texpack_pair_key() in recomp/runtime/texpack.c."""
    k = a ^ ((b * 0x9E3779B97F4A7C15) & M64)
    k ^= k >> 31; k = (k * 0xBF58476D1CE4E5B9) & M64; k ^= k >> 29
    return k

def lz10(src, pos=0):
    """Nintendo LZ77 (type 0x10) decompression."""
    assert src[pos] == 0x10
    size = src[pos + 1] | src[pos + 2] << 8 | src[pos + 3] << 16
    out = bytearray(); p = pos + 4
    while len(out) < size:
        flags = src[p]; p += 1
        for bit in range(8):
            if len(out) >= size: break
            if flags & (0x80 >> bit):
                b0, b1 = src[p], src[p + 1]; p += 2
                n = (b0 >> 4) + 3; disp = ((b0 & 15) << 8 | b1) + 1
                for _ in range(n): out.append(out[-disp])
            else:
                out.append(src[p]); p += 1
    return bytes(out)

def pak_entries(d):
    """Trauma Team .pak: [name 0xFC bytes][u32 LE compressed size][LZ10 data] ..."""
    p = 0
    while p + 0x100 <= len(d):
        raw = d[p:p + 0xFC].split(b'\0')[0]
        if not raw or not all(32 <= c < 127 for c in raw) or b'.' not in raw: break
        name = raw.decode('latin1')
        csize = struct.unpack_from('<I', d, p + 0xFC)[0]
        yield name, d, p + 0x100, csize
        p += 0x100 + csize
        p = (p + 3) & ~3 if p < len(d) and d[p] == 0 else p
