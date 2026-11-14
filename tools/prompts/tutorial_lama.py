"""Erase the Wii Remote / Nunchuk from a tutorial picture with LaMa (big-lama.pt, torchscript) and redraw the
bubble plates in a controller's symbols.  python tutorial_lama.py <orig dir> <upscaled dir> <out> --only HASH,..."""
import sys, os, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); sys.path.insert(0, "/tmp")
import numpy as np, cv2
from PIL import Image, ImageDraw
from scipy import ndimage
import make_tutorials as M
from make_prompts import draw_symbol, SYMBOL
from lama import lama


def device_mask(a, plates):
    m = M.find_remote(a, plates)
    if not m.any(): return m
    rows = m.sum(1); body = m.copy(); body[rows < 7] = False
    if body.any():
        ys, xs = np.nonzero(body); hull = cv2.convexHull(np.stack([xs, ys], 1).astype(np.int32))
        hm = np.zeros(m.shape, np.uint8); cv2.fillConvexPoly(hm, hull, 1); m |= hm.astype(bool)
    return ndimage.binary_dilation(m, iterations=4)


def erase(up, mask):
    """inpaint at 608 wide, paste the filled part back into the full picture with a soft edge"""
    H, W = up.shape[:2]
    s = cv2.resize(up, (608, round(608 * H / W)), interpolation=cv2.INTER_AREA)
    sm = cv2.resize(mask.astype(np.uint8), (s.shape[1], s.shape[0]), interpolation=cv2.INTER_NEAREST) > 0
    r = lama(s, sm)
    big = cv2.resize(r, (W, H), interpolation=cv2.INTER_CUBIC)
    a = cv2.resize(mask.astype(np.float32), (W, H), interpolation=cv2.INTER_LINEAR)
    a = cv2.GaussianBlur(a, (0, 0), W / 304 * 1.2)[..., None].clip(0, 1)
    return (up * (1 - a) + big * a).astype(np.uint8)


def plates(im, a, pl, style, S):
    for x, y, w, h, k in pl:
        sym = SYMBOL[style].get({"plus": "plus", "minus": "minus"}.get(k, k))
        if not sym: continue
        Wp, Hp = round((w + 1) * S), round((h + 1) * S)
        p = Image.new("RGBA", (Wp, Hp), (0, 0, 0, 0))
        ImageDraw.Draw(p).rounded_rectangle([0, 0, Wp - 1, Hp - 1], radius=min(Wp, Hp) * 0.2, fill=(214, 214, 220, 255))
        g, _ = draw_symbol(sym[0], sym[1], round((w - 3) * S), round((h - 3) * S), (24, 24, 24, 255))
        p.alpha_composite(g, (round(2 * S), round(2 * S)))
        im.alpha_composite(p, (round((x - 0.5) * S), round((y - 0.5) * S)))


def render(orig_path, up_path, styles=("keyboard", "xbox", "ps4", "ps5")):
    a = np.array(Image.open(orig_path).convert("RGB")).astype(int)
    pl = M.find_plates(a) + M.find_plates(a, small=True)
    big_pl = M.find_plates(a)
    up = np.array(Image.open(up_path).convert("RGB")); S = up.shape[1] / a.shape[1]
    m = device_mask(a, big_pl) if big_pl else np.zeros(a.shape[:2], bool)
    base = erase(up, m) if m.any() else up
    out = {}
    for st in styles:
        im = Image.fromarray(base).convert("RGBA"); plates(im, a, pl, st, S); out[st] = im.convert("RGB")
    return out
