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

# Pages the automatic search gets wrong (a Remote at an angle, on a light background, above its bubble, or a
# Nunchuk): the device boxes by hand, in original pixels: (x0, y0, x1, y1, cable x or None, "remote" / "nunchuk").
# A Nunchuk's bubble plate is a Z.
FIX = {   # keyed by texture id (several pages share a name)
    "04480567FBE05EFA": [(218, 50, 268, 100, None, "remote")],                  # em scissors_01
    "0CBD1AB53138C709": [(228, 110, 282, 176, 269, "remote")],                  # au clothes02
    "166E6FD5D8B77F55": [(32, 100, 64, 162, 47, "nunchuk"), (236, 98, 286, 157, 262, "remote")],   # su countershock_04
    "A9C0F54DDDE3D008": [(32, 100, 64, 162, 47, "nunchuk"), (236, 98, 286, 157, 262, "remote")],   # em countershock_04
    "50EC0FF04558A8FE": [(238, 104, 274, 222, None, "remote")],                 # or bone3_02
    "66C9F197D7CDB05C": [(222, 110, 280, 174, 264, "remote")],                  # em forceps_01
    "7B2A2199EABA9361": [(222, 110, 280, 174, 264, "remote")],                  # em forceps_02
    "8D87F73484814D65": [(34, 115, 90, 177, 45, "remote")],                     # or bone1_01
    "9051BAEEB70E458D": [(148, 110, 204, 174, 164, "remote")],                  # au pcanalyze02
    "A986B8DFC57CD439": [(95, 115, 150, 177, 110, "remote")],                   # em removetray_01
    "BED4FB7477433BE9": [(95, 115, 150, 180, 106, "remote")],                   # em removetray_02
    "C7CC7E164EE029C8": [(226, 50, 266, 102, None, "remote")],                  # em scissors_02
    "CCA800CF57B30C41": [(225, 110, 276, 170, 262, "remote")],                  # em press_bandage1_03
    "D4CE597DE55E1337": [(232, 48, 265, 97, None, "remote")],                   # em scissors_03
    "F245E3624D72B241": [(228, 113, 279, 167, 265, "remote")],                  # or stapler_02
}
# plates the search misses (the Nunchuk's Z): (x, y, w, h, kind)
FIX_PLATES = {
    "9051BAEEB70E458D": [(157, 72, 19, 25, "A"), (190, 72, 19, 25, "B")],
    "04480567FBE05EFA": [(235, 122, 11, 15, "A"), (257, 122, 11, 15, "B")],
    "C7CC7E164EE029C8": [(236, 122, 11, 15, "A"), (258, 122, 10, 15, "B")],
    "D4CE597DE55E1337": [(236, 122, 11, 15, "A"), (258, 122, 10, 15, "B")],
    "166E6FD5D8B77F55": [(38, 49, 16, 19, "Z"), (255, 47, 14, 19, "B")],
    "A9C0F54DDDE3D008": [(39, 49, 15, 19, "Z"), (256, 47, 14, 19, "B")],
}
def fix_for(name):
    return FIX.get(name[3:19])


