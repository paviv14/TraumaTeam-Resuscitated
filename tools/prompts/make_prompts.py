"""Button prompts for keyboard & mouse and controllers, made from the game's own Wii Remote help pictures.

The game's help bars, speech-bubble prompts and "OK / BACK" keys show Wii Remote buttons on grey plates
("Ⓐ CHECK  Ⓑ BACK"). This keeps each picture (its text, plates and bubbles) and redraws only the symbol on each
plate - a mouse button, a key, or a controller button - in the same colours, so the result looks like the game's
own art. One set per controller style is written to <out>/<set>/ with the game's texture names; the runtime
(texpack.c) shows the set that matches the device used last (settings: Button prompts / Controller style).

    python make_prompts.py <extracted 2D textures> <out> [--upscaled <texture pack>]

<extracted 2D textures>: the game's 2D art (the "Trauma Team textures" folder: sprite\\..\\xxx.tpx\\tt_<id>_<name>.png).
--upscaled: a texture pack with the same names; its (larger) pictures are used as the base where present, so the
prompts match the pack. Without it the originals are enlarged 4x (pixel-art scaler).
"""
import os, sys, argparse
import numpy as np
from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------------------------------------- where the plates are
# (picture name, [(button, x, y, w, h, kind)])  - coordinates in the original picture; kind: "plate" = a grey plate
# whose inner symbol is redrawn, "bare" = an icon with no plate (the Nunchuk stick), redrawn whole.
PLATES = {
    "help_icon_208x64": [("dpad_lr", 3, 2, 17, 26, "plate"), ("dpad_ud", 25, 2, 17, 26, "plate"), ("A", 47, 2, 17, 26, "plate"),
                         ("minus", 3, 32, 17, 26, "plate"), ("plus", 25, 32, 18, 26, "plate")],
    "key_help_64x64": [("A", 3, 2, 17, 26, "plate"), ("B", 3, 32, 17, 26, "plate")],
    "help_136x40": [("stick", 6, 5, 20, 24, "bare"), ("B", 80, 4, 17, 26, "plate")],
    "help02_112x40": [("Z", 6, 8, 17, 16, "plate")],
    "help_a_b_168x48": [("A", 16, 10, 17, 26, "plate"), ("B", 90, 10, 17, 26, "plate")],
    "help_b_back_96x48": [("B", 21, 10, 17, 26, "plate")],
    "help_200x88": [("stick", 13, 10, 20, 24, "bare"), ("stick", 13, 53, 20, 24, "bare")],
    "a__56x64": [("A", 15, 9, 18, 26, "plate")], "a_02_56x64": [("A", 15, 9, 18, 26, "plate")],
    "a_03_56x64": [("A", 15, 9, 18, 26, "plate")], "a_04_56x64": [("A", 15, 9, 18, 26, "plate")],
    "a_b_88x64": [("A", 15, 9, 18, 26, "plate"), ("B", 48, 9, 18, 26, "plate")],
    "a_b02_88x64": [("A", 15, 9, 18, 26, "plate"), ("B", 48, 9, 18, 26, "plate")],
    "a_b03_88x64": [("A", 15, 9, 18, 26, "plate"), ("B", 48, 9, 18, 26, "plate")],
    "a_b04_88x64": [("A", 15, 9, 18, 26, "plate"), ("B", 48, 9, 18, 26, "plate")],
}

# ---------------------------------------------------------------------------------------------- what each button becomes
# Matches the controls (runtime/platform.c): A = left click / pad south button, B = right click / pad east button,
# Z = E / left trigger, C = Q / left bumper, + / - = the + / - keys / Menu-View (Options-Share/Create),
# D-pad = arrow keys / D-pad, Nunchuk stick = W A S D / left stick.
SYMBOL = {
    "keyboard": {"A": ("mouse", "L"), "B": ("mouse", "R"), "Z": ("key", "E"), "C": ("key", "Q"), "plus": ("key", "+"),
                 "minus": ("key", "−"), "dpad_lr": ("arrows", "lr"), "dpad_ud": ("arrows", "ud"), "stick": ("wasd", None)},
    "xbox": {"A": ("face", "A"), "B": ("face", "B"), "Z": ("text", "LT"), "C": ("text", "LB"), "plus": ("menu", None),
             "minus": ("view", None), "dpad_lr": None, "dpad_ud": None, "stick": ("stick", "L")},
    "ps4": {"A": ("face", "cross"), "B": ("face", "circle"), "Z": ("text", "L2"), "C": ("text", "L1"),
            "plus": ("text", "OPT"), "minus": ("text", "SHR"), "dpad_lr": None, "dpad_ud": None, "stick": ("stick", "L")},
    "ps5": {"A": ("face", "cross"), "B": ("face", "circle"), "Z": ("text", "L2"), "C": ("text", "L1"),
            "plus": ("menu", None), "minus": ("create", None), "dpad_lr": None, "dpad_ud": None, "stick": ("stick", "L")},
}

