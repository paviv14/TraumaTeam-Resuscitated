"""Make the live button prompts from your own disc (run by Setup; also by hand):
    python setup_prompts.py <disc DATA folder> <out prompts folder> <work folder> [--tutorials]

1. extracts the 2D art the prompts are drawn over (sprite/, tutorial/) from the disc;
2. upscales it 4x (so the buttons drawn in later are sharp);
3. blanks the Wii buttons out of the help bars, speech bubbles and the in-text icon sheet;
4. the tutorial pictures stay the game's own: only the buttons drawn in them are redrawn from the player's bindings
   (the old LaMa repaint of the Wii Remote, --tutorials, is no longer used);
5. writes <out>/base, <out>/slots.txt and copies the glyphs (data/glyphs) to <out>/glyphs.
Nothing made here may be shared: it is the game's art."""
import os, sys, glob, shutil, subprocess
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(HERE))
DATA, OUT, WORK = [os.path.abspath(p) for p in sys.argv[1:4]]
TUTS = False   # the tutorial pictures stay the game's own (only their buttons change); --tutorials is ignored
PY = sys.executable
files = os.path.join(DATA, "files"); dol = os.path.join(DATA, "sys", "main.dol")
tex, up, erased = os.path.join(WORK, "tex"), os.path.join(WORK, "up"), os.path.join(WORK, "erased")


def run(*a): print(">", " ".join(os.path.basename(x) if i < 2 else x for i, x in enumerate(a)), flush=True); subprocess.check_call(list(a))


# 1. extract
for sub in ("sprite", "tutorial"):
    if not os.path.isdir(os.path.join(tex, sub)):
        run(PY, os.path.join(ROOT, "tools", "extract_textures.py"), os.path.join(files, sub), os.path.join(tex, sub))
# 2. upscale what the prompts use (the bubbles' sheets, the message window, the tutorials)
sys.path.insert(0, HERE)
from make_prompts import PLATES
want = set()
for n in PLATES: want |= set(glob.glob(os.path.join(tex, "**", "tt_*_" + n + ".png"), recursive=True))
want |= set(glob.glob(os.path.join(tex, "tutorial", "**", "tt_*.png"), recursive=True))
want |= set(glob.glob(os.path.join(tex, "sprite", "misc", "msw.tpx", "*.png")))
k = 0
for f in sorted(want):
    dst = os.path.join(up, os.path.relpath(f, tex))
    if os.path.exists(dst): continue
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    im = Image.open(f); im.convert("RGBA").resize((im.width * 4, im.height * 4), Image.LANCZOS).save(dst); k += 1
print("upscaled", k, "pictures", flush=True)
# 3-4. which text goes with each tutorial picture (the pictures themselves stay the game's own)
cat = os.path.join(WORK, "catalog.json")
run(PY, os.path.join(HERE, "catalog.py"), dol, os.path.join(files, "tutorial"), os.path.join(tex, "tutorial"), cat)
# 5. bases, slots, glyphs
if os.path.isdir(os.path.join(OUT, "base")): shutil.rmtree(os.path.join(OUT, "base"))
run(PY, os.path.join(HERE, "build_live_prompts.py"), tex, up, os.path.join(tex, "tutorial"), erased, cat, OUT)
if os.path.isdir(os.path.join(OUT, "glyphs")): shutil.rmtree(os.path.join(OUT, "glyphs"))
shutil.copytree(os.path.join(ROOT, "data", "glyphs"), os.path.join(OUT, "glyphs"))
print("button prompts ready in", OUT)
