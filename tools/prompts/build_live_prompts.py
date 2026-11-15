"""Live-prompt pictures: <out>/base/tt_<id>_<name>.(png|jpg) with blank button slots, and <out>/slots.txt
(<id> x0 y0 x1 y1 <action>, coordinates 0..1). The runtime draws each slot from the player's bindings.

Sources: the 15 help / speech-bubble prompts (make_prompts.PLATES), the in-text icon sheet (tutorial/button.tpx),
and the tutorial pictures already painted (tutorial_batch.py output, keyboard set: Remote erased with LaMa).
Tutorial slot actions: the bubble's plates (A, B, A+B grab, Z...), else what the page's text says to do."""
import os, sys, glob, json, re
import numpy as np
from scipy import ndimage
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_prompts import PLATES, font
import make_tutorials as M

ORIG, UP, TUT, ERASED, CAT, OUT = sys.argv[1:7]   # 2D art, upscaled pack, tutorial originals, erase_cache.py out, catalog.json, out
os.makedirs(os.path.join(OUT, "base"), exist_ok=True)
slots = []
DARK = (12, 12, 14)


def slot(h, W, H, x0, y0, x1, y1, act): slots.append("%s %.5f %.5f %.5f %.5f %s" % (h, x0 / W, y0 / H, x1 / W, y1 / H, act))


def ring_colour(a, x0, y0, x1, y1):
    """median colour of the 3-px ring just outside the box"""
    H, W = a.shape[:2]; m = np.zeros((H, W), bool)
    m[max(0, y0 - 3):min(H, y1 + 3), max(0, x0 - 3):min(W, x1 + 3)] = True; m[max(0, y0):y1, max(0, x0):x1] = False
    return tuple(int(v) for v in np.median(a[m], 0))


def group(plates, gap=12):
    """plates of one bubble together: [[plate...], ...]"""
    gs = []
    for p in sorted(plates, key=lambda p: p[0]):
        for g in gs:
            q = g[-1]
            if p[0] - (q[0] + q[2]) < gap and abs(p[1] - q[1]) < 8: g.append(p); break
        else: gs.append([p])
    return gs


def act_of(kinds):
    k = set(kinds)
    if k == {"A", "B"}: return "AB"
    if k == {"Z", "B"}: return "ZB"
    return {"plus": "plus", "minus": "minus"}.get(kinds[0], kinds[0])


# 1. help bars and speech bubbles ----------------------------------------------------------------------
KEY = {"dpad_lr": "dpad_lr", "dpad_ud": "dpad_ud", "stick": "stick"}
for f in glob.glob(os.path.join(ORIG, "**", "tt_*.png"), recursive=True):
    fn = os.path.basename(f); name = fn[20:-4]
    if name not in PLATES: continue
    rel = os.path.relpath(f, ORIG); up = os.path.join(UP, rel)
    o = np.array(Image.open(f).convert("RGBA")).astype(int)
    b = Image.open(up if os.path.exists(up) else f).convert("RGBA")
    S = b.width / o.shape[1]; d = ImageDraw.Draw(b); h = fn[3:19]
    pl = [(x, y, w, hh, k) for k, x, y, w, hh, kind in PLATES[name]]
    gs = group(pl, 20) if name.startswith("a_b") else [[p] for p in pl]
    for g in gs:
        x0 = min(p[0] for p in g) - 1; y0 = min(p[1] for p in g) - 1; x1 = max(p[0] + p[2] for p in g) + 1; y1 = max(p[1] + p[3] for p in g) + 1
        bg = ring_colour(o, x0, y0, x1, y1)
        if bg[3] > 100: d.rectangle([x0 * S, y0 * S, x1 * S, y1 * S], fill=bg)
        else: d.rectangle([(x0 - 1) * S, (y0 - 1) * S, (x1 + 1) * S, (y1 + 1) * S], fill=(0, 0, 0, 0))
        slot(h, o.shape[1], o.shape[0], x0, y0, x1, y1, KEY.get(g[0][4], act_of([p[4] for p in g])))
    b.save(os.path.join(OUT, "base", fn))