FONT_PATHS = ["/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "C:/Windows/Fonts/arialbd.ttf",
              "C:/Windows/Fonts/segoeuib.ttf", "/System/Library/Fonts/Supplemental/Arial Bold.ttf"]


def font(px):
    for p in FONT_PATHS:
        if os.path.exists(p):
            return ImageFont.truetype(p, int(px))
    return ImageFont.load_default()


# ---------------------------------------------------------------------------------------------- enlarging
def scale2x(a):
    h, w, _ = a.shape
    p = np.pad(a, ((1, 1), (1, 1), (0, 0)), mode="edge")
    B = p[0:h, 1:w + 1]; D = p[1:h + 1, 0:w]; E = p[1:h + 1, 1:w + 1]; F = p[1:h + 1, 2:w + 2]; H = p[2:h + 2, 1:w + 1]
    eq = lambda x, y: np.all(x == y, axis=2)
    c = (~eq(B, H)) & (~eq(D, F))
    o = np.empty((h * 2, w * 2, 4), a.dtype)
    o[0::2, 0::2] = np.where((c & eq(D, B))[..., None], D, E); o[0::2, 1::2] = np.where((c & eq(B, F))[..., None], F, E)
    o[1::2, 0::2] = np.where((c & eq(D, H))[..., None], D, E); o[1::2, 1::2] = np.where((c & eq(H, F))[..., None], F, E)
    return o


