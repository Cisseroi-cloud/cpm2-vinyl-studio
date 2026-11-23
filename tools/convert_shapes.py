import argparse
import json
import math
from pathlib import Path

NATIVE_W = 0.0245
NATIVE_H = 0.0244


def pack_color(r: int, g: int, b: int, a: int) -> int:
    return ((a & 0xFF) << 24) | ((b & 0xFF) << 16) | ((g & 0xFF) << 8) | (r & 0xFF)


def main() -> None:
    ap = argparse.ArgumentParser(description="Convert Geometrize shapes.json to CPM2 injector layers file.")
    ap.add_argument("--input", default="work/shapes.json")
    ap.add_argument("--output", default="work/shapes_layers.txt")
    ap.add_argument("--width", type=float, default=600.0)
    ap.add_argument("--height", type=float, default=600.0)
    ap.add_argument("--region-scale", type=float, default=1.0, help="Canvas fraction covered by image (default 1.0)")
    ap.add_argument("--offset-x", type=float, default=0.0, help="Center X in CPM2 [-0.5, 0.5] (= U - 0.5)")
    ap.add_argument("--offset-y", type=float, default=0.0, help="Center Y in CPM2 [-0.5, 0.5] (= V - 0.5)")
    ap.add_argument("--canvas-rot", type=float, default=0.0, help="CCW rotation of entire image in degrees (e.g. -90 for CW 90 on car13 hood)")
    ap.add_argument("--flip-y", action="store_true", help="Use cy/H - 0.5 instead of 0.5 - cy/H")
    ap.add_argument("--rot-sign", type=float, default=-1.0, help="Multiplier for Geometrize angle (default -1.0)")
    ap.add_argument("--reverse-order", action="store_true", help="Reverse layer z order")
    ap.add_argument("--x-angle", type=int, default=151)
    ap.add_argument("--y-angle", type=int, default=0)
    ap.add_argument("--filter-luma", type=float, default=None, help="Drop shapes with luminance > threshold (0-255)")
    ap.add_argument("--bg-color", default=None, help="Optional R,G,B,A solid background square at order 0")
    ap.add_argument("--bg-rect", default=None, help="Optional cx,cy,w,h,R,G,B,A background rect in CPM2 coords at order 0")
    ap.add_argument("--clip-white-box", default=None, help="Optional umin,umax,vmin,vmax to keep near-white shapes inside panel")
    ap.add_argument("--max-layers", type=int, default=300)
    args = ap.parse_args()

    raw = json.loads(Path(args.input).read_text(encoding="utf-8"))
    shapes = raw["shapes"] if isinstance(raw, dict) else raw

    layers = []
    if args.bg_rect:
        parts = [float(v) for v in args.bg_rect.split(",")]
        bx, by, bw, bh = parts[0], parts[1], parts[2], parts[3]
        rgba = [int(v) for v in parts[4:8]]
        if len(rgba) == 3:
            rgba.append(255)
        layers.append((bx, by, bw / NATIVE_W, bh / NATIVE_H, 0.0, pack_color(*rgba), 87))
    elif args.bg_color:
        rgba = [int(v) for v in args.bg_color.split(",")]
        if len(rgba) == 3:
            rgba.append(255)
        sx = args.region_scale / NATIVE_W
        sy = args.region_scale / NATIVE_H
        layers.append((args.offset_x, args.offset_y, sx, sy, args.canvas_rot % 360.0, pack_color(*rgba), 87))

    rad = math.radians(args.canvas_rot)
    cos_a = math.cos(rad)
    sin_a = math.sin(rad)

    clip_box = [float(v) for v in args.clip_white_box.split(",")] if args.clip_white_box else None

    for sh in shapes:
        t = sh["type"]
        data = sh["data"]
        r, g, b, a = sh["color"]
        luma = 0.299 * r + 0.587 * g + 0.114 * b
        if args.filter_luma is not None and luma > args.filter_luma:
            continue

        if t == 2:  # rotated rectangle: [x1, y1, x2, y2, angle]
            x1, y1, x2, y2, ang = data
            cx = (x1 + x2) * 0.5
            cy = (y1 + y2) * 0.5
            w = abs(x2 - x1)
            h = abs(y2 - y1)
            sticker = 87
        elif t == 16:  # rotated ellipse: [cx, cy, rx, ry, angle]
            cx, cy, rx, ry, ang = data
            w = abs(rx) * 2.0
            h = abs(ry) * 2.0
            sticker = 89
        else:
            continue

        if w <= 0 or h <= 0:
            continue

        lx = ((cx / args.width) - 0.5) * args.region_scale
        ly_base = ((cy / args.height) - 0.5) if args.flip_y else (0.5 - (cy / args.height))
        ly = ly_base * args.region_scale
        rx_pos = lx * cos_a - ly * sin_a
        ry_pos = lx * sin_a + ly * cos_a
        px = rx_pos + args.offset_x
        py = ry_pos + args.offset_y
        uw = (w / args.width) * args.region_scale
        uh = (h / args.height) * args.region_scale
        rot = (args.rot_sign * ang + args.canvas_rot) % 360.0

        # Prevent oversized near-white background shapes from spilling onto adjacent car panels
        if clip_box is not None and luma > 240:
            umin, umax, vmin, vmax = clip_box
            u, v = px + 0.5, py + 0.5
            rad_max = max(0.002, min(u - umin, umax - u, v - vmin, vmax - v))
            cur_r = 0.5 * math.hypot(uw, uh)
            if cur_r > rad_max:
                scale_down = rad_max / cur_r
                uw *= scale_down
                uh *= scale_down

        sx = uw / NATIVE_W
        sy = uh / NATIVE_H
        col = pack_color(r, g, b, a)
        layers.append((px, py, sx, sy, rot, col, sticker))
        if len(layers) >= args.max_layers:
            break

    n = len(layers)
    lines = ["# px py order sx sy rot color sticker xAngle yAngle"]
    for i, (px, py, sx, sy, rot, col, sticker) in enumerate(layers):
        order = (n - 1 - i) if args.reverse_order else i
        lines.append(
            f"{px:.6f} {py:.6f} {order} {sx:.6f} {sy:.6f} {rot:.3f} {col} {sticker} {args.x_angle} {args.y_angle}"
        )

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote {n} layers -> {out}")


if __name__ == "__main__":
    main()