# ---------------------------------------------------------------------------------------------- finding things
def find_plates(a, small=False):
    """Grey button plates (A, B, +, -) inside dark speech bubbles: [(x, y, w, h, kind)] in original pixels.
    small: the game's own little on-screen prompts captured in the screenshot (plates 7-13 px tall)."""
    mx = a.max(2); mn = a.min(2)
    g = (mx > (95 if small else 150)) & (mx - mn < 28); dark = mx < (40 if small else 50)
    lab, _ = ndimage.label(g); out = []
    for i, s in enumerate(ndimage.find_objects(lab)):
        ph = s[0].stop - s[0].start; pw = s[1].stop - s[1].start
        if small:
            if not (7 <= ph <= 13 and 5 <= pw <= 12 and ph > pw * 1.05): continue
        elif not (14 <= ph <= 30 and 10 <= pw <= 26 and ph > pw * 1.05): continue
        reg = ndimage.binary_fill_holes(lab[s] == i + 1)
        if reg.mean() < 0.8: continue
        inner = dark[s] & reg
        if inner.sum() < (3 if small else 6): continue
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
    arrows = []
    def stick(cx, cy, lit=False):
        r = H * 0.11; d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=LIT if lit else (205, 205, 212, 255), outline=dk, width=lw)
        if lit: arrows.append((cx, cy, r))
    def dpad(cx, cy):
        k = H * 0.075; lit = "dpad" in pressed
        for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
            d.rectangle([cx + dx * k * 1.1 - k * 0.55, cy + dy * k * 1.1 - k * 0.55, cx + dx * k * 1.1 + k * 0.55, cy + dy * k * 1.1 + k * 0.55], fill=LIT if lit else (180, 180, 188, 255), outline=dk)
        d.rectangle([cx - k * 0.55, cy - k * 0.55, cx + k * 0.55, cy + k * 0.55], fill=(180, 180, 188, 255))
    ls, rs = "stick" in pressed, "pointer" in pressed
    if ps: dpad(W * 0.24, H * 0.36); stick(W * 0.36, H * 0.66, ls); stick(W * 0.64, H * 0.66, rs)
    else: stick(W * 0.24, H * 0.36, ls); dpad(W * 0.36, H * 0.66); stick(W * 0.64, H * 0.66, rs)
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
    # a moving stick: four arrows around it, drawn past the pad's outline so they read as "move this"
    for cx, cy, r in arrows:
        a0, a1 = r * 1.35, r * 2.05
        for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
            tip = (cx + dx * a1, cy + dy * a1); px, py = -dy, dx
            base = (cx + dx * a0, cy + dy * a0)
            d.polygon([tip, (base[0] + px * r * 0.45, base[1] + py * r * 0.45), (base[0] - px * r * 0.45, base[1] - py * r * 0.45)],
                      fill=LIT, outline=(30, 90, 140, 255))
    # left trigger (Wii Z), drawn on the top edge
    d.rounded_rectangle([W * 0.12, H * 0.02, W * 0.30, H * 0.12], radius=H * 0.04, fill=LIT if "Z" in pressed else (200, 200, 208, 255), outline=dk, width=lw)
    # menu / view (Wii + / -)
    for k, cx in (("minus", W * 0.42), ("plus", W * 0.58)):
        lit = k in pressed; d.rounded_rectangle([cx - W * 0.03, H * 0.33, cx + W * 0.03, H * 0.39], radius=H * 0.02, fill=LIT if lit else (170, 170, 178, 255), outline=dk)
    return im.resize((w, h), Image.LANCZOS)


def draw_key(w, h, label):
    S = 4
    def body(d, W, H, fill, outline=None, width=1):
        d.rounded_rectangle([W * 0.04, H * 0.04, W * 0.96, H * 0.96], radius=W * 0.18, fill=fill, outline=outline, width=width)
    im = plastic((w, h), body, S); d = ImageDraw.Draw(im); W, H = im.size
    from make_prompts import font
    f = font(H * 0.5); bb = d.textbbox((0, 0), label, font=f)
    d.text((W / 2 - (bb[0] + bb[2]) / 2, H / 2 - (bb[1] + bb[3]) / 2), label, font=f, fill=LIT)
    return im.resize((w, h), Image.LANCZOS)


