"""Extract every texture on the Trauma Team disc as a PNG named for the texture pack.

    python tools/extract_textures.py <disc/DATA/files> <output folder>

Output: <output>/<disc folder>/<file>/tt_<hash>_<name>_<w>x<h>.png - the runtime (recomp/runtime/texpack.c)
reads only the "tt_<16 hex digits>" prefix, so files can be moved and renamed after that.

What is extracted:
  * .tpx sprite sheets (all 2D art). Their CI4/CI8 images carry two IA8 palettes that the game binds to
    two texture maps (palette A = red + green, palette B = blue + alpha); they are written here as one
    full-colour RGBA image, keyed like video.c's resolve_pairs() does at runtime.
  * BRRES TEX0 textures (3D models, effects; also inside .pak archives), keyed by their data hash.
  * Plain TPL files found anywhere else (.arc etc.).
The keys reproduce the hashes the game computes at runtime (see hash_bytes / tex_data_hash in video.c),
so a PNG edited or upscaled from this folder replaces that texture in game.
"""
import os, struct, sys
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tpx_info import tpx_entries, tpl_images, hash_bytes, pair_key, be, lz10, pak_entries

SEED = 0x9E3779B97F4A7C15
BLOCK = {0: (8, 8, 4), 1: (8, 4, 8), 2: (8, 4, 8), 3: (4, 4, 16), 4: (4, 4, 16), 5: (4, 4, 16), 6: (4, 4, 32),
         8: (8, 8, 4), 9: (8, 4, 8), 10: (4, 4, 16), 14: (8, 8, 4)}   # block w, h, bits per pixel

def tex_size(w, h, fmt):
    bw, bh, bpp = BLOCK[fmt]
    return ((w + bw - 1) // bw) * ((h + bh - 1) // bh) * bw * bh * bpp // 8

def untile(raw, w, h, bw, bh, per_px):
    """Blocks of bw x bh pixels (each pixel per_px values) -> h x w x per_px array."""
    nbx, nby = (w + bw - 1) // bw, (h + bh - 1) // bh
    a = raw.reshape(nby, nbx, bh, bw, per_px).transpose(0, 2, 1, 3, 4).reshape(nby * bh, nbx * bw, per_px)
    return a[:h, :w]

def rgb565(v):
    r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), np.full_like(v, 255)], -1).astype(np.uint8)

