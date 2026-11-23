import argparse
import json
import math
import struct
import subprocess
from collections import deque
from pathlib import Path
import numpy as np
from numpy.lib.stride_tricks import sliding_window_view as swv
from PIL import Image


GEOMETRIZE_EXE = Path(__file__).resolve().parent / "geometrize_cli" / "build" / "geometrize_cli.exe"
SPRITES_V4_DIR = Path(__file__).resolve().parent / "geometrize_cli" / "sprites_v4"


def sample_border_bg_color(rgb: np.ndarray) -> np.ndarray:
    top = rgb[0, :, :]
    bot = rgb[-1, :, :]
    left = rgb[:, 0, :]
    right = rgb[:, -1, :]
    border = np.concatenate([top, bot, left, right], axis=0)
    return np.median(border, axis=0).astype(np.float32)


def erode_mask(mask: np.ndarray, steps: int) -> np.ndarray:
    cur = mask.copy()
    for _ in range(steps):
        p = np.pad(cur, 1, mode="constant", constant_values=False)
        cur = p[:-2, 1:-1] & p[2:, 1:-1] & p[1:-1, :-2] & p[1:-1, 2:] & p[1:-1, 1:-1]
    return cur


def dilate_mask(mask: np.ndarray, steps: int) -> np.ndarray:
    cur = mask.copy()
    for _ in range(steps):
        p = np.pad(cur, 1, mode="constant", constant_values=False)
        cur = p[:-2, 1:-1] | p[2:, 1:-1] | p[1:-1, :-2] | p[1:-1, 2:] | p[1:-1, 1:-1]
    return cur


def decontaminate_edge_rgb(rgb: np.ndarray, fg: np.ndarray, rim_erode: int = 2, out_dilate: int = 4) -> np.ndarray:
    """
    Remove background color halo from the 1-2 px anti-aliased outer rim of a cutout AND
    pad pure interior foreground colors 4 px outward into the transparent background before
    LANCZOS resizing (prevents LANCZOS from blending foreground edges with black (0,0,0) or sky/pink bg!).
    """
    h, w, _ = rgb.shape
    core = erode_mask(fg, rim_erode)
    if not core.any():
        core = fg.copy()
    out = rgb.astype(np.float32).copy()
    valid = core.copy()
    for _ in range(rim_erode + out_dilate):
        missing = ~valid
        if not missing.any():
            break
        acc = np.zeros_like(out)
        cnt = np.zeros((h, w), dtype=np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)):
            v_shift = np.zeros_like(valid)
            r_shift = np.zeros_like(out)
            ys0, ys1 = max(0, -dy), h - max(0, dy)
            xs0, xs1 = max(0, -dx), w - max(0, dx)
            yd0, yd1 = max(0, dy), h - max(0, -dy)
            xd0, xd1 = max(0, dx), w - max(0, -dx)
            v_shift[yd0:yd1, xd0:xd1] = valid[ys0:ys1, xs0:xs1]
            r_shift[yd0:yd1, xd0:xd1] = out[ys0:ys1, xs0:xs1]
            m = v_shift & missing
            acc[m] += r_shift[m]
            cnt[m] += 1.0
        filled = missing & (cnt > 0)
        if not filled.any():
            break
        out[filled] = acc[filled] / cnt[filled, None]
        valid[filled] = True
    return np.clip(np.round(out), 0, 255).astype(np.uint8)