# ---------------------------------------------------------------------------------------------- symbols
def draw_symbol(kind, arg, w, h, ink):
    """An RGBA symbol of size w x h (drawn 4x larger, then reduced for smooth edges) in colour ink."""
    S = 4; W, H = w * S, h * S
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    m = min(W, H); cx, cy = W / 2, H / 2
    lw = max(2, int(m * 0.09))
    def text(t, size, y=cy):
        f = font(size); bb = d.textbbox((0, 0), t, font=f)
        d.text((cx - (bb[0] + bb[2]) / 2, y - (bb[1] + bb[3]) / 2), t, font=f, fill=ink)
    if kind == "mouse":            # a mouse seen from above, the pressed button filled
        mw, mh = W * 0.7, H * 0.84; x0, y0 = cx - mw / 2, cy - mh / 2
        d.rounded_rectangle([x0, y0, x0 + mw, y0 + mh], radius=mw * 0.48, outline=ink, width=lw)
        d.line([x0, y0 + mh * 0.42, x0 + mw, y0 + mh * 0.42], fill=ink, width=lw)
        d.line([cx, y0, cx, y0 + mh * 0.42], fill=ink, width=lw)
        # fill the pressed half of the top
        mask = Image.new("L", (W, H), 0); md = ImageDraw.Draw(mask)
        md.rounded_rectangle([x0, y0, x0 + mw, y0 + mh], radius=mw * 0.48, fill=255)
        half = Image.new("L", (W, H), 0); hd = ImageDraw.Draw(half)
        hx0, hx1 = (x0, cx) if arg == "L" else (cx, x0 + mw)
        hd.rectangle([hx0, y0, hx1, y0 + mh * 0.42], fill=255)
        im.paste(Image.new("RGBA", (W, H), ink), (0, 0), Image.fromarray(np.minimum(np.array(mask), np.array(half))))
    elif kind == "key":            # a keycap with its label
        k = m * 0.86; x0, y0 = cx - k / 2, cy - k / 2
        d.rounded_rectangle([x0, y0, x0 + k, y0 + k], radius=k * 0.2, outline=ink, width=lw)
        text(arg, k * 0.62)
    elif kind == "arrows":         # two arrow keys
        k = min(W * 0.8, H * 0.42); f = k * 0.5
        for i, ch in enumerate(("◀", "▶") if arg == "lr" else ("▲", "▼")):
            yy = cy - k * 0.55 + i * k * 1.1
            d.rounded_rectangle([cx - k / 2, yy - k / 2, cx + k / 2, yy + k / 2], radius=k * 0.2, outline=ink, width=lw)
            tri = {"◀": [(-1, 0), (0.6, -0.8), (0.6, 0.8)], "▶": [(1, 0), (-0.6, -0.8), (-0.6, 0.8)],
                   "▲": [(0, -1), (-0.8, 0.6), (0.8, 0.6)], "▼": [(0, 1), (-0.8, -0.6), (0.8, -0.6)]}[ch]
            d.polygon([(cx + px * f * 0.45, yy + py * f * 0.45) for px, py in tri], fill=ink)
    elif kind == "wasd":           # W over A S D
        k = min(W / 3.3, H / 2.3); g = k * 0.12
        pos = [("W", cx, cy - (k + g) / 2), ("A", cx - k - g, cy + (k + g) / 2), ("S", cx, cy + (k + g) / 2), ("D", cx + k + g, cy + (k + g) / 2)]
        for ch, x, y in pos:
            d.rounded_rectangle([x - k / 2, y - k / 2, x + k / 2, y + k / 2], radius=k * 0.2, outline=ink, width=max(2, lw // 2))
            f = font(k * 0.58); bb = d.textbbox((0, 0), ch, font=f)
            d.text((x - (bb[0] + bb[2]) / 2, y - (bb[1] + bb[3]) / 2), ch, font=f, fill=ink)
    elif kind == "face":           # a round face button
        r = m * 0.44
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=ink, width=lw)
        if arg == "cross":
            q = r * 0.48
            d.line([cx - q, cy - q, cx + q, cy + q], fill=ink, width=int(lw * 1.3)); d.line([cx - q, cy + q, cx + q, cy - q], fill=ink, width=int(lw * 1.3))
        elif arg == "circle":
            q = r * 0.5; d.ellipse([cx - q, cy - q, cx + q, cy + q], outline=ink, width=int(lw * 1.2))
        else:
            text(arg, r * 1.25)
    elif kind == "stick":          # an analog stick from above
        r = m * 0.45
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=ink, width=lw)
        r2 = r * 0.66; d.ellipse([cx - r2, cy - r2, cx + r2, cy + r2], fill=ink)
        f = font(r2 * 1.1); bb = d.textbbox((0, 0), arg, font=f)
        d.text((cx - (bb[0] + bb[2]) / 2, cy - (bb[1] + bb[3]) / 2), arg, font=f, fill=(0, 0, 0, 0))
        return im.resize((w, h), Image.LANCZOS), True     # the letter is cut out
    elif kind == "text":           # a shoulder / small button with its name
        k = m * 0.9
        d.rounded_rectangle([cx - W * 0.44, cy - k * 0.38, cx + W * 0.44, cy + k * 0.38], radius=k * 0.2, outline=ink, width=lw)
        text(arg, min(k * 0.42, W * 0.88 / max(1, len(arg)) * 1.5))
    elif kind == "menu":           # three lines
        r = m * 0.42; d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=ink, width=lw)
        for i in (-1, 0, 1):
            d.line([cx - r * 0.45, cy + i * r * 0.32, cx + r * 0.45, cy + i * r * 0.32], fill=ink, width=lw)
    elif kind == "view":           # two overlapping windows
        r = m * 0.42; d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=ink, width=lw)
        q = r * 0.36
        d.rectangle([cx - q * 1.2, cy - q * 1.0, cx + q * 0.4, cy + q * 0.4], outline=ink, width=lw)
        d.rectangle([cx - q * 0.4, cy - q * 0.4, cx + q * 1.2, cy + q * 1.0], fill=ink)
    elif kind == "create":         # PS5 Create: a small pill with three rays
        r = m * 0.42; d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=ink, width=lw)
        d.rounded_rectangle([cx - r * 0.18, cy - r * 0.05, cx + r * 0.18, cy + r * 0.55], radius=r * 0.18, fill=ink)
        for ang in (-50, 0, 50):
            t = np.radians(ang - 90); x1, y1 = cx + np.cos(t) * r * 0.25, cy - r * 0.15 + np.sin(t) * r * 0.25
            x2, y2 = cx + np.cos(t) * r * 0.55, cy - r * 0.15 + np.sin(t) * r * 0.55
            d.line([x1, y1, x2, y2], fill=ink, width=lw)
    return im.resize((w, h), Image.LANCZOS), False


