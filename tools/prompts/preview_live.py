"""Preview of what the runtime draws (mirror of texpack.c prompt_compose + platform.c prompt_tokens).
    python preview_live.py <live dir> <glyphs dir> <out dir> [remap]
remap: an alternative binding set to check that odd mappings still read well."""
import os, sys, glob
from PIL import Image, ImageDraw, ImageFont
LIVE, GL, OUT = sys.argv[1:4]; REMAP = len(sys.argv) > 4
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
# default bindings (platform.c): keyboard key, pad button (SDL numbers; 30 LT, 31 RT)
KB = dict(Z="E", C="Q", plus="=", minus="-", home="Esc", one="1", two="2", up="W", left="A", down="S", right="D")
PAD = dict(A=0, B=1, GRAB=31, Z=30, C=9, plus=6, minus=4, home=5, one=2, two=3, SWING=3, TW_L=9, TW_R=10)
if REMAP:
    KB.update(Z="Shift", C="Ctrl", plus="Enter", up="Up", left="Left", down="Down", right="Right")
    PAD.update(A=2, GRAB=10, Z=9, C=30, TW_L=11, TW_R=12)
STY = {1: "keyboard", 2: "xbox", 3: "ps4", 4: "ps5"}


def P(p): return ("g", "lt" if p == 30 else "rt" if p == 31 else "b%d" % p)


def tokens(st, a):
    kb = st == 1; K = lambda k: ("k", KB[k]); T = lambda t: ("t", t); G = lambda g: ("g", g)
    if a == "A": return [G("mouse_l")] if kb else [P(PAD["A"])]
    if a == "B": return [G("mouse_r")] if kb else [P(PAD["B"])]
    if a == "AB": return [G("mouse_l"), T("+"), G("mouse_r")] if kb else [P(PAD["A"]), T("+"), P(PAD["GRAB"])]
    if a == "ZB": return [K("Z"), T("+"), G("mouse_r")] if kb else [P(PAD["Z"]), T("+"), P(PAD["GRAB"])]
    if a in ("Z", "C", "plus", "minus", "home", "one", "two"): return [K(a)] if kb else [P(PAD[a])]
    if a == "stick": return [G("wasd" if KB["up"] == "W" else "arrows")] if kb else [G("ls")]
    if a == "pointA": return [G("mouse_move"), T("+"), G("mouse_l")] if kb else [G("rs"), T("+"), P(PAD["A"])]
    if a == "pointer": return [G("mouse_move" if kb else "rs")]
    if a in ("dpad", "dpad_lr", "dpad_ud"): return [G("arrows" + a[4:])] if kb else [G(a)]
    if a == "swing": return [G("mouse_m")] if kb else [P(PAD["SWING"])]
    if a in ("twist", "twistn"):
        if kb: return ([("k", "Ctrl"), T("+")] if a == "twistn" else []) + [G("wheel")]
        return ([P(PAD["Z"]), T("+")] if a == "twistn" else []) + [P(PAD["TW_L"]), T("/"), P(PAD["TW_R"])]
    if a == "paddles": return [K("up"), T("+"), G("wheel")] if kb else [P(7), T("+"), P(8)]
    if a in ("fwd", "back"): return [G("wheel" if kb else "rs")]
    if a == "circle": return [G("mouse_l"), T("+"), G("mouse_move")] if kb else [P(PAD["A"]), T("+"), G("rs")]
    return []


cache = {}
def glyph(st, n):
    d = STY[st]; k = (d, n)
    if k not in cache:
        f = os.path.join(GL, d, n + ".png")
        if not os.path.exists(f): f = os.path.join(GL, "xbox", n + ".png")
        cache[k] = Image.open(f).convert("RGBA") if os.path.exists(f) else None
    return cache[k]


def tw(s, px): return ImageFont.truetype(FONT, max(1, int(px))).getlength(s)


def compose(img, slots, st):
    W, H = img.size; d = ImageDraw.Draw(img)
    for x0, y0, x1, y1, act in slots:
        tk = tokens(st, act)
        if not tk: continue
        rx, ry, rw, rh = x0 * W, y0 * H, (x1 - x0) * W, (y1 - y0) * H; gh = rh
        for _ in range(2):
            ws = []
            for kind, n in tk:
                if kind == "k": kg = glyph(1, "key"); ws.append(max(tw(n, gh * .42) + gh * .42, gh * kg.width / kg.height))
                elif kind == "t": ws.append(tw(n, gh * .6) + gh * .08)
                else: g = glyph(st, n); ws.append(gh * g.width / g.height if g else gh * .8)
            tot = sum(ws)
            if tot <= rw: break
            gh *= rw / tot
        x = rx + (rw - tot) / 2; y = ry + (rh - gh) / 2
        for (kind, n), w in zip(tk, ws):
            if kind == "t": d.text((x + gh * .04, y + gh * .18), n, font=ImageFont.truetype(FONT, max(1, int(gh * .6))), fill=(236, 236, 236))
            else:
                g = glyph(1 if kind == "k" else st, "key" if kind == "k" else n)
                if g: img.alpha_composite(g.resize((max(1, round(w)), max(1, round(gh))), Image.BILINEAR), (round(x), round(y)))
                if kind == "k":
                    ts = gh * .42; d.text((x + (w - tw(n, ts)) / 2, y + gh * .27), n, font=ImageFont.truetype(FONT, max(1, int(ts))), fill=(28, 28, 30))
            x += w


S = {}
for ln in open(os.path.join(LIVE, "slots.txt")):
    p = ln.split()
    if len(p) == 6: S.setdefault(p[0], []).append(tuple(map(float, p[1:5])) + (p[5],))
os.makedirs(OUT, exist_ok=True)
for f in sorted(glob.glob(os.path.join(LIVE, "base", "tt_*"))):
    h = os.path.basename(f)[3:19]; base = Image.open(f).convert("RGBA"); row = []
    for st in (1, 2, 4):
        im = base.copy(); compose(im, S.get(h, []), st); row.append(im)
    w, hh = row[0].size; sc = 304 / w if w > 304 else 1
    strip = Image.new("RGB", (int(w * sc) * 3 + 4, int(hh * sc)), (40, 40, 44))
    for i, im in enumerate(row):
        bg = Image.new("RGBA", im.size, (40, 40, 44, 255)); bg.alpha_composite(im)
        strip.paste(bg.convert("RGB").resize((int(w * sc), int(hh * sc)), Image.LANCZOS), ((int(w * sc) + 2) * i, 0))
    strip.save(os.path.join(OUT, h + ".jpg"), quality=85)
print(len(S), "pictures")