def build_foreground_mask(
    rgba: np.ndarray,
    bg_mode: str,
    bg_tol: float,
    remove_holes: bool,
    min_component_ratio: float,
    erode_px: int = 0,
    strip_white_border: bool = False,
) -> tuple[np.ndarray, np.ndarray]:
    h, w, _ = rgba.shape
    rgb = rgba[:, :, :3].astype(np.float32)
    alpha = rgba[:, :, 3]

    if bg_mode == "none":
        return np.ones((h, w), dtype=bool), sample_border_bg_color(rgb)

    has_real_alpha = (alpha < 128).mean() > 0.01
    if bg_mode == "auto":
        mode = "alpha" if has_real_alpha else "flood"
    else:
        mode = bg_mode

    bg_rgb = sample_border_bg_color(rgb)

    if mode == "alpha":
        fg = alpha >= 128
    elif mode == "key":
        dist = np.linalg.norm(rgb - bg_rgb[None, None, :], axis=2)
        fg = dist > bg_tol
        # For monochrome line-art on a white background (e.g. picture.jpg), exclude diffuse light-gray airbrush halo
        # around eye hatching while preserving enclosed mid-gray fills (like the tongue inside the dark mouth outline)
        chroma = rgb.max(axis=2) - rgb.min(axis=2)
        if bg_rgb.mean() > 240.0 and (chroma > 20.0).mean() < 0.02:
            luma = 0.299 * rgb[:, :, 0] + 0.587 * rgb[:, :, 1] + 0.114 * rgb[:, :, 2]
            dark_ink = luma < 168.0
            near_dark = dilate_mask(luma < 110.0, 18)
            fg = dark_ink | ((luma < 218.0) & near_dark & (np.indices((h, w))[0] > int(0.55 * h)))
    elif mode == "sticker_white":
        luma = 0.299 * rgb[:, :, 0] + 0.587 * rgb[:, :, 1] + 0.114 * rgb[:, :, 2]
        chroma = rgb.max(axis=2) - rgb.min(axis=2)
        is_white_wall = (luma >= 235.0) & (chroma <= 32.0)
        bg = np.zeros((h, w), dtype=bool)
        q = deque()
        for x in range(w):
            if not is_white_wall[0, x] and not bg[0, x]:
                bg[0, x] = True; q.append((0, x))
            if not is_white_wall[h - 1, x] and not bg[h - 1, x]:
                bg[h - 1, x] = True; q.append((h - 1, x))
        for y in range(h):
            if not is_white_wall[y, 0] and not bg[y, 0]:
                bg[y, 0] = True; q.append((y, 0))
            if not is_white_wall[y, w - 1] and not bg[y, w - 1]:
                bg[y, w - 1] = True; q.append((y, w - 1))
        while q:
            cy, cx = q.popleft()
            if cy > 0 and not is_white_wall[cy - 1, cx] and not bg[cy - 1, cx]:
                bg[cy - 1, cx] = True; q.append((cy - 1, cx))
            if cy + 1 < h and not is_white_wall[cy + 1, cx] and not bg[cy + 1, cx]:
                bg[cy + 1, cx] = True; q.append((cy + 1, cx))
            if cx > 0 and not is_white_wall[cy, cx - 1] and not bg[cy, cx - 1]:
                bg[cy, cx - 1] = True; q.append((cy, cx - 1))
            if cx + 1 < w and not is_white_wall[cy, cx + 1] and not bg[cy, cx + 1]:
                bg[cy, cx + 1] = True; q.append((cy, cx + 1))
        fg = ~bg
    elif mode == "flood":
        dist = np.linalg.norm(rgb - bg_rgb[None, None, :], axis=2)
        is_bg_cand = dist <= bg_tol
        bg = np.zeros((h, w), dtype=bool)
        q = deque()
        for x in range(w):
            if is_bg_cand[0, x] and not bg[0, x]:
                bg[0, x] = True; q.append((0, x))
            if is_bg_cand[h - 1, x] and not bg[h - 1, x]:
                bg[h - 1, x] = True; q.append((h - 1, x))
        for y in range(h):
            if is_bg_cand[y, 0] and not bg[y, 0]:
                bg[y, 0] = True; q.append((y, 0))
            if is_bg_cand[y, w - 1] and not bg[y, w - 1]:
                bg[y, w - 1] = True; q.append((y, w - 1))
        while q:
            cy, cx = q.popleft()
            if cy > 0 and is_bg_cand[cy - 1, cx] and not bg[cy - 1, cx]:
                bg[cy - 1, cx] = True; q.append((cy - 1, cx))
            if cy + 1 < h and is_bg_cand[cy + 1, cx] and not bg[cy + 1, cx]:
                bg[cy + 1, cx] = True; q.append((cy + 1, cx))
            if cx > 0 and is_bg_cand[cy, cx - 1] and not bg[cy, cx - 1]:
                bg[cy, cx - 1] = True; q.append((cy, cx - 1))
            if cx + 1 < w and is_bg_cand[cy, cx + 1] and not bg[cy, cx + 1]:
                bg[cy, cx + 1] = True; q.append((cy, cx + 1))
        if remove_holes:
            # Hysteresis interior background hole opening:
            # 1. A genuine enclosed background pocket (e.g. between hair strands) has a core matching bg_rgb within
            #    strict tolerance (min(14.0, bg_tol * 0.28)) of area >= 25 px.
            # 2. Flood-fill from those validated cores out to the full bg_tol so no background halo rim remains inside loops,
            #    while NEVER punching false holes inside specular hair highlights!
            strict_tol = min(14.0, bg_tol * 0.28)
            core_cand = (~bg) & (dist <= strict_tol)
            vis_h = np.zeros((h, w), dtype=bool)
            hy, hx = np.where(core_cand)
            seeds = []
            for sy, sx in zip(hy.tolist(), hx.tolist()):
                if vis_h[sy, sx]:
                    continue
                comp = []
                qh = deque([(sy, sx)])
                vis_h[sy, sx] = True
                while qh:
                    cy, cx = qh.popleft()
                    comp.append((cy, cx))
                    for ny, nx in ((cy - 1, cx), (cy + 1, cx), (cy, cx - 1), (cy, cx + 1)):
                        if 0 <= ny < h and 0 <= nx < w and core_cand[ny, nx] and not vis_h[ny, nx]:
                            vis_h[ny, nx] = True
                            qh.append((ny, nx))
                if len(comp) >= 25:
                    seeds.extend(comp)
            if seeds:
                qh = deque(seeds)
                for sy, sx in seeds:
                    bg[sy, sx] = True
                while qh:
                    cy, cx = qh.popleft()
                    for ny, nx in ((cy - 1, cx), (cy + 1, cx), (cy, cx - 1), (cy, cx + 1)):
                        if 0 <= ny < h and 0 <= nx < w and is_bg_cand[ny, nx] and not bg[ny, nx]:
                            bg[ny, nx] = True
                            qh.append((ny, nx))
        fg = ~bg
    else:
        raise ValueError(f"Unknown bg_mode: {bg_mode}")

    # Optional: peel exterior white sticker border up to the dark outline of the character
    if strip_white_border and fg.any():
        luma = 0.299 * rgb[:, :, 0] + 0.587 * rgb[:, :, 1] + 0.114 * rgb[:, :, 2]
        chroma = rgb.max(axis=2) - rgb.min(axis=2)
        is_white = fg & (luma >= 210.0) & (chroma <= 42.0)
        bg = ~fg
        q = deque()
        ys, xs = np.where(is_white)
        for y, x in zip(ys.tolist(), xs.tolist()):
            if (y == 0 or y == h - 1 or x == 0 or x == w - 1 or
                bg[y - 1, x] or bg[y + 1, x] or bg[y, x - 1] or bg[y, x + 1]):
                bg[y, x] = True
                q.append((y, x))
        while q:
            cy, cx = q.popleft()
            for ny, nx in ((cy - 1, cx), (cy + 1, cx), (cy, cx - 1), (cy, cx + 1)):
                if 0 <= ny < h and 0 <= nx < w and is_white[ny, nx] and not bg[ny, nx]:
                    bg[ny, nx] = True
                    q.append((ny, nx))
        fg = ~bg

    if min_component_ratio > 0.0:
        visited = np.zeros((h, w), dtype=bool)
        comps = []
        ys, xs = np.where(fg)
        for sy, sx in zip(ys.tolist(), xs.tolist()):
            if visited[sy, sx]:
                continue
            comp = []
            q = deque([(sy, sx)])
            visited[sy, sx] = True
            while q:
                cy, cx = q.popleft()
                comp.append((cy, cx))
                for ny, nx in ((cy - 1, cx), (cy + 1, cx), (cy, cx - 1), (cy, cx + 1)):
                    if 0 <= ny < h and 0 <= nx < w and fg[ny, nx] and not visited[ny, nx]:
                        visited[ny, nx] = True
                        q.append((ny, nx))
            comps.append(comp)
        if comps:
            max_sz = max(len(c) for c in comps)
            thresh = max_sz * min_component_ratio
            new_fg = np.zeros((h, w), dtype=bool)
            for c in comps:
                if len(c) >= thresh:
                    for cy, cx in c:
                        new_fg[cy, cx] = True
            fg = new_fg

    if erode_px > 0 and fg.any():
        er = erode_mask(fg, erode_px)
        if er.any():
            fg = er

    return fg, bg_rgb


