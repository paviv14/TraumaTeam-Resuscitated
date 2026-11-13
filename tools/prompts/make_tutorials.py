"""Tutorial pictures for keyboard & mouse and controllers.

The game's tutorial pages (tutorial/<mode>/tuto_*.tpx, 304x232) are Wii screenshots with a Wii Remote pasted in under
a speech bubble that shows the button to press ("A", "A+B"). For each controller style this finds the bubble's plates
and the Remote under them (in the original picture), paints the Remote out of the (upscaled) picture, draws a mouse or
a controller in its place with the pressed buttons lit, and redraws the plates' symbols as make_prompts.py does.

    python make_tutorials.py <original tutorial folder> <upscaled tutorial folder> <out> [--only name,name]

Output: <out>/<style>/tt_<id>_<name>.jpg (the runtime loads .jpg prompt pictures too). Pictures with no bubble found
are left alone (the pack's picture shows).
"""
import os, sys, glob, json, argparse
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from scipy import ndimage
import cv2
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_prompts import draw_symbol, SYMBOL

STYLES = ("keyboard", "xbox", "ps4", "ps5")


# ---------------------------------------------------------------------------------------------- finding things
def find_plates(a):
    """Grey button plates (A, B, +, -) inside dark speech bubbles: [(x, y, w, h, kind)] in original pixels."""
    mx = a.max(2); mn = a.min(2)
    g = (mx > 150) & (mx - mn < 28); dark = mx < 50
    lab, _ = ndimage.label(g); out = []
    for i, s in enumerate(ndimage.find_objects(lab)):
        ph = s[0].stop - s[0].start; pw = s[1].stop - s[1].start
        if not (14 <= ph <= 30 and 10 <= pw <= 26 and ph > pw * 1.05): continue
        reg = ndimage.binary_fill_holes(lab[s] == i + 1)
        if reg.mean() < 0.8: continue
        inner = dark[s] & reg
        if inner.sum() < 6: continue
        y0, x0 = max(0, s[0].start - 3), max(0, s[1].start - 3)
        ring = dark[y0:s[0].stop + 3, x0:s[1].stop + 3].copy()
        ring[s[0].start - y0:s[0].stop - y0, s[1].start - x0:s[1].stop - x0] = False
        if ring.mean() < 0.35: continue
        out.append((s[1].start, s[0].start, pw, ph, classify(inner)))
    return out


