"""Tutorial pictures v2: per-page specs (tutorial_specs.json), checked by eye.

A page's spec:
  "erase":   [[x0, y0, x1, y1], ...]   boxes (original 304x232 pixels) holding a Wii Remote / Nunchuk and its cable;
                                       the light / grey pixels in them are painted out of the upscaled picture
  "plates":  [[x, y, w, h, key], ...]  button plates already in the picture (the help bubbles and the game's own little
                                       on-screen prompts) to redraw with the controller's symbol
  "cards":   [[x, y, [keys...], off], ...]  a new dark card at (x, y) (its top-left) showing these inputs as plates;
                                       off = true adds "OFF" (let go). Keys: A B Z C plus minus stick pointer
Pages with no spec are left alone.
"""
import os, sys, json, argparse
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from scipy import ndimage
import cv2
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_prompts import draw_symbol, SYMBOL, font

STYLES = ("keyboard", "xbox", "ps4", "ps5")
POINTER = {"keyboard": ("mousemove", None), "xbox": ("stick", "R"), "ps4": ("stick", "R"), "ps5": ("stick", "R")}


def symbol(style, key, w, h, ink):
    if key == "pointer": kind, arg = POINTER[style]
    else:
        sym = SYMBOL[style].get(key)
        if not sym: return None
        kind, arg = sym
    if kind == "mousemove":          # a mouse with four arrows: move the mouse
        S = 4; W, H = w * S, h * S; im = Image.new("RGBA", (W, H), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
        mw, mh = W * 0.42, H * 0.5; x0, y0 = (W - mw) / 2, (H - mh) / 2
        d.rounded_rectangle([x0, y0, x0 + mw, y0 + mh], radius=mw * 0.48, outline=ink, width=max(2, int(W * 0.06)))
        d.line([W / 2, y0 + 2, W / 2, y0 + mh * 0.38], fill=ink, width=max(2, int(W * 0.05)))
        a = W * 0.11
        for tip, b1, b2 in (((W / 2, H * 0.02), (W / 2 - a, H * 0.02 + a), (W / 2 + a, H * 0.02 + a)),
                            ((W / 2, H * 0.98), (W / 2 - a, H * 0.98 - a), (W / 2 + a, H * 0.98 - a)),
                            ((W * 0.02, H / 2), (W * 0.02 + a, H / 2 - a), (W * 0.02 + a, H / 2 + a)),
                            ((W * 0.98, H / 2), (W * 0.98 - a, H / 2 - a), (W * 0.98 - a, H / 2 + a))):
            d.polygon([tip, b1, b2], fill=ink)
        return im.resize((w, h), Image.LANCZOS)
    g, _ = draw_symbol(kind, arg, w, h, ink)
    return g


def plate_img(style, key, w, h, S, colour=(214, 214, 220, 255)):
    W, H = round(w * S), round(h * S)
    pl = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(pl).rounded_rectangle([0, 0, W - 1, H - 1], radius=min(W, H) * 0.2, fill=colour)
    g = symbol(style, key, max(1, round((w - 3) * S)), max(1, round((h - 3) * S)), (24, 24, 24, 255))
    if g: pl.alpha_composite(g, (round(1.5 * S), round(1.5 * S)))
    return pl


def card(style, keys, off, S):
    """A dark rounded card like the game's bubbles: plates joined by "+", "OFF" under them if off."""
    pw, ph, gap = 15, 19, 9
    n = len(keys); w = 8 + n * pw + (n - 1) * gap; h = 8 + ph + (12 if off else 0)
    W, H = round(w * S), round(h * S)
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, W - 1, H - 1], radius=min(W, H) * 0.3, fill=(12, 12, 14, 235))
    x = 4
    for i, k in enumerate(keys):
        im.alpha_composite(plate_img(style, k, pw, ph, S), (round(x * S), round(4 * S)))
        x += pw
        if i < n - 1:
            f = font(10 * S); bb = d.textbbox((0, 0), "+", font=f)
            d.text((round((x + gap / 2) * S) - (bb[0] + bb[2]) / 2, round((4 + ph / 2) * S) - (bb[1] + bb[3]) / 2), "+", font=f, fill=(235, 235, 235, 255))
            x += gap
    if off:
        f = font(9 * S); bb = d.textbbox((0, 0), "OFF", font=f)
        d.text((W / 2 - (bb[0] + bb[2]) / 2, round((4 + ph + 6) * S) - (bb[1] + bb[3]) / 2), "OFF", font=f, fill=(235, 235, 235, 255))
    return im