# ---------------------------------------------------------------------------------------------- one picture
def colours(orig, x, y, w, h):
    """The plate colour (the lightest common colour in the box, with its alpha: some plates are half see-through
    and tinted by the game) and the symbol colour (the darkest), or a see-through symbol (alpha 0)."""
    box = orig[y:y + h, x:x + w].reshape(-1, 4)
    amax = box[:, 3].max()
    if amax == 0:
        return (200, 200, 200, 255), (0, 0, 0, 0)
    solid = box[box[:, 3] >= amax * 0.8]
    lum = solid[:, :3].mean(1)
    plate = tuple(int(v) for v in np.median(solid[lum >= np.percentile(lum, 60)], axis=0))
    if (box[:, 3] < amax * 0.4).mean() > 0.12:      # the symbol is cut out of the plate
        return plate, (0, 0, 0, 0)
    sym = tuple(int(v) for v in np.median(solid[lum <= np.percentile(lum, 20)], axis=0))
    return plate, sym


def make(orig_img, base_img, slots, style):
    orig = np.array(orig_img.convert("RGBA")).astype(np.int32)
    base = base_img.convert("RGBA")
    S = base.width / orig_img.width
    out = base.copy()
    for button, x, y, w, h, kind in slots:
        sym = SYMBOL[style].get(button)
        if sym is None:
            continue                                    # same as the Wii Remote (D-pad on a controller)
        plate, ink = colours(orig, x, y, w, h)
        X, Y, Wd, Hd = round(x * S), round(y * S), round(w * S), round(h * S)
        if kind == "plate":
            # the plate's inside (keep its rim and rounded corners): refill, then the new symbol
            ix, iy = round((x + 2) * S), round((y + 2) * S); iw, ih = round((w - 4) * S), round((h - 4) * S)
            fill = Image.new("RGBA", (iw, ih), plate)
            out.paste(fill, (ix, iy))
            glyph, cut = draw_symbol(sym[0], sym[1], iw, ih, ink if ink[3] else (255, 255, 255, 255))
            if ink[3] == 0:                             # see-through symbol: punch it out of the plate
                region = np.array(out.crop((ix, iy, ix + iw, iy + ih)))
                a = np.array(glyph)[..., 3].astype(np.int32)
                region[..., 3] = (region[..., 3] * (255 - a) // 255).astype(np.uint8)
                out.paste(Image.fromarray(region), (ix, iy))
            else:
                out.alpha_composite(glyph, (ix, iy))
        else:
            # a bare icon: clear it (to transparent, or to the surrounding colour) and draw the new one in the plate colour
            ring = np.concatenate([orig[max(0, y - 2), x:x + w], orig[min(orig.shape[0] - 1, y + h + 1), x:x + w]])
            bg = (0, 0, 0, 0) if (ring[:, 3] < 100).mean() > 0.5 else tuple(int(v) for v in np.median(ring, axis=0))
            out.paste(Image.new("RGBA", (Wd, Hd), bg), (X, Y))
            glyph, cut = draw_symbol(sym[0], sym[1], Wd, Hd, plate)
            if cut and bg[3]:                            # cut-out letter: show the background through it
                under = Image.new("RGBA", (Wd, Hd), bg); under.alpha_composite(glyph); glyph = under
            out.alpha_composite(glyph, (X, Y))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("orig"); ap.add_argument("out"); ap.add_argument("--upscaled")
    a = ap.parse_args()
    done = 0
    for root, dirs, files in os.walk(a.orig):
        for fn in files:
            if not fn.startswith("tt_") or not fn.endswith(".png"):
                continue
            name = fn[20:-4]
            if name not in PLATES:
                continue
            rel = os.path.relpath(os.path.join(root, fn), a.orig)
            orig = Image.open(os.path.join(root, fn))
            up = os.path.join(a.upscaled, rel) if a.upscaled else None
            if up and os.path.exists(up):
                base = Image.open(up)
            else:
                base = Image.fromarray(scale2x(scale2x(np.array(orig.convert("RGBA")))))
            for style in SYMBOL:
                img = make(orig, base, PLATES[name], style)
                dst = os.path.join(a.out, style, fn)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                img.save(dst)
            done += 1
    print(f"{done} pictures x {len(SYMBOL)} styles -> {a.out}")


if __name__ == "__main__":
    main()
