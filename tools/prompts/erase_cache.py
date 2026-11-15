"""Tutorial pictures with the Wii hardware erased (LaMa) and nothing drawn: <out>/<name>.jpg. Resumable; arg 4 = seconds."""
import sys, os, glob, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from PIL import Image
from scipy import ndimage
import make_tutorials as M
COPY = "--copy-only" in sys.argv; sys.argv = [a for a in sys.argv if a != "--copy-only"]   # no LaMa: only pictures with nothing to erase
if not COPY: import tutorial_lama as L
orig, up, out = sys.argv[1:4]; limit = float(sys.argv[4]) if len(sys.argv) > 4 else 1e9
os.makedirs(out, exist_ok=True); t0 = time.time(); n = left = 0
for f in sorted(glob.glob(os.path.join(orig, "*", "*", "tt_*.png"))):
    name = os.path.basename(f); h = name[3:19]; dst = os.path.join(out, name[:-4] + ".jpg")
    if "tutorial.tpx" in f or "button.tpx" in f or os.path.exists(dst): continue
    upf = os.path.join(up, os.path.relpath(f, orig))
    if not os.path.exists(upf): continue
    if time.time() - t0 > limit: left += 1; continue
    a = np.array(Image.open(f).convert("RGB")).astype(int); u = np.array(Image.open(upf).convert("RGB"))
    big = M.find_plates(a); m = np.zeros(a.shape[:2], bool)
    if h in M.FIX:
        for x0, y0, x1, y1, cab, k in M.FIX[h]:
            m[y0:y1, x0:x1] = True
            if cab is not None: m[y1:224, cab - 3:cab + 4] = True
    elif big: m = M.find_remote(a, big) if COPY else L.device_mask(a, big)
    else:
        for dm, top, cx, k in M.find_devices(a): m |= ndimage.binary_dilation(dm, iterations=4)
    if m.any() and COPY: continue
    Image.fromarray(L.erase(u, m) if m.any() else u).save(dst, quality=92); n += 1
    if n % 20 == 0: print("   tutorial pictures: %d done" % n, flush=True)
print(n, "done,", left, "left")