def build_vivid_color_target(rgba: np.ndarray) -> np.ndarray:
    """
    Create a Line-Free Vivid Color Target for Engine v5 coarse base silhouette fills:
    Inpaint ONLY thin dark ink strokes (width <= 6px) with adjacent vivid interior colors,
    preserving genuine large dark fills (like dark hair, sleeves, or black mouth cavities).
    """
    h, w, _ = rgba.shape
    rgb = rgba[:, :, :3].astype(np.float32)
    fg = rgba[:, :, 3] > 0
    luma = 0.299 * rgb[:, :, 0] + 0.587 * rgb[:, :, 1] + 0.114 * rgb[:, :, 2]

    is_dark = fg & (luma < 85.0)
    is_bright_fg = fg & (luma >= 105.0)

    near_bright = dilate_mask(is_bright_fg, 3)
    thin_ink = is_dark & near_bright
    chroma = rgb.max(axis=2) - rgb.min(axis=2)
    if fg.any() and (chroma[fg] > 20.0).mean() < 0.08:
        return rgba.copy()

    out_rgb = rgb.copy()
    valid = fg & (~thin_ink)
    for _ in range(5):
        missing = thin_ink & (~valid)
        if not missing.any():
            break
        acc = np.zeros_like(out_rgb)
        cnt = np.zeros((h, w), dtype=np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)):
            v_shift = np.zeros_like(valid)
            r_shift = np.zeros_like(out_rgb)
            ys0, ys1 = max(0, -dy), h - max(0, dy)
            xs0, xs1 = max(0, -dx), w - max(0, dx)
            yd0, yd1 = max(0, dy), h - max(0, -dy)
            xd0, xd1 = max(0, dx), w - max(0, -dx)
            v_shift[yd0:yd1, xd0:xd1] = valid[ys0:ys1, xs0:xs1]
            r_shift[yd0:yd1, xd0:xd1] = out_rgb[ys0:ys1, xs0:xs1]
            m = v_shift & missing
            acc[m] += r_shift[m]
            cnt[m] += 1.0
        filled = missing & (cnt > 0)
        out_rgb[filled] = acc[filled] / cnt[filled, None]
        valid[filled] = True

    out = rgba.copy()
    out[:, :, :3] = np.clip(np.round(out_rgb), 0, 255).astype(np.uint8)
    return out