def draw_wasd(w, h):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0)); k = int(min(w / 3.2, h / 2.15)); g = max(1, k // 10)
    for lab, cx, cy in (("W", w / 2, k / 2), ("A", w / 2 - k - g, k * 1.5 + g), ("S", w / 2, k * 1.5 + g), ("D", w / 2 + k + g, k * 1.5 + g)):
        key = draw_key(k, k, lab); im.alpha_composite(key, (int(cx - k / 2), int(cy - k / 2)))
    return im


def device_image(style, pressed, S, kind="remote"):
    if style == "keyboard":
        if kind == "nunchuk" and "Z" not in pressed and "C" not in pressed: w, h = 58, 40; return draw_wasd(int(w * S), int(h * S)), w, h
        if kind == "nunchuk" or "Z" in pressed: w, h = 26, 26; return draw_key(int(w * S), int(h * S), "E"), w, h
        if "pointer" in pressed:      # moving the mouse: arrows on four sides
            w, h = 40, 46; W, H = int(w * S), int(h * S); im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
            mw, mh = int(24 * S), int(36 * S); im.alpha_composite(draw_mouse(mw, mh, set()), ((W - mw) // 2, (H - mh) // 2))
            d = ImageDraw.Draw(im); r = 3.2 * S; cx, cy = W / 2, H / 2
            for dx, dy, ext in ((0, -1, H / 2), (0, 1, H / 2), (-1, 0, W / 2), (1, 0, W / 2)):
                tip = (cx + dx * (ext - 1), cy + dy * (ext - 1)); bx, by = cx + dx * (ext - 1 - r * 1.6), cy + dy * (ext - 1 - r * 1.6)
                d.polygon([tip, (bx - dy * r, by + dx * r), (bx + dy * r, by - dx * r)], fill=LIT, outline=(30, 90, 140, 255))
            return im, w, h
        w, h = 26, 40; return draw_mouse(int(w * S), int(h * S), pressed), w, h
    w, h = 60, 40; return draw_pad(int(w * S), int(h * S), style, pressed), w, h


def paste_device(im, dev, x, y, S):
    shadow = Image.new("RGBA", dev.size, (0, 0, 0, 0)); shadow.putalpha(dev.getchannel("A").point(lambda v: v * 0.45))
    shadow = shadow.filter(ImageFilter.GaussianBlur(S * 1.5))
    im.alpha_composite(shadow, (x + int(S * 1.5), y + int(S * 2))); im.alpha_composite(dev, (x, y))


# ---------------------------------------------------------------------------------------------- one picture
def make(orig_img, up_img, style, name=""):
    a = np.array(orig_img.convert("RGB")).astype(int)
    plates = find_plates(a)
    fix = fix_for(name)
    if not plates and not fix: return make_generic(a, up_img, style, name)
    if fix:
        if name[3:19] in FIX_PLATES: plates = FIX_PLATES[name[3:19]]
        return make_fixed(a, up_img, style, plates, fix)
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
    dev, w, h = device_image(style, pressed, S)
    x = int((cx - w / 2) * S); x = max(int(6 * S), min(x, im.width - dev.width - int(6 * S)))
    y = int((top + 1) * S); y = min(y, im.height - dev.height - int(6 * S))
    paste_device(im, dev, x, y, S)
    redraw_plates(im, a, plates, style, S)
    return im.convert("RGB")


def make_fixed(a, up_img, style, plates, fix):
    """A page from the FIX table: paint out the given boxes (rounded) and cables, draw each device there."""
    up = np.array(up_img.convert("RGB")); S = up.shape[1] / a.shape[1]; H, W = a.shape[:2]
    mx = a.max(2); mn = a.min(2)
    small = np.zeros((H, W), bool)
    for (x0, y0, x1, y1, cab, kind) in fix:
        small[y0:y1, x0:x1] = True
        # the rest of the Remote below the box and its cable: light, colourless pixels down to the frame
        reg = np.zeros((H, W), bool); reg[y1 - 2:H - 7, max(0, x0 - 6):min(W, x1 + 6)] = True
        light = reg & (mx > 120) & (mx - mn < 34)
        lab, _ = ndimage.label(light | small)
        ids = set(lab[small].ravel()) - {0}
        small |= np.isin(lab, list(ids)) & reg
        if cab is not None: small[y1:H - 8, cab - 2:cab + 3] = True
    small = ndimage.binary_dilation(small, iterations=1)
    m = (cv2.resize(small.astype(np.uint8) * 255, (up.shape[1], up.shape[0]), interpolation=cv2.INTER_LINEAR) > 40)
    m = (ndimage.binary_dilation(m, iterations=int(S * 1.5)) * 255).astype(np.uint8)
    out = cv2.inpaint(up, m, int(S * 3), cv2.INPAINT_TELEA)
    im = Image.fromarray(out).convert("RGBA")
    # Z plates belong to a Nunchuk: the plate nearest above each Nunchuk box
    pl = [list(p) for p in plates]
    for (x0, y0, x1, y1, cab, kind) in fix:
        near = [p for p in pl if abs(p[0] + p[2] / 2 - (x0 + x1) / 2) < 60]
        pressed = {p[4] for p in near} or {"A"}
        if kind == "nunchuk": pressed = {"Z"}
        dev, w, h = device_image(style, pressed, S, kind)
        cx = (x0 + x1) / 2
        x = int((cx - w / 2) * S); x = max(int(6 * S), min(x, im.width - dev.width - int(6 * S)))
        y = int(y0 * S); y = min(y, im.height - dev.height - int(6 * S))
        paste_device(im, dev, x, y, S)
    redraw_plates(im, a, [tuple(p) for p in pl], style, S)
    return im.convert("RGB")


# ---------------------------------------------------------------------------------------------- pages without a found plate
def find_devices(a):
    """Remotes / Nunchuks standing in the picture (a light body whose cable or body reaches the bottom):
    [(mask, top, centre x, kind)]; kind "remote" or "nunchuk" (a Nunchuk narrows towards its cable)."""
    mx = a.max(2); mn = a.min(2); H, W = mx.shape
    light = (mx > 150) & (mx - mn < 28)
    light[:6] = light[-6:] = False; light[:, :6] = light[:, -6:] = False
    body = ndimage.binary_opening(light, iterations=2)
    lab, n = ndimage.label(body); out = []
    grey = (mx > 55) & (mx - mn < 24)
    for i, s in enumerate(ndimage.find_objects(lab)):
        h = s[0].stop - s[0].start; w = s[1].stop - s[1].start; r = lab[s] == i + 1
        if h < 22 or w < 9 or w > 48 or r.sum() < 160 or r.mean() < 0.45: continue
        if h > H * 0.75 or (w <= 14 and h > 120): continue            # a divider between two pictures
        if s[0].start < 45: continue                                   # light picture parts reaching from the top
        x0, x1 = max(0, s[1].start - 4), min(W, s[1].stop + 4)
        col = light[s[0].stop:H - 6, x0:x1].any(1)
        if not (s[0].stop >= H - 14 or (len(col) > 0 and col.mean() > 0.85)): continue
        m = np.zeros((H, W), bool); m[s] |= r
        reg = np.zeros((H, W), bool); reg[s[0].start:H - 6, x0:x1] = True
        l2, _ = ndimage.label(((light | grey) & reg) | m); ids = set(l2[m].ravel()) - {0}
        m |= np.isin(l2, list(ids)) & reg
        near = ndimage.binary_dilation(m, iterations=4) & ~m
        m |= near & (mx < 95) & (mx - mn < 30)
        # shape: widths along the body
        rows = [r[y].sum() for y in range(h)]
        wmax = max(rows); low = np.mean(rows[int(h * 0.75):]) if h > 8 else wmax
        kind = "remote"
        # the stick's blue direction arrows drawn around a Nunchuk
        ring = ndimage.binary_dilation(m, iterations=10) & ~m
        blue = (a[..., 2] > 200) & (a[..., 0] < 110) & (a[..., 2] - a[..., 0] > 110)
        if (ring & blue).sum() > 60: kind = "nunchuk"
        out.append((m, s[0].start, (s[1].start + s[1].stop) / 2, kind))
    return out


def bubble_above(a, top, cx):
    """The speech bubble over a device (if any): its plates [(x, y, w, h, kind)] and whether it says OFF."""
    mx = a.max(2); mn = a.min(2); H, W = mx.shape
    y0 = max(0, top - 60); x0 = max(0, int(cx) - 45); x1 = min(W, int(cx) + 45)
    dark = np.zeros((H, W), bool); dark[y0:top + 2, x0:x1] = mx[y0:top + 2, x0:x1] < 45
    lab, n = ndimage.label(ndimage.binary_closing(dark, iterations=1))
    best = None
    for i, s in enumerate(ndimage.find_objects(lab)):
        r = lab[s] == i + 1
        if r.sum() < 120: continue
        if best is None or r.sum() > best[1]: best = (s, r.sum(), i + 1)
    if not best: return [], False
    s, _, li = best
    inside = ndimage.binary_fill_holes(lab[s] == li) & ~(lab[s] == li)
    sub = a[s]; smx = sub.max(2); smn = sub.min(2)
    g = inside & (smx > 140) & (smx - smn < 30)
    # plate pieces (the symbol can cut a plate in two): merge pieces whose columns overlap
    gl, gn = ndimage.label(g); boxes = []
    for t in ndimage.find_objects(gl):
        if (t[0].stop - t[0].start) * (t[1].stop - t[1].start) < 12: continue
        boxes.append([t[0].start, t[0].stop, t[1].start, t[1].stop])
    merged = True
    while merged:
        merged = False
        for i in range(len(boxes)):
            for j in range(i + 1, len(boxes)):
                A, B = boxes[i], boxes[j]
                ov = min(A[3], B[3]) - max(A[2], B[2])
                if ov > 0.5 * min(A[3] - A[2], B[3] - B[2]):
                    boxes[i] = [min(A[0], B[0]), max(A[1], B[1]), min(A[2], B[2]), max(A[3], B[3])]; del boxes[j]; merged = True; break
            if merged: break
    plates = []
    for y0_, y1_, x0_, x1_ in boxes:
        ph = y1_ - y0_; pw = x1_ - x0_
        if 11 <= ph <= 30 and 8 <= pw <= 26 and ph >= pw * 0.8:
            inner = (smx[y0_:y1_, x0_:x1_] < 60); inner[:2] = False; inner[-2:] = False; inner[:, :2] = False; inner[:, -2:] = False
            kind = classify2(inner)
            plates.append((s[1].start + x0_, s[0].start + y0_, pw, ph, kind))
    off = not plates and (inside & (smx > 200)).sum() > 15 and inside.sum() < 400
    return plates, off


def classify2(inner):
    ys, xs = np.nonzero(inner)
    if len(ys) == 0: return "A"
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    box = inner[y0:y1, x0:x1]
    top = box[:max(1, (y1 - y0) // 6)].mean(0); bot = box[-max(1, (y1 - y0) // 6):].mean(0)
    if box.mean() < 0.5 and (top > 0.5).mean() > 0.6 and (bot > 0.5).mean() > 0.6: return "Z"
    if box.mean() < 0.25: return "plus"
    bh, bw = box.shape; c = max(1, bw // 5)
    corners = np.mean([box[:c, :c].mean(), box[:c, -c:].mean(), box[-c:, :c].mean(), box[-c:, -c:].mean()])
    return "B" if corners > 0.6 else "A"


NOT_NUNCHUK = {"8D7532A344FC1530", "BDD33F7F4C75D745", "B14D5EA4E9D6FBC1"}   # blue things near a Remote


def make_generic(a, up_img, style, name=""):
    devs = find_devices(a)
    if name[3:19] in NOT_NUNCHUK: devs = [(m, t, c, "remote") for m, t, c, k in devs]
    if not devs: return None
    up = np.array(up_img.convert("RGB")); S = up.shape[1] / a.shape[1]
    allm = np.zeros(a.shape[:2], bool)
    for m, *_ in devs: allm |= m
    big = cv2.resize(allm.astype(np.uint8) * 255, (up.shape[1], up.shape[0]), interpolation=cv2.INTER_LINEAR) > 40
    big = ndimage.binary_dilation(big, iterations=int(S * 2.5) + 2)
    out = cv2.inpaint(up, big.astype(np.uint8) * 255, int(S * 3), cv2.INPAINT_TELEA)
    im = Image.fromarray(out).convert("RGBA")
    for m, top, cx, kind in devs:
        plates, off = bubble_above(a, top, cx)
        pressed = {p[4] for p in plates}
        if "Z" in pressed or "C" in pressed: kind = "nunchuk"
        elif pressed & {"A", "B"}: kind = "remote"
        if off: pressed = set()
        elif not pressed: pressed = {"stick"} if kind == "nunchuk" else {"pointer"}
        dev, w, h = device_image(style, pressed, S, kind)
        x = int((cx - w / 2) * S); x = max(int(6 * S), min(x, im.width - dev.width - int(6 * S)))
        y = int((top + 1) * S); y = min(y, im.height - dev.height - int(6 * S))
        paste_device(im, dev, x, y, S)
        redraw_plates(im, a, plates, style, S)
    return im.convert("RGB")


def redraw_plates(im, a, plates, style, S):
    for (px, py, pw, ph, kind) in plates:
        key = {"A": "A", "B": "B", "plus": "plus", "minus": "minus", "Z": "Z", "C": "C"}[kind]
        sym = SYMBOL[style].get(key)
        if not sym: continue
        plate = tuple(int(v) for v in np.median(a[py + 1:py + ph - 1, px + 1:px + pw - 1][(a[py + 1:py + ph - 1, px + 1:px + pw - 1].max(2) > 150)], axis=0)) + (255,)
        X0, Y0, Wp, Hp = round((px - 0.5) * S), round((py - 0.5) * S), round((pw + 1) * S), round((ph + 1) * S)
        pl = Image.new("RGBA", (Wp, Hp), (0, 0, 0, 0)); ImageDraw.Draw(pl).rounded_rectangle([0, 0, Wp - 1, Hp - 1], radius=min(Wp, Hp) * 0.2, fill=plate)
        im.alpha_composite(pl, (X0, Y0))
        g, _ = draw_symbol(sym[0], sym[1], round((pw - 3) * S), round((ph - 3) * S), (24, 24, 24, 255))
        im.alpha_composite(g, (round((px + 1.5) * S), round((py + 1.5) * S)))


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
            r = make(o, u, st, name[:-4])
            if r is None: break
            os.makedirs(os.path.join(a.out, st), exist_ok=True)
            r.save(os.path.join(a.out, st, name[:-4] + ".jpg"), quality=90)
        else: done += 1
    print(f"{done} tutorial pictures x {len(STYLES)} styles -> {a.out}")


if __name__ == "__main__":
    main()