# 2. the in-text icon sheet ---------------------------------------------------------------------------
f = (glob.glob(os.path.join(ORIG, "**", "button.tpx", "*.png"), recursive=True) or glob.glob("/tmp/tz/tutorial/button.tpx/*.png"))[0]; fn = os.path.basename(f); h = fn[3:19]
o = Image.open(f); Wd, Hd = o.size; S = 4
Image.new("RGBA", (Wd * S, Hd * S), (0, 0, 0, 0)).save(os.path.join(OUT, "base", fn))
for (x, y, w, hh), act in zip([(6, 6, 19, 28), (36, 6, 19, 28), (66, 6, 19, 28), (97, 6, 18, 28), (127, 6, 18, 28), (156, 6, 19, 28),
                               (5, 47, 21, 26), (34, 48, 22, 23), (65, 49, 21, 21), (96, 45, 18, 28)],
                              ["minus", "one", "two", "A", "B", "plus", "stick", "C", "Z", "dpad"]):
    slot(h, Wd, Hd, x, y, x + w, y + hh, act)

# 2b. the message window's "next" button: the A letter (next02) over a round button (next03) -> one A slot on the button
nx = glob.glob(os.path.join(UP, "**", "msw.tpx"), recursive=True)
if nx:
    Image.new("RGBA", (96, 96), (0, 0, 0, 0)).save(os.path.join(OUT, "base", "tt_05CA9D9CD0686106_next02_24x24.png"))
    Image.new("RGBA", (128, 160), (0, 0, 0, 0)).save(os.path.join(OUT, "base", "tt_D32EC6C728A116D0_next03_32x40.png"))
    slot("D32EC6C728A116D0", 1, 1, 0, 0, 1, 1, "A"); slot("05CA9D9CD0686106", 1, 1, 0, 0, 0, 0, "none")   # (kept blank)

# 3. tutorial pictures --------------------------------------------------------------------------------
def stem(n):   # tuto_en_snare_01 / snare_03 -> snare
    n = re.sub(r"^tuto_(su|em|en|di|au|or)_", "", n); n = re.sub(r"^(su|em|en|di|au|or)_", "", n)
    return re.sub(r"(_?\d+|_left|_right|_l_r)+$", "", n)


cat = json.load(open(CAT)); text_of = {}
for t in cat:   # the message shown with each page (page i <-> message i when the counts match, else all of them)
    msgs = [" ".join([m["title"]] + m["lines"]) for m in t["msgs"]]
    for i, fr in enumerate(t["shots"]):
        tx = msgs[i] if len(msgs) == len(t["shots"]) else " ".join(msgs)
        for x in fr: text_of[x["hash"]] = tx; text_of.setdefault(stem(x["name"]), " ".join(msgs))


def device_act(kind, tx):
    """what a Remote / Nunchuk with no bubble stands for, from the page's text ([3] A, [4] B, [6] stick, [8] Z)"""
    t = tx.lower()
    if kind == "nunchuk":
        if "twist the nunchuk" in t: return "twistn"
        if "[6]" in t: return "stick"
        if "[8]" in t: return "Z"
        return "stick"
    if "push the wii remote and nunchuk" in t: return "paddles"
    if "towards the screen" in t: return "fwd"
    if "towards you" in t: return "back"
    if "swing" in t or "strike" in t: return "swing"
    if "rotate the" in t: return "circle"
    if "twist" in t: return "twist"
    if "[3] and [4]" in t: return "AB"
    if "[8] and [4]" in t: return "ZB"
    if "point" in t and "[3]" in t: return "pointA"
    if "[3]" in t: return "A"
    if "[8]" in t: return "Z"
    return "pointer"