def compute_contour_valley_and_thickness(rgba: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """
    Morphological Black-Hat Valley + Gradient + Silhouette-Rim detector:
    Isolates EVERY dark contour line (black ink on hair, charcoal/brown lines on bright skin/teeth,
    eyelids, mouth, jawline, ear folds) while staying 0.0 inside flat dark hair fills!
    Also computes true local stroke thickness in pixels at every contour pixel.
    """
    h, w, _ = rgba.shape
    rgb = rgba[:, :, :3].astype(np.float32) / 255.0
    fg = rgba[:, :, 3] > 0
    luma = 0.299 * rgb[:, :, 0] + 0.587 * rgb[:, :, 1] + 0.114 * rgb[:, :, 2]

    gx = np.zeros((h, w), dtype=np.float32)
    gy = np.zeros((h, w), dtype=np.float32)
    gx[:, 1:-1] = np.linalg.norm(rgb[:, 2:, :] - rgb[:, :-2, :], axis=2)
    gy[1:-1, :] = np.linalg.norm(rgb[2:, :, :] - rgb[:-2, :, :], axis=2)
    grad = np.sqrt(gx * gx + gy * gy)

    # 7x7 local maximum of foreground luma (Black-Hat morphological filter)
    luma_fg = np.where(fg, luma, -1.0)
    local_max = swv(np.pad(luma_fg, 3, mode="edge"), (7, 7)).max(axis=(2, 3))
    valley = np.where(fg & (local_max > 0.0), np.clip(local_max - luma, 0.0, 1.0), 0.0)

    # 2px inner silhouette rim
    sil_rim = fg & (~erode_mask(fg, 2))

    # True local thickness of dark valley / contour mask via iterative erosion distance transform
    stroke_mask = fg & ((valley >= 0.12) | (grad >= 0.34) | (sil_rim & (luma < 0.45)))
    dist = np.zeros((h, w), dtype=np.float32)
    cur = stroke_mask.copy()
    for _ in range(5):
        if not cur.any():
            break
        dist += cur.astype(np.float32)
        cur = erode_mask(cur, 1)
    # Dilate dist by 1px so centerline gets full half-width
    p_d = np.pad(dist, 1, mode="edge")
    half_w = swv(p_d, (3, 3)).max(axis=(2, 3))
    thickness_map = np.clip(2.0 * half_w, 1.2, 6.5)

    return valley, grad, sil_rim, thickness_map


def extract_contour_segments(rgba: np.ndarray) -> tuple[bytes, int]:
    """
    Extract ordered smooth contour stroke segments from morphological Black-Hat ridge centerlines,
    silhouette rims, and strong edges, with true local stroke thickness and multi-scale slicing.
    """
    h, w, _ = rgba.shape
    fg = rgba[:, :, 3] > 0
    valley, grad, sil_rim, thickness_map = compute_contour_valley_and_thickness(rgba)

    score = valley + 0.45 * grad + 0.25 * sil_rim.astype(np.float32)
    # Directional ridge thinning (non-maximum suppression along 4 axes) so we trace 1px centerlines
    p = np.pad(score, 1, mode="constant", constant_values=0.0)
    c = p[1:-1, 1:-1]
    max_h = (c >= p[1:-1, :-2]) & (c >= p[1:-1, 2:])
    max_v = (c >= p[:-2, 1:-1]) & (c >= p[2:, 1:-1])
    max_d1 = (c >= p[:-2, :-2]) & (c >= p[2:, 2:])
    max_d2 = (c >= p[:-2, 2:]) & (c >= p[2:, :-2])
    is_ridge = fg & (score >= 0.16) & ((max_h.astype(int) + max_v.astype(int) + max_d1.astype(int) + max_d2.astype(int)) >= 2)
    mask = is_ridge | (fg & (~erode_mask(fg, 1)) & (score >= 0.12))

    visited = np.zeros((h, w), dtype=bool)
    ys, xs = np.where(mask)
    # Order starting points by descending score so sharpest contours trace unbroken first
    order = np.argsort(-score[ys, xs])
    ys = ys[order]
    xs = xs[order]

    chains = []
    nbrs = [(-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)]
    for sy, sx in zip(ys.tolist(), xs.tolist()):
        if visited[sy, sx]:
            continue
        chain = [(sx, sy)]
        visited[sy, sx] = True
        cy, cx = sy, sx
        prev_dx, prev_dy = 0.0, 0.0
        while len(chain) < 100:
            best_n = None
            best_sc = -999.0
            for dy, dx in nbrs:
                ny, nx = cy + dy, cx + dx
                if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not visited[ny, nx]:
                    nlen = math.hypot(dx, dy)
                    ndot = (dx * prev_dx + dy * prev_dy) / nlen if (prev_dx or prev_dy) else 0.5
                    if ndot < -0.35:
                        continue
                    sc = ndot * 1.8 + float(score[ny, nx]) * 1.5
                    if sc > best_sc:
                        best_sc = sc
                        best_n = (ny, nx, dx / nlen, dy / nlen)
            if best_n is None:
                break
            ny, nx, ndx, ndy = best_n
            visited[ny, nx] = True
            chain.append((nx, ny))
            prev_dx = 0.6 * prev_dx + 0.4 * ndx if (prev_dx or prev_dy) else ndx
            prev_dy = 0.6 * prev_dy + 0.4 * ndy if (prev_dx or prev_dy) else ndy
            plen = math.hypot(prev_dx, prev_dy)
            if plen > 1e-5:
                prev_dx /= plen
                prev_dy /= plen
            cy, cx = ny, nx
        if len(chain) >= 4:
            chains.append(chain)

    # Multi-scale slicing:
    # 1. Primary sweeping contour curves (win=20, step=9)
    # 2. Short fine detail segments (win=8, step=6) for mouth lines, nose, eyelids, ear folds
    segs = []
    for ch in chains:
        windows = []
        if len(ch) >= 12:
            win, step = 20, 9
            if len(ch) <= win:
                windows.append(ch)
            else:
                for i in range(0, len(ch) - 5, step):
                    windows.append(ch[i:i + win])
        # Also add fine segments where contour score or curvature is high, or chain is short
        if len(ch) < 12:
            windows.append(ch)
        else:
            for i in range(0, len(ch) - 4, 7):
                sl = ch[i:i + 9]
                mx, my = sl[len(sl) // 2]
                if valley[my, mx] >= 0.24 or len(ch) < 20:
                    windows.append(sl)

        for sl in windows:
            if len(sl) < 4:
                continue
            p0 = sl[0]
            p1 = sl[-1]
            pm = sl[len(sl) // 2]
            dx = float(p1[0] - p0[0])
            dy = float(p1[1] - p0[1])
            length = math.hypot(dx, dy)
            if length < 2.8:
                continue
            cx = sum(p[0] for p in sl) / float(len(sl))
            cy = sum(p[1] for p in sl) / float(len(sl))
            angle_deg = math.degrees(math.atan2(dy, dx)) % 360.0
            nx_vec, ny_vec = -dy / length, dx / length
            mx_chord = 0.5 * (p0[0] + p1[0])
            my_chord = 0.5 * (p0[1] + p1[1])
            sagitta = (pm[0] - mx_chord) * nx_vec + (pm[1] - my_chord) * ny_vec
            curvature = sagitta / max(1.0, length)
            iy = max(0, min(h - 1, int(round(cy))))
            ix = max(0, min(w - 1, int(round(cx))))
            thickness = float(thickness_map[iy, ix])
            segs.append((cx, cy, length * 1.12, thickness, angle_deg, curvature))

    buf = bytearray(struct.pack("<I", len(segs)))
    for s in segs:
        buf.extend(struct.pack("<6f", *s))
    return bytes(buf), len(segs)


def build_importance_map(
    rgba: np.ndarray,
    focus_box: tuple[float, float, float, float] | None = None,
    focus_boost: float = 3.5,
) -> np.ndarray:
    """
    Contour- and Silhouette-focused importance map:
    Uses morphological Black-Hat valley (all dark contour lines regardless of global luminance),
    White-Hat peak (bright eye sparkles & teeth), 2px silhouette boundary rim, and gradient.
    Does NOT inflate flat saturated fills over dark/gray contour line-art!
    """
    h, w, _ = rgba.shape
    rgb = rgba[:, :, :3].astype(np.float32) / 255.0
    fg = rgba[:, :, 3] > 0
    luma = 0.299 * rgb[:, :, 0] + 0.587 * rgb[:, :, 1] + 0.114 * rgb[:, :, 2]

    valley, grad, sil_rim, _ = compute_contour_valley_and_thickness(rgba)
    grad_norm = np.clip(grad / 0.80, 0.0, 1.0)

    # White-Hat local peak for eye highlights, teeth, earring, specular shine
    luma_fg_hi = np.where(fg, luma, 2.0)
    local_min = swv(np.pad(luma_fg_hi, 3, mode="edge"), (7, 7)).min(axis=(2, 3))
    peak = np.where(fg & (local_min <= 1.0), np.clip(luma - local_min, 0.0, 1.0), 0.0)

    pad_g = np.pad(grad_norm, 3, mode="edge")
    density = np.clip(swv(pad_g, (7, 7)).mean(axis=(2, 3)) * 3.0, 0.0, 1.0)
    chroma = rgb.max(axis=2) - rgb.min(axis=2)

    weight = (
        1.0
        + 2.6 * grad_norm
        + 4.5 * valley
        + 1.8 * peak
        + 2.2 * sil_rim.astype(np.float32)
        + 2.4 * density
        + 1.2 * chroma
    )

    if focus_box is not None:
        fx0, fy0, fx1, fy1 = focus_box
        yy, xx = np.indices((h, w), dtype=np.float32)
        nx = xx / max(1.0, float(w - 1))
        ny = yy / max(1.0, float(h - 1))
        in_x = np.clip(np.minimum((nx - fx0) / 0.04, (fx1 - nx) / 0.04), 0.0, 1.0)
        in_y = np.clip(np.minimum((ny - fy0) / 0.04, (fy1 - ny) / 0.04), 0.0, 1.0)
        weight *= (1.0 + (focus_boost - 1.0) * (in_x * in_y))

    weight = np.where(fg, weight, 1.0)
    return np.clip(np.round(weight * 14.0), 4, 255).astype(np.uint8)


def composite_checkerboard(rgba: np.ndarray, tile: int = 16) -> Image.Image:
    h, w, _ = rgba.shape
    yy, xx = np.indices((h, w))
    chk = (((yy // tile) + (xx // tile)) % 2).astype(np.float32)
    bg = np.zeros((h, w, 3), dtype=np.float32)
    bg[chk == 0] = (46, 50, 58)
    bg[chk == 1] = (72, 78, 90)
    a = (rgba[:, :, 3:4].astype(np.float32)) / 255.0
    rgb = rgba[:, :, :3].astype(np.float32)
    out = np.clip(rgb * a + bg * (1.0 - a), 0, 255).astype(np.uint8)
    return Image.fromarray(out, "RGB")


def composite_on_color(rgba: np.ndarray, bg_hex: str = "#C4161C") -> Image.Image:
    s = bg_hex.strip().lstrip("#")
    br, bg_c, bb = int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16)
    paint = np.array([br, bg_c, bb], dtype=np.float32)[None, None, :]
    a = (rgba[:, :, 3:4].astype(np.float32)) / 255.0
    rgb = rgba[:, :, :3].astype(np.float32)
    out = np.clip(rgb * a + paint * (1.0 - a), 0, 255).astype(np.uint8)
    return Image.fromarray(out, "RGB")


def refresh_aux_for_existing_rgba(prefix: Path, focus_box=None, focus_boost: float = 4.5):
    meta_p = Path(f"{prefix}_meta.json")
    raw_p = Path(f"{prefix}.rgba")
    meta = json.loads(meta_p.read_text(encoding="utf-8"))
    fw, fh = meta["src_size"]
    if focus_box is None and meta.get("focus"):
        focus_box = tuple(float(x) for x in meta["focus"])
    final_rgba = np.frombuffer(raw_p.read_bytes(), dtype=np.uint8).reshape((fh, fw, 4))

    vivid_rgba = build_vivid_color_target(final_rgba)
    Path(f"{prefix}_color.rgba").write_bytes(vivid_rgba.tobytes())
    composite_checkerboard(vivid_rgba).save(Path(f"{prefix}_color_target.png"))

    c_bytes, n_segs = extract_contour_segments(final_rgba)
    Path(f"{prefix}_contours.bin").write_bytes(c_bytes)

    w_u8 = build_importance_map(final_rgba, focus_box=focus_box, focus_boost=focus_boost)
    Path(f"{prefix}_weights.raw").write_bytes(w_u8.tobytes())
    meta["contour_segments"] = n_segs
    meta_p.write_text(json.dumps(meta, indent=2), encoding="utf-8")
    print(f"Refreshed aux for {prefix} ({fw}x{fh}): contours={n_segs}")


def main():
    ap = argparse.ArgumentParser(description="Prepare any RGB/RGBA image with decontaminated cutout, Black-Hat valley contour extraction, and Engine v5 vectorization.")
    ap.add_argument("--input", default="")
    ap.add_argument("--out-prefix", required=True)
    ap.add_argument("--refresh-aux", action="store_true", help="Only refresh _contours.bin, _weights.raw, and _color.rgba from existing <out-prefix>.rgba")
    ap.add_argument("--size", type=int, default=440, help="Max working dimension in pixels")
    ap.add_argument("--bg-mode", choices=["auto", "alpha", "flood", "key", "sticker_white", "none"], default="auto")
    ap.add_argument("--bg-tol", type=float, default=45.0)
    ap.add_argument("--erode", type=int, default=-1, help="Mask erosion in high-res pixels (-1 = auto: 1px for flood/key, 0 for alpha)")
    ap.add_argument("--strip-white-border", action="store_true", help="Peel exterior white die-cut sticker border up to dark character outline")
    ap.add_argument("--posterize", type=int, default=0, help="Optional K-color median-cut posterization of Phase-1 base fill target (e.g. 12 for cel-shaded anime fills)")
    ap.add_argument("--close-bg-color", default=None, help="Fill small interior gaps/spirals inside foreground with this opaque hex color (e.g. #FFF8FB for line-art on a white license plate so alternating dark/white layers can carve spirals & details!)")
    ap.add_argument("--remove-holes", action="store_true")
    ap.add_argument("--min-comp", type=float, default=0.0)
    ap.add_argument("--no-trim", action="store_true")
    ap.add_argument("--pad", type=int, default=8)
    ap.add_argument("--canvas-aspect", type=float, default=1.0, help="Target surface aspect ratio (e.g. 2.0 for 2:1 license plate)")
    ap.add_argument("--focus", default=None, help="Normalized focus box x0,y0,x1,y1")
    ap.add_argument("--focus-boost", type=float, default=4.5)
    ap.add_argument("--no-weights", action="store_true")
    ap.add_argument("--shapes", type=int, default=298)
    ap.add_argument("--alpha", type=int, default=255)
    ap.add_argument("--adaptive-alpha", type=int, default=1)
    ap.add_argument("--candidates", type=int, default=420)
    ap.add_argument("--mutations", type=int, default=240)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--seed", type=int, default=9001)
    ap.add_argument("--bg-penalty", type=float, default=2.5)
    ap.add_argument("--preview-bg", default="#C4161C", help="Car paint hex color for *_preview_on_paint.png")
    ap.add_argument("--prep-only", action="store_true")
    args = ap.parse_args()

    focus_box = None
    if args.focus:
        parts = [float(x) for x in args.focus.split(",")]
        if len(parts) == 4:
            focus_box = tuple(parts)

    prefix = Path(args.out_prefix)
    if args.refresh_aux:
        refresh_aux_for_existing_rgba(prefix, focus_box=focus_box, focus_boost=args.focus_boost)
        return

    if not args.input:
        raise ValueError("--input is required unless --refresh-aux is set")

    src = Image.open(args.input).convert("RGBA")
    orig_w, orig_h = src.size

    work_max = max(args.size * 2, 760)
    scale0 = min(1.0, work_max / float(max(orig_w, orig_h)))
    if scale0 < 1.0:
        src = src.resize((max(1, int(round(orig_w * scale0))), max(1, int(round(orig_h * scale0)))), Image.LANCZOS)

    rgba = np.array(src)
    has_real_alpha = (rgba[:, :, 3] < 128).mean() > 0.01
    eff_mode = ("alpha" if has_real_alpha else "flood") if args.bg_mode == "auto" else args.bg_mode
    erode_px = (1 if eff_mode in ("flood", "key") else 0) if args.erode < 0 else args.erode

    # Automatic removal of localized contrasting corner watermarks (e.g. cyan/blue watermark crossing warm border in picture3.png)
    if eff_mode == "flood":
        h0, w0, _ = rgba.shape
        rgb0 = rgba[:, :, :3].astype(np.float32)
        bg0 = sample_border_bg_color(rgb0)
        if bg0[0] - bg0[2] > 80.0:
            yy, xx = np.indices((h0, w0))
            wm = (yy > int(0.70 * h0)) & (xx > int(0.60 * w0)) & ((rgb0[:, :, 2] - rgb0[:, :, 0]) > 24.0)
            if wm.any():
                wm = dilate_mask(wm, 2)
                valid = ~wm
                out0 = rgb0.copy()
                for _ in range(36):
                    if valid.all():
                        break
                    for dy, dx in ((-1, 0), (1, 0), (-2, 0), (2, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)):
                        v_s = np.zeros_like(valid)
                        r_s = np.zeros_like(out0)
                        ys0, ys1 = max(0, -dy), h0 - max(0, dy)
                        xs0, xs1 = max(0, -dx), w0 - max(0, dx)
                        yd0, yd1 = max(0, dy), h0 - max(0, -dy)
                        xd0, xd1 = max(0, dx), w0 - max(0, -dx)
                        v_s[yd0:yd1, xd0:xd1] = valid[ys0:ys1, xs0:xs1]
                        r_s[yd0:yd1, xd0:xd1] = out0[ys0:ys1, xs0:xs1]
                        m = v_s & (~valid)
                        out0[m] = r_s[m]
                        valid[m] = True
                rgba[:, :, :3] = np.clip(np.round(out0), 0, 255).astype(np.uint8)

    fg, bg_rgb = build_foreground_mask(
        rgba,
        args.bg_mode,
        args.bg_tol,
        args.remove_holes,
        args.min_comp,
        erode_px=erode_px,
        strip_white_border=args.strip_white_border,
    )

    out_rgba = rgba.copy()
    if args.close_bg_color:
        # For line-art on a known surface color (e.g. #FFF8FB license plate):
        # Unmix background color from anti-aliased ink pixels, and keep the morphological closure
        # of the features (eyes, iris spirals, mouth) as opaque foreground over close_bg_color
        # so the optimizer can layer alternating dark and white shapes!
        s_hex = args.close_bg_color.strip().lstrip("#")
        cr, cg, cb = int(s_hex[0:2], 16), int(s_hex[2:4], 16), int(s_hex[4:6], 16)
        closed_fg = erode_mask(dilate_mask(fg, 10), 8)
        # Also fill any interior holes inside closed_fg components
        bg_ext = np.zeros_like(closed_fg)
        q_ext = deque()
        ch, cw = closed_fg.shape
        for x in range(cw):
            if not closed_fg[0, x]: bg_ext[0, x] = True; q_ext.append((0, x))
            if not closed_fg[ch - 1, x]: bg_ext[ch - 1, x] = True; q_ext.append((ch - 1, x))
        for y in range(ch):
            if not closed_fg[y, 0]: bg_ext[y, 0] = True; q_ext.append((y, 0))
            if not closed_fg[y, cw - 1]: bg_ext[y, cw - 1] = True; q_ext.append((y, cw - 1))
        while q_ext:
            cy, cx = q_ext.popleft()
            for ny, nx in ((cy - 1, cx), (cy + 1, cx), (cy, cx - 1), (cy, cx + 1)):
                if 0 <= ny < ch and 0 <= nx < cw and not closed_fg[ny, nx] and not bg_ext[ny, nx]:
                    bg_ext[ny, nx] = True
                    q_ext.append((ny, nx))
        fg = ~bg_ext
        # Tint near-white pixels inside fg to exact close_bg_color and sharpen dark ink contrast
        rgb_f = rgba[:, :, :3].astype(np.float32)
        luma = (0.299 * rgb_f[:, :, 0] + 0.587 * rgb_f[:, :, 1] + 0.114 * rgb_f[:, :, 2]) / 255.0
        # Gamma/contrast curve on ink so anti-aliased gray strokes stay crisp dark ink
        ink_t = np.clip((luma - 0.08) / 0.86, 0.0, 1.0) ** 1.35
        target_bg = np.array([cr, cg, cb], dtype=np.float32)[None, None, :]
        out_rgba[:, :, :3] = np.clip(np.round(ink_t[:, :, None] * target_bg), 0, 255).astype(np.uint8)
    elif args.bg_mode != "none" and fg.any():
        # Decontaminate the 2px outer rim of fg AND dilate pure interior foreground colors 4px outward
        # BEFORE LANCZOS resizing so the downsampling kernel never mixes edge pixels with bg_rgb or black (0,0,0)!
        out_rgba[:, :, :3] = decontaminate_edge_rgb(rgba[:, :, :3], fg, rim_erode=2 if eff_mode in ("flood", "key") else 1, out_dilate=4)
    out_rgba[:, :, 3] = np.where(fg, 255, 0).astype(np.uint8)

    if not args.no_trim and args.bg_mode != "none" and fg.any():
        ys, xs = np.where(fg)
        y0, y1 = max(0, int(ys.min()) - 2), min(out_rgba.shape[0], int(ys.max()) + 3)
        x0, x1 = max(0, int(xs.min()) - 2), min(out_rgba.shape[1], int(xs.max()) + 3)
        out_rgba = out_rgba[y0:y1, x0:x1]

    cur_h, cur_w, _ = out_rgba.shape
    eff_w = cur_w / max(0.25, args.canvas_aspect)
    max_scale = max(1.0, args.canvas_aspect)
    scale = min(max_scale, args.size / float(max(eff_w, cur_h)))
    new_w = max(8, int(round(eff_w * scale)))
    new_h = max(8, int(round(cur_h * scale)))
    pil_rgb = Image.fromarray(out_rgba[:, :, :3], "RGB").resize((new_w, new_h), Image.LANCZOS)
    pil_a = Image.fromarray(out_rgba[:, :, 3], "L").resize((new_w, new_h), Image.BILINEAR)
    final_rgb = np.array(pil_rgb)
    # Keep clean silhouette mask and zero out RGB outside foreground
    final_a = (np.array(pil_a) >= 128).astype(np.uint8) * 255
    final_rgb[final_a == 0] = 0
    final_rgba = np.dstack([final_rgb, final_a])

    pad = args.pad if args.bg_mode != "none" else 0
    if pad > 0:
        padded = np.zeros((new_h + 2 * pad, new_w + 2 * pad, 4), dtype=np.uint8)
        padded[pad:pad + new_h, pad:pad + new_w] = final_rgba
        final_rgba = padded

    fh, fw, _ = final_rgba.shape
    prefix.parent.mkdir(parents=True, exist_ok=True)

    raw_path = Path(f"{prefix}.rgba")
    color_raw = Path(f"{prefix}_color.rgba")
    color_png = Path(f"{prefix}_color_target.png")
    contours_bin = Path(f"{prefix}_contours.bin")
    weights_raw = Path(f"{prefix}_weights.raw")
    tgt_png = Path(f"{prefix}_target.png")
    shapes_json = Path(f"{prefix}_shapes.json")
    prev_raw = Path(f"{prefix}_preview.rgba")
    prev_png = Path(f"{prefix}_preview.png")
    prev_paint_png = Path(f"{prefix}_preview_on_paint.png")
    meta_json = Path(f"{prefix}_meta.json")

    raw_path.write_bytes(final_rgba.tobytes())
    composite_checkerboard(final_rgba).save(tgt_png)

    vivid_rgba = build_vivid_color_target(final_rgba)
    if args.posterize and args.posterize >= 2:
        v_fg = vivid_rgba[:, :, 3] > 0
        q_pil = Image.fromarray(vivid_rgba[:, :, :3], "RGB").quantize(colors=int(args.posterize), method=Image.Quantize.MEDIANCUT).convert("RGB")
        vivid_rgba[:, :, :3] = np.where(v_fg[:, :, None], np.array(q_pil), 0)
    color_raw.write_bytes(vivid_rgba.tobytes())
    composite_checkerboard(vivid_rgba).save(color_png)

    c_bytes, n_segs = extract_contour_segments(final_rgba)
    contours_bin.write_bytes(c_bytes)

    weights_arg = "none"
    if not args.no_weights:
        w_u8 = build_importance_map(final_rgba, focus_box=focus_box, focus_boost=args.focus_boost)
        weights_raw.write_bytes(w_u8.tobytes())
        weights_arg = str(weights_raw)

    fg_pct = (final_rgba[:, :, 3] > 0).mean() * 100.0
    print(f"Prepared {args.input} -> {fw}x{fh} (aspect={fw/fh:.3f}, fg={fg_pct:.1f}%, contours={n_segs}, bg_rgb={bg_rgb.round(1).tolist()})")

    meta = {
        "input": str(args.input),
        "src_size": [fw, fh],
        "aspect": round(fw / fh, 4),
        "canvas_aspect": args.canvas_aspect,
        "fg_percent": round(fg_pct, 2),
        "contour_segments": n_segs,
        "bg_mode": args.bg_mode,
        "bg_tol": args.bg_tol,
        "erode": erode_px,
        "strip_white_border": args.strip_white_border,
        "focus": list(focus_box) if focus_box else None,
        "shapes_file": str(shapes_json),
        "target_shapes": args.shapes,
        "bg_penalty": 0.0 if args.bg_mode == "none" else args.bg_penalty,
    }

    if not args.prep_only:
        eff_penalty = 0.0 if args.bg_mode == "none" else args.bg_penalty
        cmd = [
            str(GEOMETRIZE_EXE),
            str(raw_path),
            str(fw),
            str(fh),
            str(shapes_json),
            str(args.shapes),
            str(args.alpha),
            str(args.candidates),
            str(args.mutations),
            str(args.threads),
            str(args.seed),
            f"{eff_penalty:.2f}",
            str(prev_raw),
            weights_arg,
            str(args.adaptive_alpha),
            str(SPRITES_V4_DIR),
            str(color_raw),
            str(contours_bin),
        ]
        print("Running Engine v5...")
        subprocess.run(cmd, check=True)
        if prev_raw.exists():
            arr = np.frombuffer(prev_raw.read_bytes(), dtype=np.uint8).reshape((fh, fw, 4))
            composite_checkerboard(arr).save(prev_png)
            composite_on_color(arr, args.preview_bg).save(prev_paint_png)
            prev_raw.unlink(missing_ok=True)
            print(f"  Preview: {prev_png} & {prev_paint_png}")
        parsed = json.loads(shapes_json.read_text(encoding="utf-8"))
        actual = len(parsed["shapes"] if isinstance(parsed, dict) else parsed)
        meta["actual_shapes"] = actual
        print(f"  Saved {actual} shapes -> {shapes_json}")

    meta_json.write_text(json.dumps(meta, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
