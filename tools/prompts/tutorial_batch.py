"""All tutorial pictures: LaMa-erase the Wii hardware, redraw bubble plates, or add a small input card where a
pointing / stick-tilting Remote or Nunchuk stood. Writes out/<style>/<name>.jpg (full size, for the game) and
strips/<hash>.jpg (original | keyboard | xbox | ps5, for the catalogue). Skips pictures already done."""
import sys, os, glob, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from PIL import Image
from scipy import ndimage
import make_tutorials as M, tutorial_lama as L, tutorial_v2 as T
STY = ("keyboard", "xbox", "ps4", "ps5")
orig, up, out = sys.argv[1:4]; limit = float(sys.argv[4]) if len(sys.argv) > 4 else 1e9
import time; t0 = time.time(); n = 0; log = {}
lp = os.path.join(out, "log.json")
if os.path.exists(lp): log = json.load(open(lp))
for f in sorted(glob.glob(os.path.join(orig, "*", "*", "tt_*.png"))):
    name = os.path.basename(f); h = name[3:19]
    if "tutorial.tpx" in f or "button.tpx" in f or h in log: continue
    if time.time() - t0 > limit: break
    rel = os.path.relpath(f, orig); upf = os.path.join(up, rel)
    if not os.path.exists(upf): log[h] = "no upscaled"; continue
    a = np.array(Image.open(f).convert("RGB")).astype(int)
    big_pl = M.find_plates(a); small_pl = M.find_plates(a, small=True)
    u = np.array(Image.open(upf).convert("RGB")); S = u.shape[1] / a.shape[1]
    cards = []
    if big_pl:
        m = L.device_mask(a, big_pl); kind = "bubble"
    else:
        devs = [] if h in M.FIX else M.find_devices(a)
        m = np.zeros(a.shape[:2], bool)
        for dm, top, cx, k in devs:
            pls, off = M.bubble_above(a, top, cx)
            m |= ndimage.binary_dilation(dm, iterations=4)
            if pls: big_pl += pls
            else:
                keys = [p[4] for p in small_pl] if off and small_pl else (["A", "B"] if off else ["stick" if k == "nunchuk" else "pointer"])
                cards.append((cx, top, keys, off))
        kind = "device" if devs else ("small only" if small_pl else "none")
    if h in M.FIX:
        m = np.zeros(a.shape[:2], bool)
        for x0, y0, x1, y1, cab, k in M.FIX[h]:
            m[y0:y1, x0:x1] = True
            if cab is not None: m[y1:224, cab - 3:cab + 4] = True
        big_pl = M.FIX_PLATES.get(h) or big_pl; kind = "fixed"
    base = L.erase(u, m) if m.any() else u
    res = {}
    for st in STY:
        im = Image.fromarray(base).convert("RGBA")
        L.plates(im, a, big_pl + small_pl, st, S)
        for cx, top, keys, off in cards:
            c = T.card(st, keys, off, S)
            X = int(min(max(0, cx * S - c.width / 2), im.width - c.width)); Y = int(min(max(0, (top + 4) * S), im.height - c.height - 6 * S))
            im.alpha_composite(c, (X, Y))
        res[st] = im.convert("RGB")
        if kind != "none":
            os.makedirs(os.path.join(out, st), exist_ok=True); res[st].save(os.path.join(out, st, name[:-4] + ".jpg"), quality=90)
    os.makedirs(os.path.join(out, "strips"), exist_ok=True)
    W, H = 304, 232; strip = Image.new("RGB", (W * 4 + 6, H), (20, 20, 20))
    strip.paste(Image.open(f).convert("RGB").resize((W, H)), (0, 0))
    for i, st in enumerate(("keyboard", "xbox", "ps5")): strip.paste(res[st].resize((W, H), Image.LANCZOS), ((W + 2) * (i + 1), 0))
    strip.save(os.path.join(out, "strips", h + ".jpg"), quality=80)
    log[h] = kind; n += 1
    json.dump(log, open(lp, "w"))
print(n, "done this run;", len(log), "total")
