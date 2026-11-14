"""Button glyphs for live prompts: prompts/glyphs/<style>/<name>.png, each a light plate (like the game's own
button plates) with a dark symbol, 128 px tall. The runtime composes them into the tutorial pictures, the speech
bubbles and the in-text icon sheet from the player's current bindings (texpack.c prompt_compose).
  pad styles (xbox, ps4, ps5): b0..b20 = SDL controller buttons, lt, rt, ls, rs (stick moves), dpad
  keyboard: mouse_l, mouse_r, mouse_m, wheel, mouse_move, key (a blank keycap; the runtime writes the key name)
All drawn here: no game art."""
import os, sys
import numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_prompts import draw_symbol, font

H = 128; W = 104; PLATE = (222, 222, 228, 255); INK = (28, 28, 30, 255); HOT = (236, 118, 26, 255)   # HOT: the pressed part


def plate(w=W, h=H):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(im).rounded_rectangle([0, 0, w - 1, h - 1], radius=min(w, h) * 0.22, fill=PLATE)
    return im


def sym(kind, arg, w=W, pad=10):
    im = plate(w)
    g, _ = draw_symbol(kind, arg, w - 2 * pad, H - 2 * pad, INK)
    im.alpha_composite(g, (pad, pad)); return im


def custom(fn, w=W):
    S = 4; im = Image.new("RGBA", (w * S, H * S), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    fn(d, w * S, H * S, max(6, int(H * S * 0.06)))
    out = plate(w); out.alpha_composite(im.resize((w, H), Image.LANCZOS)); return out


def arrows4(d, cx, cy, r, t, ink):
    for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
        bx, by = cx + dx * r, cy + dy * r; tip = (bx + dx * t * 1.5, by + dy * t * 1.5)
        d.polygon([tip, (bx - dy * t, by + dx * t), (bx + dy * t, by - dx * t)], fill=ink)


def stick_move(letter):
    def f(d, W_, H_, lw):
        cx, cy = W_ / 2, H_ / 2; r = min(W_, H_) * 0.27
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=INK, width=lw)
        r2 = r * 0.62; d.ellipse([cx - r2, cy - r2, cx + r2, cy + r2], fill=INK)
        fo = font(r2 * 1.1); bb = d.textbbox((0, 0), letter, font=fo)
        d.text((cx - (bb[0] + bb[2]) / 2, cy - (bb[1] + bb[3]) / 2), letter, font=fo, fill=PLATE)
        arrows4(d, cx, cy, r * 1.25, r * 0.22, INK)
    return f


def mouse_move(d, W_, H_, lw):
    cx, cy = W_ / 2, H_ / 2; mw, mh = W_ * 0.34, H_ * 0.46; x0, y0 = cx - mw / 2, cy - mh / 2
    d.rounded_rectangle([x0, y0, x0 + mw, y0 + mh], radius=mw * 0.48, outline=INK, width=lw)
    d.line([cx, y0, cx, y0 + mh * 0.4], fill=INK, width=lw); d.line([x0, y0 + mh * 0.4, x0 + mw, y0 + mh * 0.4], fill=INK, width=lw)
    arrows4(d, cx, cy, max(mw, mh) * 0.62, W_ * 0.06, INK)