def rgb5a3(v):
    opaque = (v & 0x8000) != 0
    r = np.where(opaque, ((v >> 10) & 31) * 255 // 31, ((v >> 8) & 15) * 17)
    g = np.where(opaque, ((v >> 5) & 31) * 255 // 31, ((v >> 4) & 15) * 17)
    b = np.where(opaque, (v & 31) * 255 // 31, (v & 15) * 17)
    a = np.where(opaque, 255, ((v >> 12) & 7) * 255 // 7)
    return np.stack([r, g, b, a], -1).astype(np.uint8)

def ia8(v):   # 16-bit (A << 8 | I) -> RGBA (I, I, I, A)
    i = v & 255; a = v >> 8
    return np.stack([i, i, i, a], -1).astype(np.uint8)

def palette(raw, pfmt):
    v = np.frombuffer(raw, '>u2').astype(np.int64)
    return ia8(v) if pfmt == 0 else rgb565(v) if pfmt == 1 else rgb5a3(v)

def indices(data, w, h, fmt):
    bw, bh, bpp = BLOCK[fmt]
    if len(data) < tex_size(w, h, fmt): data = bytes(data) + bytes(tex_size(w, h, fmt) - len(data))
    raw = np.frombuffer(data[:tex_size(w, h, fmt)], np.uint8)
    if fmt == 8: raw = np.stack([raw >> 4, raw & 15], -1).reshape(-1)
    elif fmt == 10: raw = np.frombuffer(data[:tex_size(w, h, fmt)], '>u2')
    return untile(raw, w, h, bw, bh, 1)[..., 0].astype(np.int64)

def decode(data, w, h, fmt, pal=None):
    """GX texture -> h x w x 4 RGBA (uint8). pal: RGBA palette array for CI formats."""
    bw, bh, bpp = BLOCK[fmt]
    n = tex_size(w, h, fmt)
    if len(data) < n: data = bytes(data) + bytes(n - len(data))   # a few images run past their file
    raw = np.frombuffer(data[:n], np.uint8)
    if fmt == 0:
        v = np.stack([raw >> 4, raw & 15], -1).reshape(-1) * 17
        i = untile(v, w, h, bw, bh, 1)[..., 0]; return np.stack([i, i, i, i], -1).astype(np.uint8)
    if fmt == 1:
        i = untile(raw, w, h, bw, bh, 1)[..., 0]; return np.stack([i, i, i, i], -1)
    if fmt == 2:
        v = untile(raw, w, h, bw, bh, 1)[..., 0]; i = (v & 15) * 17; a = (v >> 4) * 17
        return np.stack([i, i, i, a], -1).astype(np.uint8)
    if fmt == 3:
        v = untile(raw, w, h, bw, bh, 2); a, i = v[..., 0], v[..., 1]
        return np.stack([i, i, i, a], -1)
    if fmt in (4, 5):
        v = untile(np.frombuffer(data[:n], '>u2').astype(np.int64), w, h, bw, bh, 1)[..., 0]
        return rgb565(v) if fmt == 4 else rgb5a3(v)
    if fmt == 6:   # 4x4 tiles: 32 bytes AR, then 32 bytes GB
        nbx, nby = (w + 3) // 4, (h + 3) // 4
        t = raw.reshape(nby, nbx, 2, 16, 2)
        ar, gb = t[:, :, 0], t[:, :, 1]
        px = np.stack([ar[..., 1], gb[..., 0], gb[..., 1], ar[..., 0]], -1)   # (nby, nbx, 16, 4)
        return px.reshape(nby, nbx, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(nby * 4, nbx * 4, 4)[:h, :w]
    if fmt in (8, 9, 10):
        return pal[np.clip(indices(data, w, h, fmt), 0, len(pal) - 1)]
    if fmt == 14:
        return cmpr(raw, w, h)
    raise ValueError(fmt)

def cmpr(raw, w, h):
    nbx, nby = (w + 7) // 8, (h + 7) // 8
    out = np.zeros((nby * 8, nbx * 8, 4), np.uint8)
    sub = raw.reshape(nby, nbx, 4, 8)   # each 8x8 block = 2x2 DXT1 blocks
    c0 = (sub[..., 0].astype(np.int64) << 8) | sub[..., 1]; c1 = (sub[..., 2].astype(np.int64) << 8) | sub[..., 3]
    p0, p1 = rgb565(c0).astype(np.int64), rgb565(c1).astype(np.int64)
    gt = (c0 > c1)[..., None]
    p2 = np.where(gt, (2 * p0 + p1) // 3, (p0 + p1) // 2); p3 = np.where(gt, (p0 + 2 * p1) // 3, 0)
    p2[..., 3] = 255; p3[..., 3] = np.where(gt[..., 0], 255, 0)
    pal = np.stack([p0, p1, p2, p3], -2)   # (nby, nbx, 4 sub, 4 colours, 4)
    bits = sub[..., 4:8].astype(np.int64)  # 4 rows x 4 px, 2 bits each, msb first
    idx = np.stack([(bits >> (6 - 2 * k)) & 3 for k in range(4)], -1)   # (nby, nbx, 4 sub, 4 rows, 4 cols)
    # straightforward gather
    P = pal[:, :, :, None, None, :, :]                       # (nby,nbx,4,1,1,4c,4ch)
    I = idx[..., None, None]                                 # (nby,nbx,4,4,4,1,1)
    px = np.take_along_axis(np.broadcast_to(P, idx.shape + (4, 4)), np.broadcast_to(I, idx.shape + (1, 4)), axis=-2)[..., 0, :]
    for s in range(4):
        sy, sx = (s // 2) * 4, (s % 2) * 4
        blk = px[:, :, s]                                    # (nby, nbx, 4, 4, 4)
        out.reshape(nby, 8, nbx, 8, 4)[:, sy:sy + 4, :, sx:sx + 4] = blk.transpose(0, 2, 1, 3, 4)
    return out[:h, :w].astype(np.uint8)

# ----------------------------------------------------------------------------------------- writers
def safe(s): return ''.join(c if c.isalnum() or c in '-_.' else '_' for c in s)[:48]

class Out:
    def __init__(self, root): self.root, self.seen, self.n = root, set(), 0
    def save(self, key, rel, name, rgba):
        if key in self.seen: return
        self.seen.add(key)
        d = os.path.join(self.root, rel); os.makedirs(d, exist_ok=True)
        h, w = rgba.shape[:2]
        Image.fromarray(np.ascontiguousarray(rgba), 'RGBA').save(os.path.join(d, 'tt_%016X_%s_%dx%d.png' % (key, safe(name), w, h)), optimize=False)
        self.n += 1

def do_tpx(path, rel, out):
    d = open(path, 'rb').read()
    for idx, off, size, name in tpx_entries(d):
        if off + 16 > len(d) or be('I', d, off)[0] != 0x0020AF30: continue
        for k, im in enumerate(tpl_images(d, off)):
            w, h, fmt = im['w'], im['h'], im['fmt']
            data = d[off + im['data']: off + im['data'] + tex_size(w, h, fmt)]
            dh = hash_bytes(data, SEED)
            if im['pal'] and im['pal'][1] == 0xFF:   # dual IA8 palettes -> one RGBA image
                ne, pf, pd = im['pal']
                pa = d[off + pd: off + pd + 2 * ne]; pb = d[off + pd + 2 * ne: off + pd + 4 * ne]
                key = pair_key(hash_bytes(pa, dh), hash_bytes(pb, dh))
                A = decode(data, w, h, fmt, palette(pa, 0)); B = decode(data, w, h, fmt, palette(pb, 0))
                rgba = np.stack([A[..., 0], A[..., 3], B[..., 0], B[..., 3]], -1)
            elif im['pal']:
                ne, pf, pd = im['pal']; praw = d[off + pd: off + pd + 2 * ne]
                key = hash_bytes(praw, dh ^ pf); rgba = decode(data, w, h, fmt, palette(praw, pf))
            else:
                key = dh; rgba = decode(data, w, h, fmt)
            out.save(key, rel, name + ('' if k == 0 else '_%d' % k), rgba)

def do_tex0(d, rel, out):
    pos = 0
    while True:
        pos = d.find(b'TEX0', pos)
        if pos < 0: return
        try:
            size, ver, _, doff, noff = be('IIiIi', d, pos + 4)
            w, h, fmt = be('HHI', d, pos + 0x1C)
            ok = 0 < w <= 1024 and 0 < h <= 1024 and fmt in BLOCK and fmt not in (8, 9, 10) and pos + doff + tex_size(w, h, fmt) <= len(d)
        except struct.error:
            ok = False
        if ok:
            data = d[pos + doff: pos + doff + tex_size(w, h, fmt)]
            name = 'tex'
            if 0 < pos + noff < len(d):
                name = d[pos + noff: pos + noff + 48].split(b'\0')[0].decode('latin1', 'replace') or 'tex'
            try: out.save(hash_bytes(data, SEED), rel, name, decode(data, w, h, fmt))
            except Exception as e: print('  skip TEX0 %s in %s: %s' % (name, rel, e))
        pos += 4

def do_brfnt(d, rel, out):
    """Font glyph sheets (TGLP section of an RFNT file)."""
    pos = d.find(b'TGLP')
    if pos < 0: return
    ssize, scount, sfmt, cols, rows, sw, sh, soff = be('IHHHHHHI', d, pos + 0x0C)
    fmt = sfmt & 0x7FFF
    if fmt not in BLOCK or fmt in (8, 9, 10): return
    for k in range(scount):
        data = d[soff + k * ssize: soff + (k + 1) * ssize]
        out.save(hash_bytes(data[:tex_size(sw, sh, fmt)], SEED), rel, 'sheet%d' % k, decode(data, sw, sh, fmt))

def do_tpl_blobs(d, rel, out):
    pos = 0
    while True:
        pos = d.find(b'\x00\x20\xaf\x30', pos)
        if pos < 0: return
        try:
            for k, im in enumerate(tpl_images(d, pos)):
                w, h, fmt = im['w'], im['h'], im['fmt']
                if fmt not in BLOCK or not (0 < w <= 1024 and 0 < h <= 1024): continue
                data = d[pos + im['data']: pos + im['data'] + tex_size(w, h, fmt)]
                dh = hash_bytes(data, SEED)
                if fmt in (8, 9, 10):
                    ne, pf, pd = im['pal']; praw = d[pos + pd: pos + pd + 2 * ne]
                    if pf == 0xFF: continue
                    out.save(hash_bytes(praw, dh ^ pf), rel, 'tpl%d' % k, decode(data, w, h, fmt, palette(praw, pf)))
                else:
                    out.save(dh, rel, 'tpl%d' % k, decode(data, w, h, fmt))
        except Exception:
            pass
        pos += 4

def main():
    src, dst = sys.argv[1], sys.argv[2]
    out = Out(dst)
    files = sorted(os.path.join(r, f) for r, _, fs in os.walk(src) for f in fs)
    for i, p in enumerate(files):
        rel = os.path.relpath(p, src)
        ext = p.rsplit('.', 1)[-1].lower()
        if ext in ('brstm', 'brsar', 'bm2', 'thp', 'sfd') or '.svn' in rel: continue
        before = out.n
        try:
            if ext == 'tpx': do_tpx(p, rel, out)
            elif ext == 'brfnt': do_brfnt(open(p, 'rb').read(), rel, out)
            elif ext == 'pak':   # LZ10-compressed archives (models, effects, operation data)
                d = open(p, 'rb').read()
                for name, dd, off, cs in pak_entries(d):
                    if dd[off] != 0x10: continue
                    x = lz10(dd, off)
                    if b'TEX0' in x: do_tex0(x, os.path.join(rel, os.path.basename(name)), out)
                    if b'\x00\x20\xaf\x30' in x: do_tpl_blobs(x, os.path.join(rel, os.path.basename(name)), out)
            else:
                d = open(p, 'rb').read()
                if b'TEX0' in d: do_tex0(d, rel, out)
                if b'\x00\x20\xaf\x30' in d: do_tpl_blobs(d, rel, out)
        except Exception as e:
            print('error in %s: %s' % (rel, e))
        if out.n != before and (i % 50 == 0): print('%5d/%d %s  (%d textures)' % (i, len(files), rel, out.n), flush=True)
    print('done: %d textures' % out.n)

if __name__ == '__main__':
    main()
