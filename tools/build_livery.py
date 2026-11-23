import argparse
import json
import math
from pathlib import Path

# Auto-load exact measured native sprite sizes from sprites_v4/sprites_meta.json (with fallback)
SPRITE_NATIVE_SIZE = {
    87:  (0.02450, 0.02440),
    88:  (0.02430, 0.02430),
    89:  (0.02450, 0.02440),
    90:  (0.02430, 0.02440),
    91:  (0.02430, 0.02100),
    95:  (0.02690, 0.02300),
    96:  (0.02670, 0.02350),
    97:  (0.03670, 0.02420),
    100: (0.02460, 0.02480),
    101: (0.01330, 0.02510),
    103: (0.02610, 0.02360),
}
_META_V4 = Path(__file__).resolve().parent / "geometrize_cli" / "sprites_v4" / "sprites_meta.json"
if _META_V4.exists():
    for _m in json.loads(_META_V4.read_text(encoding="utf-8")):
        SPRITE_NATIVE_SIZE[int(_m["index"])] = (float(_m["native_w"]), float(_m["native_h"]))

NATIVE_W = 0.02450
NATIVE_H = 0.02440


def parse_color(val) -> tuple[int, int, int, int]:
    if isinstance(val, (list, tuple)):
        if len(val) == 3:
            return (int(val[0]), int(val[1]), int(val[2]), 255)
        return (int(val[0]), int(val[1]), int(val[2]), int(val[3]))
    s = str(val).strip().lstrip("#")
    if len(s) == 6:
        return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16), 255)
    if len(s) == 8:
        return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16), int(s[6:8], 16))
    raise ValueError(f"Unsupported color format: {val}")


def pack_color(r: int, g: int, b: int, a: int) -> int:
    return ((a & 0xFF) << 24) | ((b & 0xFF) << 16) | ((g & 0xFF) << 8) | (r & 0xFF)


