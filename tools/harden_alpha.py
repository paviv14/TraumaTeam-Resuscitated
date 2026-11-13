"""Harden the soft alpha edges an upscaler gives cut-out sprites (they showed as a white rim where the game
layers a face over a body). Usage: python tools/harden_alpha.py <folder> ... (rewrites the PNGs in place)."""
import sys, glob, os
import numpy as np
from PIL import Image
for d in sys.argv[1:]:
    for f in glob.glob(os.path.join(d, "**", "*.png"), recursive=True):
        a = np.array(Image.open(f).convert("RGBA"))
        a[..., 3] = np.clip((a[..., 3].astype(int) - 112) * 8, 0, 255).astype(np.uint8)
        Image.fromarray(a).save(f)
        print(f)