def classify(inner):
    """A (round), B (square), minus / plus (bars) from the plate's dark symbol."""
    h, w = inner.shape
    ys, xs = np.nonzero(inner)
    if len(ys) == 0: return "A"
    bh = ys.max() - ys.min() + 1; bw = xs.max() - xs.min() + 1
    if bh < h * 0.35: return "minus"
    # corners of the symbol's box: square (B) fills them, a circle (A) doesn't
    y0, x0 = ys.min(), xs.min(); c = max(1, bw // 5)
    corners = [inner[y0:y0 + c, x0:x0 + c].mean(), inner[y0:y0 + c, x0 + bw - c:x0 + bw].mean(),
               inner[y0 + bh - c:y0 + bh, x0:x0 + c].mean(), inner[y0 + bh - c:y0 + bh, x0 + bw - c:x0 + bw].mean()]
    fill = inner[y0:y0 + bh, x0:x0 + bw].mean()
    if fill < 0.45: return "plus"
    return "B" if np.mean(corners) > 0.6 else "A"


def find_remote(a, plates):
    """The Wii Remote (and its cable) under the bubble: a mask in original pixels."""
    mx = a.max(2); mn = a.min(2); H, W = mx.shape
    px0 = min(p[0] for p in plates); px1 = max(p[0] + p[2] for p in plates); pb = max(p[1] + p[3] for p in plates)
    light = (mx > 125) & (mx - mn < 32)
    box = np.zeros_like(light); box[pb + 4:H - 6, max(5, px0 - 22):min(W - 5, px1 + 26)] = True
    light &= box
    ll, _ = ndimage.label(light)
    seeds = set(ll[pb + 6:min(H, pb + 28), px0:px1].ravel()) - {0}
    m = np.isin(ll, list(seeds))
    sl, _ = ndimage.label(m); keep = np.zeros_like(m)
    for i, s in enumerate(ndimage.find_objects(sl)):
        r = sl[s] == i + 1
        if r.sum() >= 60 and (s[0].stop - s[0].start) >= 12: keep[s] |= r
    # a leak into a light background: the Remote is never wider than ~34 px; trim rows to the run under the bubble
    cx = (px0 + px1) / 2
    for y in range(H):
        xs = np.nonzero(keep[y])[0]
        if len(xs) and xs.max() - xs.min() > 34:
            keep[y] = False; lo = int(max(0, cx - 17)); keep[y, lo:lo + 34] = light[y, lo:lo + 34] & m[y, lo:lo + 34]
    # follow the Remote down row by row: its shaded lower half is darker than the light test. Each row keeps the
    # grey (colourless) run that overlaps the row above, no wider than the Remote.
    if not keep.any(): return keep
    grey = (mx > 55) & (mx - mn < 24)
    ys, xs = np.nonzero(keep); y = ys.min()
    row = keep[y]; out = np.zeros_like(keep); misses = 0
    while y < H - 6:
        prev = np.nonzero(row)[0]
        if len(prev) == 0: break
        lo, hi = prev.min() - 3, prev.max() + 4
        cand = grey[y].copy(); cand[:max(0, lo)] = False; cand[hi:] = False
        cand |= keep[y] & (np.arange(W) >= lo - 6) & (np.arange(W) < hi + 6)
        lab1, n1 = ndimage.label(cand)
        sel = np.zeros(W, bool)
        for k in range(1, n1 + 1):
            r = lab1 == k
            if (r & ndimage.binary_dilation(row, iterations=2)).any(): sel |= r
        xs1 = np.nonzero(sel)[0]
        if len(xs1) and xs1.max() - xs1.min() > 36:          # leaking sideways: keep the part under the row above
            c0 = int(np.mean(prev)); sel[:] = False; sel[max(0, c0 - 18):c0 + 18] = cand[max(0, c0 - 18):c0 + 18]
        if not sel.any():
            misses += 1
            if misses > 2: break
        else: misses = 0; row = sel
        out[y] |= sel; y += 1
    out |= keep
    # its drop shadow: dark, colourless pixels hugging it
    near = ndimage.binary_dilation(out, iterations=5) & ~out
    out |= near & (mx < 95) & (mx - mn < 30)
    return out


# ---------------------------------------------------------------------------------------------- drawing
LIT = (90, 200, 250, 255)


def plastic(size, shape_fn, S=4):
    """A white-plastic shape like the game's Remote render: soft top light, grey edge, thin outline."""
    W, H = size[0] * S, size[1] * S
    mask = Image.new("L", (W, H), 0); shape_fn(ImageDraw.Draw(mask), W, H, 255)
    yy = np.linspace(0, 1, H)[:, None] * np.ones((1, W))
    base = (245 - 55 * yy ** 1.5)
    dist = ndimage.distance_transform_edt(np.array(mask) > 0)
    edge = np.clip(dist / (0.08 * min(W, H)), 0, 1)
    v = base * (0.72 + 0.28 * edge)
    rgb = np.stack([v, v, v * 1.02], -1).clip(0, 255).astype(np.uint8)
    im = Image.fromarray(np.dstack([rgb, np.array(mask)]), "RGBA")
    ol = Image.new("RGBA", (W, H), (0, 0, 0, 0)); shape_fn(ImageDraw.Draw(ol), W, H, None, outline=(110, 110, 118, 255), width=max(2, S))
    im.alpha_composite(ol)
    return im


def draw_mouse(w, h, pressed):
    S = 4
    def body(d, W, H, fill, outline=None, width=1):
        d.rounded_rectangle([W * 0.06, H * 0.02, W * 0.94, H * 0.98], radius=W * 0.44, fill=fill, outline=outline, width=width)
    im = plastic((w, h), body, S); d = ImageDraw.Draw(im); W, H = im.size
    split = H * 0.40
    for side in ("L", "R"):
        if ("A" in pressed and side == "L") or ("B" in pressed and side == "R"):
            m = Image.new("L", (W, H), 0); body(ImageDraw.Draw(m), W, H, 255)
            half = Image.new("L", (W, H), 0); ImageDraw.Draw(half).rectangle([0 if side == "L" else W / 2, 0, W / 2 if side == "L" else W, split], fill=255)
            im.paste(Image.new("RGBA", (W, H), LIT), (0, 0), Image.fromarray(np.minimum(np.array(m), np.array(half))))
    line = (120, 120, 128, 255); lw = max(2, S)
    d.line([W * 0.06, split, W * 0.94, split], fill=line, width=lw); d.line([W / 2, H * 0.02, W / 2, split], fill=line, width=lw)
    d.rounded_rectangle([W * 0.45, H * 0.12, W * 0.55, H * 0.28], radius=W * 0.05, fill=(150, 150, 158, 255))   # wheel
    body(d, W, H, None, outline=(110, 110, 118, 255), width=lw)
    return im.resize((w, h), Image.LANCZOS)


def draw_pad(w, h, style, pressed):
    S = 4
    def body(d, W, H, fill, outline=None, width=1):
        # two grips and a bridge
        d.rounded_rectangle([W * 0.08, H * 0.10, W * 0.92, H * 0.62], radius=H * 0.24, fill=fill, outline=outline, width=width)
        d.ellipse([W * 0.02, H * 0.25, W * 0.36, H * 0.98], fill=fill, outline=outline, width=width)
        d.ellipse([W * 0.64, H * 0.25, W * 0.98, H * 0.98], fill=fill, outline=outline, width=width)
    im = plastic((w, h), body, S)
    # redraw the outline as one shape (the separate parts' outlines cross inside)
    mask = np.array(im)[..., 3] > 0
    edge = mask & ~ndimage.binary_erosion(mask, iterations=S)
    a = np.array(im); inner = mask & ~edge
    yy = np.linspace(0, 1, a.shape[0])[:, None] * np.ones((1, a.shape[1]))
    v = (245 - 55 * yy ** 1.5); a[inner, 0] = v[inner]; a[inner, 1] = v[inner]; a[inner, 2] = np.clip(v[inner] * 1.02, 0, 255)
    a[edge, :3] = (110, 110, 118); im = Image.fromarray(a)
    d = ImageDraw.Draw(im); W, H = im.size; dk = (95, 95, 105, 255); lw = max(2, S)
    ps = style in ("ps4", "ps5")
    # sticks and d-pad (PlayStation: d-pad up left, sticks low; Xbox: left stick up left, d-pad low)
    def stick(cx, cy):
        r = H * 0.11; d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(205, 205, 212, 255), outline=dk, width=lw)
    def dpad(cx, cy):
        k = H * 0.075; lit = "dpad" in pressed
        for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
            d.rectangle([cx + dx * k * 1.1 - k * 0.55, cy + dy * k * 1.1 - k * 0.55, cx + dx * k * 1.1 + k * 0.55, cy + dy * k * 1.1 + k * 0.55], fill=LIT if lit else (180, 180, 188, 255), outline=dk)
        d.rectangle([cx - k * 0.55, cy - k * 0.55, cx + k * 0.55, cy + k * 0.55], fill=(180, 180, 188, 255))
    if ps: dpad(W * 0.24, H * 0.36); stick(W * 0.36, H * 0.66); stick(W * 0.64, H * 0.66)
    else: stick(W * 0.24, H * 0.36); dpad(W * 0.36, H * 0.66); stick(W * 0.64, H * 0.66)
    # face buttons: south = A (Wii A), east = B (Wii B)
    fx, fy, fr = W * 0.76, H * 0.36, H * 0.068
    pos = {"S": (fx, fy + fr * 1.9), "E": (fx + fr * 1.9, fy), "W": (fx - fr * 1.9, fy), "N": (fx, fy - fr * 1.9)}
    lab = {"S": "cross", "E": "circle", "W": "square", "N": "triangle"} if ps else {"S": "A", "E": "B", "W": "X", "N": "Y"}
    for k, (cx, cy) in pos.items():
        lit = (k == "S" and "A" in pressed) or (k == "E" and "B" in pressed)
        d.ellipse([cx - fr, cy - fr, cx + fr, cy + fr], fill=LIT if lit else (190, 190, 198, 255), outline=dk, width=lw)
        ink = (255, 255, 255, 255) if lit else (90, 90, 100, 255); q = fr * 0.5
        t = lab[k]
        if t == "cross": d.line([cx - q, cy - q, cx + q, cy + q], fill=ink, width=lw); d.line([cx - q, cy + q, cx + q, cy - q], fill=ink, width=lw)
        elif t == "circle": d.ellipse([cx - q, cy - q, cx + q, cy + q], outline=ink, width=lw)
        elif t == "square": d.rectangle([cx - q, cy - q, cx + q, cy + q], outline=ink, width=lw)
        elif t == "triangle": d.polygon([(cx, cy - q), (cx - q, cy + q * 0.8), (cx + q, cy + q * 0.8)], outline=ink, width=lw)
        else:
            from make_prompts import font
            f = font(fr * 1.3); bb = d.textbbox((0, 0), t, font=f); d.text((cx - (bb[0] + bb[2]) / 2, cy - (bb[1] + bb[3]) / 2), t, font=f, fill=ink)
    # menu / view (Wii + / -)
    for k, cx in (("minus", W * 0.42), ("plus", W * 0.58)):
        lit = k in pressed; d.rounded_rectangle([cx - W * 0.03, H * 0.33, cx + W * 0.03, H * 0.39], radius=H * 0.02, fill=LIT if lit else (170, 170, 178, 255), outline=dk)
    return im.resize((w, h), Image.LANCZOS)


