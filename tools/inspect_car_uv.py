import argparse
import json
from pathlib import Path
from PIL import Image, ImageDraw
import UnityPy
from export_car_3d import resolve_bundle_dir

BUNDLE_DIR = None

SKIP_WORDS = (
    "interior", "glass", "window", "light", "engine", "wheel", "tire", "brake",
    "bolt", "taho", "speedo", "steer", "seat", "dash", "shadow", "lod", "l1_",
    "l2_", "l_", "black", "plastic", "silver", "chrome", "carbon", "caliper",
    "exhaust", "ex4", "grille", "grill", "mesh", "logo", "emblem", "plate",
    "wiper", "under", "frame", "chassis", "radiator", "pipe", "muffler", "mirror"
)

PALETTE = [
    (255, 80, 80),    # red
    (80, 220, 120),   # green
    (80, 170, 255),   # blue
    (255, 190, 60),   # orange/gold
    (210, 110, 255),  # purple
    (60, 230, 230),   # cyan
    (255, 255, 90),   # yellow
    (255, 120, 200),  # pink
    (180, 180, 180),  # light gray
    (150, 255, 150),  # mint
]


def parse_obj(obj_txt: str):
    verts, uvs, faces = [], [], []
    for line in obj_txt.splitlines():
        if line.startswith("v "):
            verts.append(tuple(map(float, line.split()[1:4])))
        elif line.startswith("vt "):
            uvs.append(tuple(map(float, line.split()[1:3])))
        elif line.startswith("f "):
            f = []
            for tok in line.split()[1:]:
                p = tok.split("/")
                vi = int(p[0]) - 1
                ti = int(p[1]) - 1 if len(p) > 1 and p[1] else vi
                f.append((vi, ti))
            faces.append(f)
    return verts, uvs, faces


def main():
    ap = argparse.ArgumentParser(description="Inspect body or window mesh UV panels for any CPM2 carId.")
    ap.add_argument("car_id", type=int)
    ap.add_argument("--all", action="store_true", help="List all meshes in bundle")
    ap.add_argument("--windows", action="store_true", help="Inspect window/glass meshes instead of painted body")
    ap.add_argument("--out", default=None, help="Output UV map PNG path")
    args = ap.parse_args()

    bdir = resolve_bundle_dir()
    bundle = bdir / f"car{args.car_id}_assets_all.bundle"
    if not bundle.exists():
        raise FileNotFoundError(f"Bundle not found: {bundle}")

    env = UnityPy.load(str(bundle))
    img = Image.new("RGB", (1024, 1024), (18, 18, 22))
    dr = ImageDraw.Draw(img)

    # Draw subtle 0.1 UV grid
    for g in range(1, 10):
        pos = int(g * 102.4)
        dr.line([(pos, 0), (pos, 1024)], fill=(38, 38, 46), width=1)
        dr.line([(0, pos), (1024, pos)], fill=(38, 38, 46), width=1)

    meshes = []
    for obj in env.objects:
        if obj.type.name != "Mesh":
            continue
        m = obj.read()
        name = m.m_Name
        low = name.lower()
        if args.windows:
            if not any(w in low for w in ("window", "glass", "windshield")):
                continue
        elif not args.all and any(w in low for w in SKIP_WORDS):
            continue
        verts, uvs, faces = parse_obj(m.export())
        if not uvs or not faces:
            continue
        # Filter triangles whose UVs fall inside [0, 1] x [0, 1] (painted body UV space)
        in_faces = []
        in_uvs = []
        in_verts = []
        for f in faces:
            ok = True
            for vi, ti in f:
                if ti >= len(uvs):
                    ok = False; break
                u, v = uvs[ti]
                if not (-0.01 <= u <= 1.01 and -0.01 <= v <= 1.01):
                    ok = False; break
            if ok:
                in_faces.append(f)
                for vi, ti in f:
                    in_uvs.append(uvs[ti])
                    if vi < len(verts):
                        in_verts.append(verts[vi])
        if len(in_faces) < 20:
            continue
        us = [u for u, _ in in_uvs]; vs = [v for _, v in in_uvs]
        xs = [p[0] for p in in_verts]; ys = [p[1] for p in in_verts]; zs = [p[2] for p in in_verts]
        du = max(us) - min(us); dv = max(vs) - min(vs)
        if du < 0.03 and dv < 0.03:
            continue
        meshes.append({
            "name": name, "faces": in_faces, "uvs": uvs, "verts": verts,
            "n_faces": len(in_faces),
            "u": (min(us), max(us)), "v": (min(vs), max(vs)),
            "x": (min(xs), max(xs)), "y": (min(ys), max(ys)), "z": (min(zs), max(zs)),
        })

    # Sort by face count descending and keep top distinct panels
    meshes.sort(key=lambda d: d["n_faces"], reverse=True)
    summary = []
    for idx, d in enumerate(meshes):
        col = PALETTE[idx % len(PALETTE)]
        u0, u1 = d["u"]; v0, v1 = d["v"]
        x0, x1 = d["x"]; y0, y1 = d["y"]; z0, z1 = d["z"]
        print(
            f"[{idx:2d}] {d['name']:24s} col={col} faces={d['n_faces']:4d} "
            f"U=[{u0:.3f},{u1:.3f}] V=[{v0:.3f},{v1:.3f}] "
            f"3D X=[{x0:+.2f},{x1:+.2f}] Y=[{y0:+.2f},{y1:+.2f}] Z=[{z0:+.2f},{z1:+.2f}]"
        )
        summary.append({
            "idx": idx, "name": d["name"], "color": col, "faces": d["n_faces"],
            "u": [round(u0, 4), round(u1, 4)], "v": [round(v0, 4), round(v1, 4)],
            "x": [round(x0, 3), round(x1, 3)], "y": [round(y0, 3), round(y1, 3)], "z": [round(z0, 3), round(z1, 3)],
        })
        if idx < 16:
            for f in d["faces"]:
                pts = [(d["uvs"][ti][0] * 1024, (1.0 - d["uvs"][ti][1]) * 1024) for _, ti in f]
                for k in range(len(pts)):
                    dr.line([pts[k], pts[(k + 1) % len(pts)]], fill=col, width=1)

    default_name = f"work/car{args.car_id}_windows_uv_map.png" if args.windows else f"work/car{args.car_id}_uv_map.png"
    out_png = Path(args.out or default_name)
    out_png.parent.mkdir(parents=True, exist_ok=True)
    img.save(out_png)
    out_json = out_png.with_suffix(".json")
    out_json.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"Saved {out_png} and {out_json} ({len(summary)} meshes)")


if __name__ == "__main__":
    main()