def card_box(style, keys, off, w, h, S):
    """A dark card of w x h (original pixels) with the inputs as big plates in the middle, "OFF" under them."""
    W, H = round(w * S), round(h * S)
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, W - 1, H - 1], radius=min(W, H) * 0.18, outline=(255, 255, 255, 70), width=max(1, round(S * 0.6)))
    n = len(keys); gap = 0.45
    room_h = h - 8 - (11 if off else 0) - (10 if len(keys) == 1 and keys[0] in ('pointer', 'stick') else 0)
    ph = min(room_h, (w - 8) / (n * 0.8 + (n - 1) * gap * 0.8)); ph = max(12, min(ph, 40)); pw = ph * 0.8
    tot = n * pw + (n - 1) * pw * gap; x = (w - tot) / 2; y = 4 + (room_h - ph) / 2
    for i, k in enumerate(keys):
        im.alpha_composite(plate_img(style, k, pw, ph, S), (round(x * S), round(y * S)))
        x += pw
        if i < n - 1:
            f = font(pw * 0.6 * S); bb = d.textbbox((0, 0), "+", font=f)
            d.text((round((x + pw * gap / 2) * S) - (bb[0] + bb[2]) / 2, round((y + ph / 2) * S) - (bb[1] + bb[3]) / 2), "+", font=f, fill=(235, 235, 235, 255))
            x += pw * gap
    if off:
        f = font(9 * S); bb = d.textbbox((0, 0), "OFF", font=f)
        d.text((W / 2 - (bb[0] + bb[2]) / 2, round((y + ph + 7) * S) - (bb[1] + bb[3]) / 2), "OFF", font=f, fill=(235, 235, 235, 255))
    if n == 1 and keys[0] in ("pointer", "stick"):     # move it: arrows on four sides of the plate
        cx, cy = W / 2, (y + ph / 2) * S; rx, ry = (pw / 2 + 3) * S, (ph / 2 + 3) * S; t = 2.6 * S
        for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
            bx, by = cx + dx * rx, cy + dy * ry; tip = (bx + dx * t * 1.4, by + dy * t * 1.4)
            d.polygon([tip, (bx - dy * t, by + dx * t), (bx + dy * t, by - dx * t)], fill=(90, 200, 250, 255))
    return im


def erase_mask(a, boxes):
    mx = a.max(2); mn = a.min(2); H, W = mx.shape
    m = np.zeros((H, W), bool)
    for x0, y0, x1, y1 in boxes:
        sub = (mx[y0:y1, x0:x1] > 55) & (mx[y0:y1, x0:x1] - mn[y0:y1, x0:x1] < 30)
        # keep the part connected to the box's biggest light piece (the device) plus its shading
        lab, n = ndimage.label(sub)
        if n:
            sizes = ndimage.sum(sub, lab, range(1, n + 1)); keep = np.isin(lab, [i + 1 for i, s in enumerate(sizes) if s >= max(20, sizes.max() * 0.15)])
            keep = ndimage.binary_fill_holes(ndimage.binary_closing(keep, iterations=2))
            m[y0:y1, x0:x1] |= keep
    near = ndimage.binary_dilation(m, iterations=3) & ~m
    m |= near & (mx < 95) & (mx - mn < 30)             # its drop shadow
    return m


def fill(img, mask, S):
    """Paint out the masked parts with picture texture from beside them (Poisson-blended), not a smear."""
    H, W = mask.shape
    lab, n = ndimage.label(ndimage.binary_dilation(mask, iterations=int(S)))
    for i, sl in enumerate(ndimage.find_objects(lab)):
        m = (lab == i + 1) & mask
        ys, xs = np.nonzero(m); y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
        bw = x1 - x0; best = None
        for dx in (-(bw + int(4 * S)), bw + int(4 * S)):           # the strip just left or right of it
            sx0 = x0 + dx
            if sx0 < 0 or sx0 + bw > W: continue
            if (mask[y0:y1, sx0:sx0 + bw]).mean() > 0.05: continue
            best = dx; break
        if best is None:
            img = cv2.inpaint(img, (m * 255).astype(np.uint8), int(S * 4), cv2.INPAINT_TELEA); continue
        src = img.copy(); src[y0:y1, x0:x1] = img[y0:y1, x0 + best:x1 + best][:, ::-1]   # mirrored, so its edge meets ours
        mm = np.zeros((H, W), np.uint8); mm[m] = 255
        mm = cv2.dilate(mm, np.ones((5, 5), np.uint8))
        ys2, xs2 = np.nonzero(mm); cy, cx = (ys2.min() + ys2.max() + 1) // 2, (xs2.min() + xs2.max() + 1) // 2
        try:
            img = cv2.seamlessClone(src, img, mm, (int(cx), int(cy)), cv2.NORMAL_CLONE)
        except cv2.error:
            img = cv2.inpaint(img, mm, int(S * 4), cv2.INPAINT_TELEA)
    return img


