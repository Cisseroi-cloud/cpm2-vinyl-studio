"""Extract all 20 primitive sprites at full 256x256 resolution from the 3 game atlas Texture2Ds
using exact sub-pixel m_Rect coordinates from calibration/calibration_dump.json.
Writes tools/geometrize_cli/sprites_v4/sprite_<id>.a256 (andtools/geometrize_cli/sprites_hr/sprite_<id>.a256).
"""
import json
from pathlib import Path
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
CAL_PATH = ROOT / "calibration" / "calibration_dump.json"
META_PATH = ROOT / "tools" / "geometrize_cli" / "sprites_v4" / "sprites_meta.json"
OUT_V4 = ROOT / "tools" / "geometrize_cli" / "sprites_v4"
OUT_HR = ROOT / "tools" / "geometrize_cli" / "sprites_hr"
N = 256


def main():
    cal = json.loads(CAL_PATH.read_text(encoding="utf-8"))
    cat = {int(e["index"]): e for e in cal["catalog"]}
    meta = json.loads(META_PATH.read_text(encoding="utf-8"))

    atlas_paths = {
        "STICKERS_NEW": ROOT / "work" / "atlas_stickers_new.png",
        "STICKERS_NEW_2023": ROOT / "work" / "atlas_stickers_new_2023.png",
        "main car sticker": ROOT / "work" / "atlas_main_car_sticker.png",
    }
    atlases = {}
    for name, p in atlas_paths.items():
        if not p.exists():
            raise FileNotFoundError(f"Missing atlas PNG: {p}")
        atlases[name] = Image.open(p).convert("RGBA")

    OUT_V4.mkdir(parents=True, exist_ok=True)
    OUT_HR.mkdir(parents=True, exist_ok=True)

    tiles = []
    for m in meta:
        idx = int(m["index"])
        e = cat[idx]
        tex = atlases[e["texture"]]
        tw, th = tex.size
        rx, ry, rw, rh = [float(v) for v in e["rect"]]
        # Unity Texture2D coordinates have (0,0) at bottom-left; PIL has (0,0) at top-left.
        x0 = rx
        y0 = float(th) - (ry + rh)
        x1 = rx + rw
        y1 = float(th) - ry

        alpha_img = tex.getchannel("A")
        # Exact sub-pixel extent resampling to NxN
        hr_img = alpha_img.transform((N, N), Image.EXTENT, (x0, y0, x1, y1), Image.BICUBIC)
        hr = np.array(hr_img, dtype=np.uint8)

        (OUT_V4 / f"sprite_{idx}.a256").write_bytes(hr.tobytes())
        (OUT_HR / f"sprite_{idx}.a256").write_bytes(hr.tobytes())

        # Also check agreement with existing 64x64 mask
        a64_new = np.array(hr_img.resize((64, 64), Image.BOX), dtype=np.float32)
        old64 = np.fromfile(OUT_V4 / m["mask_file"], dtype=np.uint8).reshape(64, 64).astype(np.float32)
        diff = np.abs(a64_new - old64)
        print(f"  {idx:3d} {e['sprite']:22s} rect=({rw:6.1f}x{rh:5.1f}) mean256={hr.mean()/255:.3f} diff64_mean={diff.mean():.2f}")

        tile = np.full((N + 8, N + 8), 40, dtype=np.uint8)
        tile[4:-4, 4:-4] = hr
        tiles.append(tile)

    row1 = np.concatenate(tiles[:10], axis=1)
    row2 = np.concatenate(tiles[10:20], axis=1)
    sheet = np.concatenate([row1, row2], axis=0)
    Image.fromarray(sheet).save(OUT_HR / "sprites_256_sheet.png")
    print(f"Extracted all {len(tiles)} 256x256 masks -> {OUT_V4} & {OUT_HR}")


if __name__ == "__main__":
    main()