HAND = {"50D92CA933200A4D": [(45, 31, 65, 50, "C")]}   # endoscopy tool palette: the round C button
n = 0
for f in sorted(glob.glob(os.path.join(TUT, "*", "*", "tt_*.png"))):
    fn = os.path.basename(f); h = fn[3:19]
    src = os.path.join(ERASED, fn[:-4] + ".jpg")
    if not os.path.exists(src): continue
    a = np.array(Image.open(f).convert("RGB")).astype(int); Hh, Ww = a.shape[:2]
    b = Image.open(src).convert("RGB"); S = b.width / Ww; d = ImageDraw.Draw(b)
    before = len(slots)
    big = M.FIX_PLATES.get(h) or M.find_plates(a)
    def inside_big(p): return any(p[0] < q[0] + q[2] and q[0] < p[0] + p[2] and p[1] < q[1] + q[3] and q[1] < p[1] + p[3] for q in big)
    def is_O(p):   # the "O" of an OFF bubble: a ring, dark in its corners
        x, y, w, hh, k = p; sub = a[y:y + hh, x:x + w].max(2)
        return hh >= 12 and min(int(sub[int(hh * fy), int(w * fx)]) for fy, fx in ((.1, .1), (.1, .9), (.9, .1), (.9, .9))) < 5
    small = [p for p in M.find_plates(a, small=True) if not inside_big(p) and not is_O(p)]
    def fill(x0, y0, x1, y1, r=0.3):
        d.rounded_rectangle([x0 * S, y0 * S, x1 * S, y1 * S], radius=(y1 - y0) * S * r, fill=DARK)
    tx = text_of.get(h) or text_of.get(stem(fn[20:].rsplit("_", 1)[0]), "")
    mxa = a.max(2); dark = mxa < 50
    lab, _ = ndimage.label(dark)

    def bubble_content(top, cx):
        """the light content (a button, or OFF) of the dark speech bubble just over a device: its box, or None.
        A bubble: a compact dark blob (at most 80 x 56) whose bottom (the tail) reaches the device's top."""
        y0 = max(0, top - 60); xa = max(0, int(cx) - 45); xb = min(Ww, int(cx) + 45)
        win = lab[y0:top + 3, xa:xb]; best = None
        for i in np.unique(win):
            if not i: continue
            ys_, xs_ = np.nonzero(lab == i)
            if len(ys_) < 100 or xs_.max() - xs_.min() > 80 or ys_.max() - ys_.min() > 56 or ys_.max() < top - 14: continue
            reg = lab == i; hole = ndimage.binary_fill_holes(reg) & ~reg
            hy, hx = np.nonzero(hole)
            if len(hy) < 20 or hy.max() - hy.min() > 30 or hx.max() - hx.min() > 40: continue
            d_ = top - ys_.max()
            if best is None or abs(d_) < best[0]: best = (abs(d_), hole)
        if not best: return None
        ys, xs = np.nonzero(best[1]); return (xs.min() - 1, ys.min() - 1, xs.max() + 2, ys.max() + 2)
    boxes = []
    for g in group(list(big), 14):
        x0 = min(p[0] for p in g) - 1; y0 = min(p[1] for p in g) - 1; x1 = max(p[0] + p[2] for p in g) + 1; y1 = max(p[1] + p[3] for p in g) + 1
        kinds = [p[4] for p in g]
        # the bubble around the plates: everything light inside it (a missed plate, a "+") is cleared too
        ring = lab[max(0, y0 - 3):y1 + 3, max(0, x0 - 3):x1 + 3]; ids = [i for i in np.unique(ring) if i]
        if ids:
            i = max(ids, key=lambda i: (ring == i).sum()); reg = ndimage.binary_fill_holes(lab == i); hole = reg & ~(lab == i)
            ys, xs = np.nonzero(hole[max(0, y0 - 14):y1 + 14, max(0, x0 - 40):x1 + 40])
            if len(xs) and reg.sum() < Ww * Hh * 0.15:
                bx0 = xs.min() + max(0, x0 - 40) - 1; bx1 = xs.max() + max(0, x0 - 40) + 2
                by0 = ys.min() + max(0, y0 - 14) - 1; by1 = ys.max() + max(0, y0 - 14) + 2
                if bx1 - bx0 > (x1 - x0) * 1.6 and set(kinds) == {"B"}: kinds = ["A", "B"]   # an A plate the detector missed
                x0, y0, x1, y1 = min(x0, bx0), min(y0, by0), max(x1, bx1), max(y1, by1)
        boxes.append([x0, y0, x1, y1, kinds])
    merged = True
    while merged:   # plates of one bubble found apart: one slot
        merged = False
        for i in range(len(boxes)):
            for j in range(i + 1, len(boxes)):
                A, B = boxes[i], boxes[j]
                if A[0] < B[2] and B[0] < A[2] and A[1] < B[3] and B[1] < A[3]:
                    boxes[i] = [min(A[0], B[0]), min(A[1], B[1]), max(A[2], B[2]), max(A[3], B[3]), sorted(set(A[4] + B[4]))]; del boxes[j]; merged = True; break
            if merged: break
    for x0, y0, x1, y1, kinds in boxes:
        kinds = sorted(set(kinds), key=lambda k: "AZB".find(k) if k in "AZB" else 9)
        if set(kinds) == {"A", "B"} and "[8] and [4]" in tx: kinds = ["Z", "B"]
        fill(x0, y0, x1, y1, 0.15); slot(h, Ww, Hh, x0, y0, x1, y1, act_of(kinds))
    for g in group(list(small), 6):
        x0 = min(p[0] for p in g) - 1; y0 = min(p[1] for p in g) - 1; x1 = max(p[0] + p[2] for p in g) + 1; y1 = max(p[1] + p[3] for p in g) + 1
        fill(x0, y0, x1, y1, 0.15); slot(h, Ww, Hh, x0, y0, x1, y1, act_of([p[4] for p in g]))
    if not big and h not in M.FIX:   # Remotes / Nunchuks: their bubble's button, or a new bubble where each stood
        acts = {sl.split()[5] for sl in slots[before:]}
        for m, top, cx, kind in M.find_devices(a):
            if Ww > 380 and abs(cx - Ww / 2) < 16: continue   # the ">" between the two halves of a before/after picture
            if "/en/" in f and kind != "nunchuk" and "[8]" in tx and "[3]" not in tx: kind = "nunchuk"   # endoscopy: the tools are on Z
            pls, off = M.bubble_above(a, top, cx)
            cb = bubble_content(top, cx)
            if pls or (cb and cb[2] - cb[0] < (cb[3] - cb[1]) * 1.6):   # a button in the bubble: drawn from the bindings
                x0, y0, x1, y1 = cb if cb else (min(p[0] for p in pls) - 1, min(p[1] for p in pls) - 1, max(p[0] + p[2] for p in pls) + 1, max(p[1] + p[3] for p in pls) + 1)
                kinds = sorted({p[4] for p in pls}, key=lambda k: "AZB".find(k) if k in "AZB" else 9) or (["Z"] if kind == "nunchuk" or "[8]" in tx else ["A"])
                if set(kinds) == {"A", "B"} and "[8] and [4]" in tx: kinds = ["Z", "B"]
                fill(x0, y0, x1, y1, 0.15); slot(h, Ww, Hh, x0, y0, x1, y1, act_of(kinds)); continue
            act = ("AB" if "[3] and [4]" in tx else "Z" if "[8]" in tx or kind == "nunchuk" else "A") if cb else device_act(kind, tx)   # cb here: an OFF bubble, which stays
            if act in acts and not cb: continue
            acts.add(act)
            w = 52 if act in ("AB", "ZB", "paddles", "twist", "twistn", "circle", "pointA") else 28; hh = 24
            x0 = max(6, min(Ww - 6 - w, cx - w / 2)); y0 = min(Hh - 8 - hh, top + 4)
            fill(x0 - 5, y0 - 4, x0 + w + 5, y0 + hh + 4, 0.45)
            slot(h, Ww, Hh, x0, y0, x0 + w, y0 + hh, act)
    for x0, y0, x1, y1, act in HAND.get(h, []):   # buttons the detectors miss
        fill(x0, y0, x1, y1, 0.5); slot(h, Ww, Hh, x0, y0, x1, y1, act)
    if len(slots) == before: continue          # nothing to show: the game's own picture stays
    b.save(os.path.join(OUT, "base", fn[:-4] + ".jpg"), quality=90); n += 1
open(os.path.join(OUT, "slots.txt"), "w").write("\n".join(slots) + "\n")
print(n, "tutorial pictures,", len(slots), "slots")
