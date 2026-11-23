"""Neutral evaluator: renders a shapes JSON with the 256x256 sprite masks (bilinear) and
compares to the prepared target, composited over a car paint colour.
usage: python tools/evalshapes.py <target.rgba> <W> <H> <shapes.json> [scale] [out.png]
"""
import sys, json, math
import numpy as np
from PIL import Image
from pathlib import Path

SPR = Path(__file__).resolve().parent / "geometrize_cli" / "sprites_v4"
_masks = {}

def mask(sid, flip):
    k = (sid, flip)
    if k not in _masks:
        p256 = SPR / f"sprite_{sid}.a256"
        if p256.exists():
            m = np.fromfile(p256, np.uint8).reshape(256, 256).astype(np.float32) / 255.0
        else:
            m = np.fromfile(SPR / f"sprite_{sid}.a64", np.uint8).reshape(64, 64).astype(np.float32) / 255.0
        if flip:
            m = m[:, ::-1]
        _masks[k] = np.ascontiguousarray(m)
    return _masks[k]

def bilinear(m, u, v):
    msz = m.shape[0]
    u = u - 0.5
    v = v - 0.5
    i0 = np.floor(u).astype(int)
    j0 = np.floor(v).astype(int)
    fu = u - i0
    fv = v - j0
    out = np.zeros(u.shape, np.float32)
    for dj, wv in ((0, 1 - fv), (1, fv)):
        for di, wu in ((0, 1 - fu), (1, fu)):
            ii = i0 + di
            jj = j0 + dj
            ok = (ii >= 0) & (ii < msz) & (jj >= 0) & (jj < msz)
            val = np.zeros(u.shape, np.float32)
            val[ok] = m[jj[ok], ii[ok]]
            out += val * wu * wv
    return out

def render(shapes, W, H, scale=1.0):
    w, h = int(round(W * scale)), int(round(H * scale))
    rgb = np.zeros((h, w, 3), np.float32)
    a = np.zeros((h, w), np.float32)
    for s in shapes:
        m = mask(s["sticker"], s.get("flipX", False))
        msz = float(m.shape[0])
        cx, cy, sw, sh = s["cx"] * scale, s["cy"] * scale, s["w"] * scale, s["h"] * scale
        if sw <= 0.1 or sh <= 0.1:
            continue
        ang = math.radians(s["angle"])
        c, sn = math.cos(ang), math.sin(ang)
        ex = abs(c) * sw / 2 + abs(sn) * sh / 2
        ey = abs(sn) * sw / 2 + abs(c) * sh / 2
        x0, x1 = max(0, int(cx - ex) - 1), min(w, int(cx + ex) + 2)
        y0, y1 = max(0, int(cy - ey) - 1), min(h, int(cy + ey) + 2)
        if x0 >= x1 or y0 >= y1:
            continue
        yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float32)
        dx = xx + 0.5 - cx
        dy = yy + 0.5 - cy
        u = (dx * c + dy * sn) / sw * msz + (msz * 0.5)
        v = (-dx * sn + dy * c) / sh * msz + (msz * 0.5)
        cov = bilinear(m, u, v) * (s["color"][3] / 255.0)
        col = np.array(s["color"][:3], np.float32)
        sub = rgb[y0:y1, x0:x1]
        sa = a[y0:y1, x0:x1]
        na = cov + sa * (1 - cov)
        sub[:] = np.where(
            na[..., None] > 0,
            (col * cov[..., None] + sub * (sa * (1 - cov))[..., None]) / np.maximum(na, 1e-6)[..., None],
            sub,
        )
        sa[:] = na
    return rgb, a

PAINT = np.array([196, 22, 28], np.float32)

def composite(rgb, a, paint=PAINT):
    return rgb * a[..., None] + paint * (1 - a[..., None])

def metrics(tgt_rgba, rgb, a):
    H, W = a.shape
    if tgt_rgba.shape[:2] != (H, W):
        t = Image.fromarray(tgt_rgba).resize((W, H), Image.LANCZOS)
        tgt_rgba = np.array(t)
    t = tgt_rgba.astype(np.float32)
    ta = t[..., 3] / 255.0
    T = composite(t[..., :3], ta)
    P = composite(rgb, a)
    d = np.sqrt(((T - P) ** 2).sum(-1))
    L = T @ np.array([.299, .587, .114], np.float32)
    gx = np.zeros_like(L)
    gy = np.zeros_like(L)
    gx[:, 1:-1] = L[:, 2:] - L[:, :-2]
    gy[1:-1] = L[2:] - L[:-2]
    g = np.hypot(gx, gy)
    edge = g > 60
    flat = g < 15
    from numpy.lib.stride_tricks import sliding_window_view as swv
    fgm = ta > .5
    win = swv(np.pad(fgm, 2, mode="edge"), (5, 5))
    band = win.any(axis=(2, 3)) & ~win.all(axis=(2, 3))
    return dict(
        all=float(d.mean()),
        edge=float(d[edge].mean()),
        flat=float(d[flat].mean()),
        sil=float(d[band].mean()),
        spill=int(((a > .5) & (ta < .5)).sum()),
        holes=int(((a < .5) & (ta > .5)).sum()),
    )

if __name__ == "__main__":
    tp, W, H, sp = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    scale = float(sys.argv[5]) if len(sys.argv) > 5 else 1.0
    tgt = np.frombuffer(Path(tp).read_bytes(), np.uint8).reshape(H, W, 4)
    raw = json.loads(Path(sp).read_text(encoding="utf-8"))
    shapes = raw["shapes"] if isinstance(raw, dict) else raw
    rgb, a = render(shapes, W, H, scale)
    m = metrics(tgt, rgb, a)
    print(" ".join(f"{k}={v:.2f}" if isinstance(v, float) else f"{k}={v}" for k, v in m.items()), f"n={len(shapes)}")
    if len(sys.argv) > 6:
        Image.fromarray(np.clip(composite(rgb, a), 0, 255).astype(np.uint8)).save(sys.argv[6])
