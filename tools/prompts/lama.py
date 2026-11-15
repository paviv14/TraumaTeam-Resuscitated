"""LaMa inpainting (big-lama.pt torchscript; path in LAMA_MODEL, default next to this file)."""
import os, numpy as np, torch
torch.set_num_threads(max(1, (os.cpu_count() or 2) - 1))
_m = None


def lama(img, mask):
    """img HxWx3 uint8, mask HxW bool -> inpainted uint8 (works at the given size; H,W padded to /8)"""
    global _m
    if _m is None:
        p = os.environ.get("LAMA_MODEL") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "big-lama.pt")
        _m = torch.jit.load(p, map_location="cpu").eval()
    H, W = mask.shape; ph = (8 - H % 8) % 8; pw = (8 - W % 8) % 8
    im = np.pad(img, ((0, ph), (0, pw), (0, 0)), mode="reflect"); mk = np.pad(mask, ((0, ph), (0, pw)), mode="constant")
    t = torch.from_numpy(im).permute(2, 0, 1)[None].float() / 255; mt = torch.from_numpy(mk.astype(np.float32))[None, None]
    with torch.no_grad(): out = _m(t, mt)
    o = out[0].permute(1, 2, 0).numpy().clip(0, 255)
    if o.max() <= 1.5: o = o * 255
    return o[:H, :W].astype(np.uint8)