def render(orig, up, style, spec):
    a = np.array(orig.convert("RGB")).astype(int)
    out = np.array(up.convert("RGB")); S = out.shape[1] / a.shape[1]
    if spec.get("erase"):
        m = erase_mask(a, spec["erase"])
        big = cv2.resize(m.astype(np.uint8) * 255, (out.shape[1], out.shape[0]), interpolation=cv2.INTER_LINEAR) > 40
        big = ndimage.binary_dilation(big, iterations=int(S * 1.5) + 1)
        out = cv2.inpaint(out, big.astype(np.uint8) * 255, int(S * 3), cv2.INPAINT_TELEA)
    im = Image.fromarray(out).convert("RGBA")
    for px, py, pw, ph, key in spec.get("plates", []):
        rgb = a[py + 1:py + ph - 1, px + 1:px + pw - 1].reshape(-1, 3); lit = rgb[rgb.max(1) > 95]
        col = tuple(int(v) for v in (np.median(lit, 0) if len(lit) else (214, 214, 220))) + (255,)
        im.alpha_composite(plate_img(style, key, pw + 1, ph + 1, S, col), (round((px - 0.5) * S), round((py - 0.5) * S)))
    for x0, y0, x1, y1, keys, off in spec.get("cards", []):
        c = card_box(style, keys, off, x1 - x0, y1 - y0, S)
        sh = Image.new("RGBA", c.size, (0, 0, 0, 0)); sh.putalpha(c.getchannel("A").point(lambda v: v * 0.5)); sh = sh.filter(ImageFilter.GaussianBlur(S))
        X, Y = round(x0 * S), round(y0 * S)
        # frosted glass: the picture behind, blurred and darkened (hides what was painted out)
        reg = im.crop((X, Y, X + c.width, Y + c.height)).convert("RGB").filter(ImageFilter.GaussianBlur(S * 3))
        reg = Image.fromarray((np.array(reg).astype(float) * 0.33).astype(np.uint8)).convert("RGBA")
        rm = Image.new("L", c.size, 0); ImageDraw.Draw(rm).rounded_rectangle([0, 0, c.width - 1, c.height - 1], radius=min(c.size) * 0.18, fill=255)
        im.alpha_composite(sh, (X + round(S), Y + round(S)))
        im.paste(reg, (X, Y), rm); im.alpha_composite(c, (X, Y))
    return im.convert("RGB")


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("orig"); ap.add_argument("up"); ap.add_argument("out")
    ap.add_argument("--specs", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "tutorial_specs.json"))
    ap.add_argument("--only"); ap.add_argument("--styles", default=",".join(STYLES))
    a = ap.parse_args(); specs = json.load(open(a.specs)); only = set(a.only.split(",")) if a.only else None
    n = 0
    for root, _, files in os.walk(a.orig):
        for f in files:
            if not f.startswith("tt_"): continue
            h = f[3:19]
            if h not in specs or (only and h not in only): continue
            rel = os.path.relpath(os.path.join(root, f), a.orig)
            upf = os.path.join(a.up, rel)
            if not os.path.exists(upf): print("no upscaled picture:", rel); continue
            o = Image.open(os.path.join(root, f)); u = Image.open(upf)
            for st in a.styles.split(","):
                os.makedirs(os.path.join(a.out, st), exist_ok=True)
                render(o, u, st, specs[h]).save(os.path.join(a.out, st, f[:-4] + ".jpg"), quality=90)
            n += 1
    print(n, "pages")


if __name__ == "__main__":
    main()