def mouse_btn(which):
    def f(d, W_, H_, lw):
        cx, cy = W_ / 2, H_ / 2; mw, mh = W_ * 0.62, H_ * 0.8; x0, y0 = cx - mw / 2, cy - mh / 2
        d.rounded_rectangle([x0, y0, x0 + mw, y0 + mh], radius=mw * 0.48, outline=INK, width=lw)
        sy = y0 + mh * 0.42
        if which in ("L", "R"):
            # fill the pressed half: clip a rectangle to the body
            m = Image.new("L", (int(W_), int(H_)), 0); md = ImageDraw.Draw(m)
            md.rounded_rectangle([x0, y0, x0 + mw, y0 + mh], radius=mw * 0.48, fill=255)
            h = Image.new("L", m.size, 0); ImageDraw.Draw(h).rectangle([x0 if which == "L" else cx, y0, cx if which == "L" else x0 + mw, sy], fill=255)
            d._image.paste(HOT, None, Image.fromarray(np.minimum(np.array(m), np.array(h))))
            d.rounded_rectangle([x0, y0, x0 + mw, y0 + mh], radius=mw * 0.48, outline=INK, width=lw)
        d.line([x0 + lw / 2, sy, x0 + mw - lw / 2, sy], fill=INK, width=lw); d.line([cx, y0, cx, sy], fill=INK, width=lw)
        ww, wh = mw * 0.16, mh * 0.2; wx0, wy0 = cx - ww / 2, y0 + mh * 0.1
        if which == "M": d.rounded_rectangle([wx0 - ww * .3, wy0, wx0 + ww * 1.3, wy0 + wh * 1.2], radius=ww, fill=HOT, outline=INK, width=max(2, lw // 2))
        if which == "W":
            d.rounded_rectangle([wx0 - ww * .3, wy0, wx0 + ww * 1.3, wy0 + wh * 1.2], radius=ww, fill=INK)
            t = W_ * 0.09
            for s in (-1, 1):
                ty = wy0 + wh * 0.6 + s * wh * 0.95
                d.polygon([(cx, ty + s * t), (cx - t, ty), (cx + t, ty)], fill=INK)
    return f


def dpad(d, W_, H_, lw):
    cx, cy = W_ / 2, H_ / 2; k = min(W_, H_) * 0.22
    d.rectangle([cx - k / 2, cy - k * 1.5, cx + k / 2, cy + k * 1.5], fill=INK); d.rectangle([cx - k * 1.5, cy - k / 2, cx + k * 1.5, cy + k / 2], fill=INK)


def main(out):
    PAD = {
        "xbox": {0: ("face", "A"), 1: ("face", "B"), 2: ("face", "X"), 3: ("face", "Y"), 4: ("view", None), 5: ("text", "Guide"), 6: ("menu", None)},
        "ps4": {0: ("face", "cross"), 1: ("face", "circle"), 2: ("face", "square"), 3: ("face", "triangle"), 4: ("text", "SHARE"), 5: ("text", "PS"), 6: ("text", "OPT")},
        "ps5": {0: ("face", "cross"), 1: ("face", "circle"), 2: ("face", "square"), 3: ("face", "triangle"), 4: ("create", None), 5: ("text", "PS"), 6: ("menu", None)},
    }
    SHOULDER = {"xbox": ("LS", "RS", "LB", "RB", "LT", "RT"), "ps4": ("L3", "R3", "L1", "R1", "L2", "R2"), "ps5": ("L3", "R3", "L1", "R1", "L2", "R2")}
    for st in ("xbox", "ps4", "ps5"):
        d = os.path.join(out, st); os.makedirs(d, exist_ok=True)
        for b, s in PAD[st].items(): (sym(*s) if s[0] != "face" or s[1] not in ("square", "triangle") else facesym(s[1])).save(f"{d}/b{b}.png")
        ls, rs, lb, rb, lt, rt = SHOULDER[st]
        sym("text", ls).save(f"{d}/b7.png"); sym("text", rs).save(f"{d}/b8.png")
        sym("text", lb).save(f"{d}/b9.png"); sym("text", rb).save(f"{d}/b10.png")
        sym("text", lt).save(f"{d}/lt.png"); sym("text", rt).save(f"{d}/rt.png")
        for i, ch in ((11, "up"), (12, "down"), (13, "left"), (14, "right")): dpad_dir(ch).save(f"{d}/b{i}.png")
        for i, n in ((15, "MISC"), (16, "P1"), (17, "P2"), (18, "P3"), (19, "P4"), (20, "PAD")): sym("text", n).save(f"{d}/b{i}.png")
        custom(stick_move("L")).save(f"{d}/ls.png"); custom(stick_move("R")).save(f"{d}/rs.png"); custom(dpad).save(f"{d}/dpad.png")
    d = os.path.join(out, "keyboard"); os.makedirs(d, exist_ok=True)
    for n, w in (("L", "mouse_l"), ("R", "mouse_r"), ("M", "mouse_m"), ("W", "wheel")): custom(mouse_btn(n)).save(f"{d}/{w}.png")
    custom(mouse_move).save(f"{d}/mouse_move.png")
    def keycap(dd, W_, H_, lw):
        k = min(W_, H_) * 0.86; cx, cy = W_ / 2, H_ / 2
        dd.rounded_rectangle([W_ * 0.07, cy - k / 2, W_ * 0.93, cy + k / 2], radius=k * 0.2, outline=INK, width=lw)
    custom(keycap).save(f"{d}/key.png")
    # key clusters for the default movement / D-pad keys (other bindings get keycaps with the key names)
    custom(cluster("WASD"), 150).save(f"{d}/wasd.png"); custom(cluster("arrows"), 150).save(f"{d}/arrows.png")
    custom(cluster("arrows", "lr"), 150).save(f"{d}/arrows_lr.png"); custom(cluster("arrows", "ud"), 150).save(f"{d}/arrows_ud.png")
    for st in ("xbox", "ps4", "ps5"):
        custom(dpad_two("lr")).save(f"{out}/{st}/dpad_lr.png"); custom(dpad_two("ud")).save(f"{out}/{st}/dpad_ud.png")


def cluster(keys, hot=None):
    """an inverted-T of four keys: W/A/S/D letters or arrows; hot = the pair to highlight (lr / ud)"""
    def f(d, W_, H_, lw):
        k = min(W_ / 3.3, H_ / 2.25); gap = k * 0.1; cx = W_ / 2; y1 = H_ / 2 + k + gap / 2
        pos = {"u": (cx - k / 2, y1 - 2 * k - gap), "l": (cx - 1.5 * k - gap, y1 - k), "d": (cx - k / 2, y1 - k), "r": (cx + k / 2 + gap, y1 - k)}
        lab = dict(zip("ulдr".replace("д", "d"), "WASD")) if keys == "WASD" else None
        for n, (x, y) in pos.items():
            on = hot is None or n in hot
            d.rounded_rectangle([x, y, x + k, y + k], radius=k * 0.2, fill=HOT if (hot and on) else None, outline=INK if on else (150, 150, 158, 255), width=max(4, lw * 2 // 3))
            c = INK if on else (150, 150, 158, 255); mx, my = x + k / 2, y + k / 2
            if lab:
                fo = font(k * 0.55); bb = d.textbbox((0, 0), lab[n], font=fo); d.text((mx - (bb[0] + bb[2]) / 2, my - (bb[1] + bb[3]) / 2), lab[n], font=fo, fill=c)
            else:
                dx, dy = {"u": (0, -1), "d": (0, 1), "l": (-1, 0), "r": (1, 0)}[n]; t = k * 0.2
                tip = (mx + dx * t * 1.2, my + dy * t * 1.2); bx, by = mx - dx * t * 0.6, my - dy * t * 0.6
                d.polygon([tip, (bx - dy * t, by + dx * t), (bx + dy * t, by - dx * t)], fill=c)
    return f


def dpad_two(hot):
    def f(d, W_, H_, lw):
        cx, cy = W_ / 2, H_ / 2; k = min(W_, H_) * 0.22; grey = (150, 150, 158, 255)
        d.rectangle([cx - k / 2, cy - k * 1.5, cx + k / 2, cy + k * 1.5], fill=INK if hot == "ud" else grey)
        d.rectangle([cx - k * 1.5, cy - k / 2, cx + k * 1.5, cy + k / 2], fill=INK if hot == "lr" else grey)
        if hot == "ud": d.rectangle([cx - k / 2, cy - k * 1.5, cx + k / 2, cy + k * 1.5], fill=INK)
    return f


def facesym(which):
    def f(d, W_, H_, lw):
        cx, cy = W_ / 2, H_ / 2; r = min(W_, H_) * 0.4
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=INK, width=lw); q = r * 0.48
        if which == "square": d.rectangle([cx - q, cy - q, cx + q, cy + q], outline=INK, width=lw)
        else: d.polygon([(cx, cy - q * 1.1), (cx - q * 1.05, cy + q * 0.75), (cx + q * 1.05, cy + q * 0.75)], outline=INK, width=lw)
    return custom(f)


def dpad_dir(which):
    def f(d, W_, H_, lw):
        cx, cy = W_ / 2, H_ / 2; k = min(W_, H_) * 0.22
        grey = (150, 150, 158, 255)
        d.rectangle([cx - k / 2, cy - k * 1.5, cx + k / 2, cy + k * 1.5], fill=grey); d.rectangle([cx - k * 1.5, cy - k / 2, cx + k * 1.5, cy + k / 2], fill=grey)
        dx, dy = {"up": (0, -1), "down": (0, 1), "left": (-1, 0), "right": (1, 0)}[which]
        x0, y0 = cx + dx * k - k / 2, cy + dy * k - k / 2
        d.rectangle([x0 + dx * k * 0.5 - (k * 0.5 if dx else 0) * 0, y0, x0 + k, y0 + k], fill=INK) if False else None
        d.rectangle([cx + dx * k - k / 2 + min(dx, 0) * k * 0.5, cy + dy * k - k / 2 + min(dy, 0) * k * 0.5,
                     cx + dx * k + k / 2 + max(dx, 0) * k * 0.5, cy + dy * k + k / 2 + max(dy, 0) * k * 0.5], fill=INK)
    return custom(f)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "glyphs")