def build_from_config(cfg_path: Path, out_override: Path | None = None) -> Path:
    cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
    max_total = int(cfg.get("max_layers", 300))
    x_angle = int(cfg.get("x_angle", 151))
    y_angle = int(cfg.get("y_angle", 0))
    default_icon = tuple(float(v) for v in cfg.get("icon_pos", [0.0, 0.72, 1.10]))
    out_path = out_override or Path(cfg.get("output", "work/livery_layers.txt"))

    # (px, py, sx, sy, rot, color, sticker, xAngle, yAngle, ix, iy, iz, isText, italic, bold, flipX, fontIdx, text)
    layers: list[tuple] = []

    if cfg.get("global_bg"):
        r, g, b, a = parse_color(cfg["global_bg"])
        layers.append((0.0, 0.0, 1.02 / NATIVE_W, 1.02 / NATIVE_H, 0.0, pack_color(r, g, b, a), 87, x_angle, y_angle, *default_icon, 0, 0, 0, 0, 0, ""))

    for name, panel in cfg.get("panels", {}).items():
        if not panel.get("enabled", True):
            continue
        u0, u1 = panel["u"]
        v0, v1 = panel["v"]
        r, g, b, a = parse_color(panel["color"])
        px = (u0 + u1) * 0.5 - 0.5
        py = (v0 + v1) * 0.5 - 0.5
        uw = abs(u1 - u0)
        uh = abs(v1 - v0)
        rot = float(panel.get("rotation", 0.0)) % 360.0
        p_icon = tuple(float(v) for v in panel.get("icon_pos", default_icon))
        layers.append((px, py, uw / NATIVE_W, uh / NATIVE_H, rot, pack_color(r, g, b, a), 87, x_angle, y_angle, *p_icon, 0, 0, 0, 0, 0, ""))

    for art in cfg.get("artworks", []):
        if not art.get("enabled", True):
            continue
        remaining = max_total - len(layers)
        if remaining <= 0:
            break
        art_cap = min(remaining, int(art.get("max_shapes", remaining)))

        raw = json.loads(Path(art["shapes"]).read_text(encoding="utf-8"))
        shapes = raw["shapes"] if isinstance(raw, dict) else raw
        if "src_size" in art:
            src_w, src_h = [float(x) for x in art["src_size"]]
        elif isinstance(raw, dict) and "width" in raw and "height" in raw:
            src_w, src_h = float(raw["width"]), float(raw["height"])
        else:
            sp = Path(art["shapes"])
            meta_p = sp.with_name(sp.name.replace("_shapes.json", "_meta.json"))
            if meta_p.exists():
                meta = json.loads(meta_p.read_text(encoding="utf-8"))
                src_w, src_h = [float(x) for x in meta.get("src_size", [400, 400])]
            else:
                src_w, src_h = 400.0, 400.0

        if "center_uv" in art:
            off_x = float(art["center_uv"][0]) - 0.5
            off_y = float(art["center_uv"][1]) - 0.5
        else:
            off_x = float(art.get("offset_x", 0.0))
            off_y = float(art.get("offset_y", 0.0))

        aspect = src_w / src_h if src_h > 0 else 1.0
        if "scale_x" in art and "scale_y" in art:
            scale_x = float(art["scale_x"])
            scale_y = float(art["scale_y"])
        elif "height" in art:
            scale_y = float(art["height"])
            scale_x = scale_y * aspect
        elif "width" in art:
            scale_x = float(art["width"])
            scale_y = scale_x / aspect
        else:
            base_scale = float(art.get("scale", 0.20))
            if aspect >= 1.0:
                scale_x = base_scale
                scale_y = base_scale / aspect
            else:
                scale_y = base_scale
                scale_x = base_scale * aspect

        canvas_rot = float(art.get("rotation", 0.0))
        flip_x = bool(art.get("flip_x", False))
        flip_y = bool(art.get("flip_y", False))
        rot_sign = float(art.get("rot_sign", -1.0))
        art_xa = int(art.get("x_angle", x_angle))
        art_ya = int(art.get("y_angle", y_angle))
        art_icon = tuple(float(v) for v in art.get("icon_pos", default_icon))

        tint = parse_color(art["bg_tint"]) if art.get("bg_tint") else None
        filter_luma = art.get("filter_luma", None)

        rad = math.radians(canvas_rot)
        cos_a, sin_a = math.cos(rad), math.sin(rad)
        added = 0

        for sh in shapes:
            if added >= art_cap:
                break
            r, g, b, a = sh["color"]
            luma = 0.299 * r + 0.587 * g + 0.114 * b
            if filter_luma is not None and luma > float(filter_luma):
                continue

            sh_flip = bool(sh.get("flipX", False))
            if "sticker" in sh:
                sticker = int(sh["sticker"])
                cx = float(sh["cx"])
                cy = float(sh["cy"])
                w = float(sh["w"])
                h = float(sh["h"])
                ang = float(sh["angle"])
            else:
                t = sh["type"]
                data = sh["data"]
                if t == 2:
                    x1, y1, x2, y2, ang = data
                    cx = (x1 + x2) * 0.5
                    cy = (y1 + y2) * 0.5
                    w = abs(x2 - x1)
                    h = abs(y2 - y1)
                    sticker = 87
                elif t == 16:
                    cx, cy, rx, ry, ang = data
                    w = abs(rx) * 2.0
                    h = abs(ry) * 2.0
                    sticker = 89
                else:
                    continue

            if w <= 0 or h <= 0:
                continue

            nx = (cx / src_w) - 0.5
            ny = 0.5 - (cy / src_h)
            if flip_x:
                nx = -nx
            if flip_y:
                ny = -ny
            eff_rot_sign = -rot_sign if (flip_x ^ flip_y) else rot_sign

            lx = nx * scale_x
            ly = ny * scale_y
            px = (lx * cos_a - ly * sin_a) + off_x
            py = (lx * sin_a + ly * cos_a) + off_y

            uw = (w / src_w) * scale_x
            uh = (h / src_h) * scale_y
            rot = (eff_rot_sign * ang + canvas_rot) % 360.0

            if tint is not None:
                tr, tg, tb, _ = tint
                r = int(round(r * tr / 255.0))
                g = int(round(g * tg / 255.0))
                b = int(round(b * tb / 255.0))

            nw, nh = SPRITE_NATIVE_SIZE.get(sticker, (NATIVE_W, NATIVE_H))
            sx = uw / nw
            sy = uh / nh
            # Mirror horizontally cleanly via negative localScale.x (verified on 320 official preset layers!)
            if sh_flip ^ flip_x:
                sx = -sx

            layers.append((px, py, sx, sy, rot, pack_color(r, g, b, a), sticker, art_xa, art_ya, *art_icon, 0, 0, 0, 0, 0, ""))
            added += 1

    # Optional direct text layers (e.g. JDM banner / plate text / custom glyphs)
    for txt in cfg.get("texts", []):
        if not txt.get("enabled", True) or len(layers) >= max_total:
            continue
        u, v = txt["center_uv"]
        r, g, b, a = parse_color(txt.get("color", "#FFFFFF"))
        t_icon = tuple(float(x) for x in txt.get("icon_pos", default_icon))
        layers.append((
            float(u) - 0.5, float(v) - 0.5,
            float(txt.get("scale_x", 2.0)), float(txt.get("scale_y", 2.0)),
            float(txt.get("rotation", 0.0)) % 360.0,
            pack_color(r, g, b, a),
            1,  # stickerIndex=1 (1_text)
            int(txt.get("x_angle", x_angle)), int(txt.get("y_angle", y_angle)),
            *t_icon,
            1, int(bool(txt.get("italic", False))), int(bool(txt.get("bold", False))),
            int(bool(txt.get("flip_x", False))), int(txt.get("font_index", 0)),
            str(txt.get("text", "")),
        ))

    lines = ["# px py order sx sy rot color sticker xAngle yAngle ix iy iz [|isText italic bold flipX fontIndex|text]"]
    for order, row in enumerate(layers[:max_total]):
        px, py, sx, sy, rot, col, sticker, xa, ya, ix, iy, iz, isT, itl, bld, flp, fnt, tstr = row
        base = f"{px:.6f} {py:.6f} {order} {sx:.6f} {sy:.6f} {rot:.3f} {col} {sticker} {xa} {ya} {ix:.4f} {iy:.4f} {iz:.4f}"
        if isT or tstr:
            base += f" |{isT} {itl} {bld} {flp} {fnt}|{tstr}"
        lines.append(base)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Built {len(layers[:max_total])} layers -> {out_path}")
    return out_path


def main():
    ap = argparse.ArgumentParser(description="Build CPM2 vinyl layers file from a JSON livery config.")
    ap.add_argument("config", help="Path to JSON config")
    ap.add_argument("--output", default=None, help="Override output layers file path")
    args = ap.parse_args()
    build_from_config(Path(args.config), Path(args.output) if args.output else None)


if __name__ == "__main__":
    main()