# ---------------------------------------------------------------------------------------------- one picture
def make(orig_img, up_img, style):
    a = np.array(orig_img.convert("RGB")).astype(int)
    plates = find_plates(a)
    if not plates: return None
    mask = find_remote(a, plates)
    up = np.array(up_img.convert("RGB")); S = up.shape[1] / a.shape[1]
    out = up.copy()
    if mask.any():
        big = cv2.resize(mask.astype(np.uint8) * 255, (up.shape[1], up.shape[0]), interpolation=cv2.INTER_LINEAR) > 40
        big = ndimage.binary_dilation(big, iterations=int(S * 2.5) + 2)
        out = cv2.inpaint(out, big.astype(np.uint8) * 255, int(S * 3), cv2.INPAINT_TELEA)
    im = Image.fromarray(out).convert("RGBA")
    pressed = {p[4] for p in plates}
    # the new device, under the bubble where the Remote's top was
    px0 = min(p[0] for p in plates); px1 = max(p[0] + p[2] for p in plates); pb = max(p[1] + p[3] for p in plates)
    cx = (px0 + px1) / 2
    if mask.any():
        ys, xs = np.nonzero(mask); top = ys.min(); cx = np.median(xs[ys < top + 20])
    else: top = pb + 12
    if style == "keyboard":
        w, h = 26, 40
        dev = draw_mouse(int(w * S), int(h * S), pressed)
    else:
        w, h = 60, 40
        dev = draw_pad(int(w * S), int(h * S), style, pressed)
    x = int((cx - w / 2) * S); x = max(int(6 * S), min(x, im.width - dev.width - int(6 * S)))
    y = int((top + 1) * S); y = min(y, im.height - dev.height - int(6 * S))
    shadow = Image.new("RGBA", dev.size, (0, 0, 0, 0)); shadow.putalpha(dev.getchannel("A").point(lambda v: v * 0.45))
    shadow = shadow.filter(ImageFilter.GaussianBlur(S * 1.5))
    im.alpha_composite(shadow, (x + int(S * 1.5), y + int(S * 2))); im.alpha_composite(dev, (x, y))
    # the plates: new symbols
    for (px, py, pw, ph, kind) in plates:
        key = {"A": "A", "B": "B", "plus": "plus", "minus": "minus"}[kind]
        sym = SYMBOL[style].get(key)
        if not sym: continue
        plate = tuple(int(v) for v in np.median(a[py + 1:py + ph - 1, px + 1:px + pw - 1][(a[py + 1:py + ph - 1, px + 1:px + pw - 1].max(2) > 150)], axis=0)) + (255,)
        X0, Y0, Wp, Hp = round((px - 0.5) * S), round((py - 0.5) * S), round((pw + 1) * S), round((ph + 1) * S)
        pl = Image.new("RGBA", (Wp, Hp), (0, 0, 0, 0)); ImageDraw.Draw(pl).rounded_rectangle([0, 0, Wp - 1, Hp - 1], radius=min(Wp, Hp) * 0.2, fill=plate)
        im.alpha_composite(pl, (X0, Y0))
        g, _ = draw_symbol(sym[0], sym[1], round((pw - 3) * S), round((ph - 3) * S), (24, 24, 24, 255))
        im.alpha_composite(g, (round((px + 1.5) * S), round((py + 1.5) * S)))
    return im.convert("RGB")


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("orig"); ap.add_argument("up"); ap.add_argument("out"); ap.add_argument("--only")
    ap.add_argument("--report")
    a = ap.parse_args()
    only = set(a.only.split(",")) if a.only else None
    done = 0; rep = {}
    for f in sorted(glob.glob(os.path.join(a.orig, "**", "tt_*.png"), recursive=True)):
        rel = os.path.relpath(f, a.orig); name = os.path.basename(f)
        if only and not any(o in name for o in only): continue
        upf = os.path.join(a.up, rel)
        if not os.path.exists(upf): continue
        o = Image.open(f); u = Image.open(upf)
        for st in STYLES:
            r = make(o, u, st)
            if r is None: break
            os.makedirs(os.path.join(a.out, st), exist_ok=True)
            r.save(os.path.join(a.out, st, name[:-4] + ".jpg"), quality=90)
        else: done += 1
    print(f"{done} tutorial pictures x {len(STYLES)} styles -> {a.out}")


if __name__ == "__main__":
    main()
