"""
CPM2 Full 3D Vehicle Exporter (.cpmcar binary format)
Extracts complete 3D vehicle geometry from Unity Addressables bundles (car<ID>_assets_all.bundle):
  - Full Unity Transform hierarchy (m_LocalPosition, m_LocalRotation, m_LocalScale)
  - Deterministic material classification:
      0 = BODY    (CarVinyls 3000x3000 UV atlas)
      1 = WINDOWS (WindowVinyls 3000x3000 UV atlas - excludes headlight glass)
      2 = PLATE   (PlateVinyls 2:1 synthesized quads anchored at PlatePos)
      3 = STATIC  (chassis, trim, lights, interior, exhausts, etc.)
  - Automatic bodykit / tuning variant grouping (hood0..N, fbumper0..N, rbumper0..N, fender0..N, skirts0..N, etc.)
  - Wheel mount points (for procedural tire/rim rendering)
  - Native Unity Left-Handed coordinates (matches DirectX 11 LH and CPM2 iconPosition 1:1)
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import struct
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np
import UnityPy
from UnityPy.export.MeshExporter import MeshHandler

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CACHE_DIR = REPO_ROOT / "cache" / "cars"
SUBPATH_BUNDLES = Path("Car Parking 2_Data") / "StreamingAssets" / "aa" / "StandaloneWindows64"


def _find_steam_libraries() -> List[Path]:
    libs: List[Path] = []
    steam_roots: List[Path] = []
    try:
        import winreg
        for hive, subkey, val in (
            (winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
            (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
            (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", "InstallPath"),
        ):
            try:
                with winreg.OpenKey(hive, subkey) as k:
                    p, _ = winreg.QueryValueEx(k, val)
                    if p:
                        steam_roots.append(Path(p))
            except OSError:
                pass
    except ImportError:
        pass

    for sr in steam_roots:
        libs.append(sr)
        vdf = sr / "steamapps" / "libraryfolders.vdf"
        if vdf.exists():
            try:
                txt = vdf.read_text(encoding="utf-8", errors="ignore")
                for m in re.finditer(r'"path"\s+"([^"]+)"', txt):
                    raw = m.group(1).replace("\\\\", "\\")
                    libs.append(Path(raw))
            except OSError:
                pass

    # Fallback common Steam library drives
    for drv in ("C", "D", "E", "F", "G"):
        libs.append(Path(f"{drv}:\\Program Files (x86)\\Steam"))
        libs.append(Path(f"{drv}:\\SteamLibrary"))
    return libs


def resolve_bundle_dir(explicit_bundle_dir: Optional[str] = None, explicit_game_dir: Optional[str] = None) -> Path:
    candidates: List[Path] = []
    if explicit_bundle_dir:
        candidates.append(Path(explicit_bundle_dir))
    if explicit_game_dir:
        candidates.append(Path(explicit_game_dir) / SUBPATH_BUNDLES)
    env_game = os.environ.get("CPM2_GAME_DIR")
    if env_game:
        candidates.append(Path(env_game) / SUBPATH_BUNDLES)

    # Check studio_config.json saved by the GUI
    cfg_path = REPO_ROOT / "studio_config.json"
    if cfg_path.exists():
        try:
            cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
            gd = cfg.get("gameDir") or cfg.get("game_dir")
            if gd:
                candidates.append(Path(gd) / SUBPATH_BUNDLES)
        except Exception:
            pass

    # Check sibling folder ../CPM2
    candidates.append(REPO_ROOT.parent / "CPM2" / SUBPATH_BUNDLES)

    # Check all discovered Steam libraries
    for lib in _find_steam_libraries():
        candidates.append(lib / "steamapps" / "common" / "CPM2" / SUBPATH_BUNDLES)

    for c in candidates:
        if c.exists() and any(c.glob("car*_assets_all.bundle")):
            return c
    raise FileNotFoundError(
        "Dossier du jeu CPM 2 introuvable. Definissez le dossier du jeu dans CPM2 Vinyl Studio "
        "(Menu Projet -> Dossier du jeu CPM 2) ou passez --game-dir <chemin>."
    )

SURF_BODY = 0
SURF_WINDOWS = 1
SURF_PLATE = 2
SURF_STATIC = 3

# Known shared Unity Material path_ids across CPM2 bundles
KNOWN_WINDOW_MAT_PID = 5635101494793351896
KNOWN_PAINT_MAT_PIDS = {-3246567781437039885}

KIT_ROOT_RE = re.compile(
    r"^(hood|fbumper|rbumper|fender|skirts|skirt|spoiler|wing|roof|diffuser|bonnet|trunk|widebody|exhaust|muffter|muffler)(\d+)$",
    re.IGNORECASE,
)

SKIP_ROOT_SUBSTR = ("_lod", "shadow", "collider")
SKIP_NODE_SUBSTR = ("shadowdecal", "collider", "caranimationspoints", "fsopoints", "roo bar point", "_point")

BODY_NAME_HINTS = (
    "body", "hood", "fbumper", "rbumper", "fender", "skirt", "door", "trunk",
    "spoiler", "wing", "roof", "mirror", "bonnet", "bumper", "lightbody", "widebody", "diffuser",
)
NON_BODY_HINTS = (
    "plastic", "black", "silver", "chrome", "light", "turn", "reverse", "brake",
    "glass", "window", "muffter", "muffler", "exhaust", "engine", "interior", "interier",
    "carpet", "leather", "seat", "speedo", "tacho", "steer", "key", "wheel", "tire",
    "caliper", "grille", "grill", "logo", "emblem", "wiper", "pipe", "radiator",
)


def quat_to_mat3(qx: float, qy: float, qz: float, qw: float) -> np.ndarray:
    n = math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw)
    if n < 1e-12:
        return np.eye(3, dtype=np.float64)
    x, y, z, w = qx / n, qy / n, qz / n, qw / n
    return np.array([
        [1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w),       2.0 * (x * z + y * w)],
        [2.0 * (x * y + z * w),       1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - x * w)],
        [2.0 * (x * z - y * w),       2.0 * (y * z + x * w),       1.0 - 2.0 * (x * x + y * y)],
    ], dtype=np.float64)


def trs_matrix(tr) -> np.ndarray:
    lp = tr.m_LocalPosition
    lr = tr.m_LocalRotation
    ls = tr.m_LocalScale
    R = quat_to_mat3(lr.x, lr.y, lr.z, lr.w)
    S = np.diag([ls.x, ls.y, ls.z])
    M = np.eye(4, dtype=np.float64)
    M[:3, :3] = R @ S
    M[:3, 3] = [lp.x, lp.y, lp.z]
    return M


def get_mat_tex_props(mat) -> List[str]:
    try:
        te = mat.m_SavedProperties.m_TexEnvs
        return [k for k, _ in te] if isinstance(te, list) else list(te.keys())
    except Exception:
        return []


def get_mat_color(mat) -> Tuple[float, float, float, float]:
    try:
        cols = mat.m_SavedProperties.m_Colors
        items = cols if isinstance(cols, list) else list(cols.items())
        for key in ("_BaseColor", "_Color", "_TintColor"):
            for k, v in items:
                if k == key:
                    return (float(v.r), float(v.g), float(v.b), float(v.a))
    except Exception:
        pass
    name = (getattr(mat, "m_Name", "") or "").lower()
    if "black" in name or "plastic" in name or "rubber" in name or "tire" in name:
        return (0.10, 0.10, 0.11, 1.0)
    if "silver" in name or "chrome" in name:
        return (0.72, 0.74, 0.77, 1.0)
    if "light" in name:
        return (0.85, 0.85, 0.88, 1.0)
    if "glass" in name or "window" in name:
        return (0.06, 0.08, 0.10, 0.58)
    if "carpet" in name or "leather" in name or "interior" in name:
        return (0.16, 0.16, 0.18, 1.0)
    return (0.35, 0.36, 0.38, 1.0)


def classify_surface(
    go_name: str,
    mesh_name: str,
    mat_ptr,
    local_mats: Dict[int, dict],
    uvs: np.ndarray,
    is_interior: bool,
) -> Tuple[int, Tuple[float, float, float, float], float, float]:
    """Returns (surfaceType, baseColorRGBA, roughness, metallic)."""
    if is_interior:
        col = (0.14, 0.14, 0.16, 1.0)
        if mat_ptr and mat_ptr.file_id == 0 and mat_ptr.path_id in local_mats:
            col = local_mats[mat_ptr.path_id]["color"]
        return SURF_STATIC, col, 0.85, 0.05

    g_low = go_name.lower()
    m_low = mesh_name.lower()

    if mat_ptr is not None:
        fid, pid = mat_ptr.file_id, mat_ptr.path_id
        # 1. Local material lookup
        if fid == 0 and pid in local_mats:
            info = local_mats[pid]
            mname = info["name"].lower()
            tex = info["tex"]
            # Vinylable window glass (specifically "glass window" or known pid)
            if pid == KNOWN_WINDOW_MAT_PID or mname == "glass window":
                return SURF_WINDOWS, (0.07, 0.09, 0.12, 0.55), 0.06, 0.10
            # Local CarPaint material (has _Flakes or _IridescneLUT)
            if "_Flakes" in tex or "_IridescneLUT" in tex:
                return SURF_BODY, (0.77, 0.09, 0.11, 1.0), 0.18, 0.35
            # Headlight/other glass ("New Material" with _CracksNormal)
            if "_CracksNormal" in tex:
                return SURF_STATIC, (0.72, 0.78, 0.85, 0.35), 0.08, 0.10
            col = info["color"]
            metallic = 0.75 if ("silver" in mname or "chrome" in mname) else 0.10
            rough = 0.25 if metallic > 0.5 else 0.65
            return SURF_STATIC, col, rough, metallic

        # 2. External shared material
        if fid != 0:
            if pid in KNOWN_PAINT_MAT_PIDS:
                return SURF_BODY, (0.77, 0.09, 0.11, 1.0), 0.18, 0.35
            # External material on a body panel whose UVs live in [0, 1]
            if any(h in g_low or h in m_low for h in BODY_NAME_HINTS) and not any(
                b in g_low for b in NON_BODY_HINTS
            ):
                if len(uvs) > 0:
                    in01 = (uvs[:, 0] >= -0.02) & (uvs[:, 0] <= 1.02) & (uvs[:, 1] >= -0.02) & (uvs[:, 1] <= 1.02)
                    if in01.mean() > 0.85:
                        du = float(uvs[:, 0].max() - uvs[:, 0].min())
                        dv = float(uvs[:, 1].max() - uvs[:, 1].min())
                        if du > 0.02 or dv > 0.02:
                            return SURF_BODY, (0.77, 0.09, 0.11, 1.0), 0.18, 0.35
            # External chrome/plastic trim fallback
            if "silver" in g_low or "muffter" in g_low or "exhaust" in g_low:
                return SURF_STATIC, (0.72, 0.74, 0.78, 1.0), 0.22, 0.85
            if "plastic" in g_low or "black" in g_low:
                return SURF_STATIC, (0.11, 0.11, 0.12, 1.0), 0.72, 0.05

    return SURF_STATIC, (0.32, 0.33, 0.35, 1.0), 0.60, 0.15


def compute_smooth_normals(verts: np.ndarray, indices: np.ndarray) -> np.ndarray:
    norms = np.zeros_like(verts, dtype=np.float64)
    tris = indices.reshape(-1, 3)
    v0 = verts[tris[:, 0]]
    v1 = verts[tris[:, 1]]
    v2 = verts[tris[:, 2]]
    # Unity LH clockwise front face: cross(v1-v0, v2-v0) points outward
    fn = np.cross(v1 - v0, v2 - v0)
    np.add.at(norms, tris[:, 0], fn)
    np.add.at(norms, tris[:, 1], fn)
    np.add.at(norms, tris[:, 2], fn)
    lens = np.linalg.norm(norms, axis=1, keepdims=True)
    norms = np.where(lens > 1e-9, norms / np.maximum(lens, 1e-9), np.array([0.0, 1.0, 0.0]))
    return norms.astype(np.float32)


def make_plate_quad(world_mat: np.ndarray, is_front: bool) -> Tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Synthesize an exact 0.3300m x 0.1662m CPM2 license plate quad at PlatePos transform."""
    hw = 0.3300 * 0.5
    hh = 0.1662 * 0.5
    # PlatePos in Unity typically has its local Y or Z pointing outward; check world orientation
    # Local quad in XZ or XY: inspect which axis points most along +/-Z world
    ax_x = world_mat[:3, 0]
    ax_y = world_mat[:3, 1]
    ax_z = world_mat[:3, 2]
    pos = world_mat[:3, 3]
    target_n = np.array([0.0, 0.0, 1.0 if is_front else -1.0], dtype=np.float64)

    # Pick the transform basis vector most aligned with target_n as normal
    axes = [(abs(np.dot(ax_y, target_n)), ax_y), (abs(np.dot(ax_z, target_n)), ax_z)]
    n = max(axes, key=lambda t: t[0])[1]
    n = n / max(np.linalg.norm(n), 1e-8)
    if np.dot(n, target_n) < 0:
        n = -n

    # Build orthonormal frame (right, up, normal) in LH space:
    # Viewing the plate head-on (looking along -n): +right is viewer's right, +up is viewer's up
    up_ref = np.array([0.0, 1.0, 0.0], dtype=np.float64)
    right = np.cross(n, up_ref)  # LH: n=(0,0,1), up=(0,1,0) -> right=(-1,0,0) which is viewer's right when facing front!
    if np.linalg.norm(right) < 1e-6:
        right = np.array([1.0, 0.0, 0.0], dtype=np.float64)
    right /= np.linalg.norm(right)
    up = np.cross(right, n)
    up /= np.linalg.norm(up)

    # Offset slightly outward (3mm) to prevent Z-fighting with bumper plate recess
    c = pos + n * 0.004
    pts = np.array([
        c - right * hw - up * hh,  # bottom-left  (u=0, v=0)
        c - right * hw + up * hh,  # top-left     (u=0, v=1)
        c + right * hw + up * hh,  # top-right    (u=1, v=1)
        c + right * hw - up * hh,  # bottom-right (u=1, v=0)
    ], dtype=np.float32)
    norms = np.tile(n.astype(np.float32), (4, 1))
    uvs = np.array([
        [0.0, 0.0],
        [0.0, 1.0],
        [1.0, 1.0],
        [1.0, 0.0],
    ], dtype=np.float32)
    # LH clockwise winding when viewed from +n:
    # 0(BL) -> 1(TL) -> 2(TR) and 0(BL) -> 2(TR) -> 3(BR)
    indices = np.array([0, 1, 2, 0, 2, 3], dtype=np.uint32)
    return pts, norms, uvs, indices


def export_car(car_id: int, out_path: Optional[Path] = None, bundle_dir: Optional[Path] = None) -> Path:
    if bundle_dir is None:
        bundle_dir = resolve_bundle_dir()
    bundle = bundle_dir / f"car{car_id}_assets_all.bundle"
    if not bundle.exists():
        raise FileNotFoundError(f"Bundle not found: {bundle}")

    env = UnityPy.load(str(bundle))

    gos, trs, mfs, mrs = {}, {}, {}, {}
    local_mats: Dict[int, dict] = {}
    for o in env.objects:
        t = o.type.name
        if t == "GameObject":
            gos[o.path_id] = o.read()
        elif t == "Transform":
            trs[o.path_id] = o.read()
        elif t == "MeshFilter":
            mfs[o.path_id] = o.read()
        elif t == "MeshRenderer":
            mrs[o.path_id] = o.read()
        elif t == "Material":
            m = o.read()
            local_mats[o.path_id] = {
                "name": m.m_Name,
                "tex": get_mat_tex_props(m),
                "color": get_mat_color(m),
            }

    go_tr = {tr.m_GameObject.path_id: tr for tr in trs.values()}
    go_mf = {mf.m_GameObject.path_id: mf for mf in mfs.values()}
    go_mr = {mr.m_GameObject.path_id: mr for mr in mrs.values()}

    # Build children map
    children: Dict[int, List[int]] = {}
    roots: List[int] = []
    for tr_pid, tr in trs.items():
        father = tr.m_Father.path_id
        if father == 0 or father not in trs:
            roots.append(tr_pid)
        else:
            children.setdefault(father, []).append(tr_pid)

    # Categorize root GameObjects
    body_root_tr = None
    chassis_root_tr = None
    interior_root_tr = None
    kit_roots: Dict[str, Dict[int, int]] = {}  # group_name -> {variant_idx: tr_pid}

    cid_str = str(car_id)
    for r_tr in roots:
        gid = trs[r_tr].m_GameObject.path_id
        name = gos[gid].m_Name
        low = name.lower()
        if any(s in low for s in SKIP_ROOT_SUBSTR):
            continue
        if low == f"b{cid_str}":
            body_root_tr = r_tr
        elif low == cid_str:
            chassis_root_tr = r_tr
        elif low == f"{cid_str}i":
            interior_root_tr = r_tr
        else:
            m = KIT_ROOT_RE.match(name)
            if m:
                grp = m.group(1).lower()
                if grp == "skirt":
                    grp = "skirts"
                if grp in ("muffter", "muffler"):
                    grp = "exhaust"
                var = int(m.group(2))
                kit_roots.setdefault(grp, {})[var] = r_tr

    # Estimate Y offset between chassis_root (<ID>) and body_root (b<ID>) / standalone kits
    # (e.g. on car187: 187/fbumper0 is at y=-0.112 while standalone fbumper0 is at y=0.0)
    chassis_y_fix = 0.0
    if chassis_root_tr is not None:
        diffs = []
        for c_tr in children.get(chassis_root_tr, []):
            cgid = trs[c_tr].m_GameObject.path_id
            cname = gos[cgid].m_Name
            m = KIT_ROOT_RE.match(cname)
            if m:
                grp = m.group(1).lower()
                if grp == "skirt":
                    grp = "skirts"
                var = int(m.group(2))
                if grp in kit_roots and var in kit_roots[grp]:
                    standalone_y = trs[kit_roots[grp][var]].m_LocalPosition.y
                    child_y = trs[c_tr].m_LocalPosition.y
                    diffs.append(standalone_y - child_y)
        if diffs:
            chassis_y_fix = float(np.median(diffs))

    # Build ordered kit groups list
    kit_groups_meta: List[dict] = []
    kit_group_index: Dict[str, int] = {}
    kit_variant_remap: Dict[Tuple[str, int], int] = {}
    for grp in sorted(kit_roots.keys()):
        vars_sorted = sorted(kit_roots[grp].keys())
        g_idx = len(kit_groups_meta)
        kit_group_index[grp] = g_idx
        for v_slot, orig_var in enumerate(vars_sorted):
            kit_variant_remap[(grp, orig_var)] = v_slot
        kit_groups_meta.append({
            "name": grp,
            "variants": vars_sorted,
            "count": len(vars_sorted),
            "default": 0,
        })

    submeshes: List[dict] = []
    wheels: List[dict] = []
    mesh_cache: Dict[int, Optional[Tuple[np.ndarray, np.ndarray, np.ndarray, List[np.ndarray]]]] = {}

    def load_mesh_data(mesh_pid: int, mesh_ptr):
        if mesh_pid in mesh_cache:
            return mesh_cache[mesh_pid]
        try:
            m = mesh_ptr.read()
            mh = MeshHandler(m)
            mh.process()
            if not mh.m_Vertices:
                mesh_cache[mesh_pid] = None
                return None
            verts = np.asarray(mh.m_Vertices, dtype=np.float64)
            if verts.ndim != 2 or verts.shape[1] < 3:
                mesh_cache[mesh_pid] = None
                return None
            verts = verts[:, :3]
            if mh.m_Normals and len(mh.m_Normals) == len(verts):
                norms = np.asarray(mh.m_Normals, dtype=np.float64)[:, :3]
            else:
                norms = np.zeros_like(verts)
            if mh.m_UV0 and len(mh.m_UV0) == len(verts):
                uvs = np.asarray(mh.m_UV0, dtype=np.float32)[:, :2]
            else:
                uvs = np.zeros((len(verts), 2), dtype=np.float32)
            tris_list = [np.asarray(st, dtype=np.uint32).reshape(-1) for st in mh.get_triangles()]
            mesh_cache[mesh_pid] = (m.m_Name, verts, norms, uvs, tris_list)
            return mesh_cache[mesh_pid]
        except Exception:
            mesh_cache[mesh_pid] = None
            return None

    def traverse(
        tr_pid: int,
        parent_mat: np.ndarray,
        kit_grp_idx: int,
        kit_var_idx: int,
        is_interior: bool,
        skip_kit_children: bool = False,
        path_prefix: str = "",
    ):
        tr = trs[tr_pid]
        gid = tr.m_GameObject.path_id
        go = gos[gid]
        name = go.m_Name
        low = name.lower()

        if any(s in low for s in SKIP_NODE_SUBSTR):
            return
        if "interier" in low or "interior" in low:
            is_interior = True

        # If traversing chassis root (<ID>), skip bodykit children already provided as standalone kit roots
        if skip_kit_children and path_prefix != "":
            m = KIT_ROOT_RE.match(name)
            if m:
                grp = m.group(1).lower()
                if grp == "skirt":
                    grp = "skirts"
                if grp in ("muffter", "muffler"):
                    grp = "exhaust"
                if grp in kit_group_index:
                    return

        world_mat = parent_mat @ trs_matrix(tr)
        cur_path = f"{path_prefix}/{name}" if path_prefix else name

        # Capture wheel mount points
        if low.startswith("wheel") and low in ("wheelfl", "wheelfr", "wheelrl", "wheelrr"):
            wp = world_mat[:3, 3]
            wheels.append({
                "name": name,
                "pos": [float(wp[0]), float(wp[1]), float(wp[2])],
            })

        # Capture PlatePos and synthesize PlateVinyls quad
        if low == "platepos":
            pos = world_mat[:3, 3]
            is_front = float(pos[2]) >= 0.0
            pts, pnorms, puvs, pidx = make_plate_quad(world_mat, is_front)
            submeshes.append({
                "name": f"{cur_path}_{'front' if is_front else 'rear'}",
                "surface": SURF_PLATE,
                "kit_group": kit_grp_idx,
                "kit_variant": kit_var_idx,
                "color": (0.95, 0.95, 0.96, 1.0),
                "roughness": 0.25,
                "metallic": 0.15,
                "verts": pts,
                "norms": pnorms,
                "uvs": puvs,
                "indices": pidx,
            })

        # Extract MeshFilter + MeshRenderer
        if gid in go_mf and gid in go_mr:
            mf = go_mf[gid]
            mr = go_mr[gid]
            mesh_pid = mf.m_Mesh.path_id
            if mesh_pid != 0:
                mdata = load_mesh_data(mesh_pid, mf.m_Mesh)
                if mdata is not None:
                    mesh_name, verts_local, norms_local, uvs, tris_list = mdata
                    # Transform positions
                    ones = np.ones((len(verts_local), 1), dtype=np.float64)
                    vh = np.hstack([verts_local, ones])
                    verts_w = (vh @ world_mat.T)[:, :3].astype(np.float32)

                    # Transform normals (inverse transpose of upper 3x3)
                    M3 = world_mat[:3, :3]
                    det = float(np.linalg.det(M3))
                    has_norms = np.linalg.norm(norms_local, axis=1).max() > 1e-5
                    if has_norms and abs(det) > 1e-10:
                        N3 = np.linalg.inv(M3).T
                        norms_w = norms_local @ N3.T
                        nl = np.linalg.norm(norms_w, axis=1, keepdims=True)
                        norms_w = (norms_w / np.maximum(nl, 1e-8)).astype(np.float32)
                    else:
                        norms_w = None

                    mats = list(mr.m_Materials)
                    for sub_i, idxs in enumerate(tris_list):
                        if len(idxs) < 3:
                            continue
                        # If negative determinant (mirrored scale), flip triangle winding
                        if det < 0.0:
                            idxs = idxs.reshape(-1, 3)[:, [0, 2, 1]].reshape(-1)
                        mat_ptr = mats[sub_i] if sub_i < len(mats) else (mats[0] if mats else None)
                        surf, col, rough, metal = classify_surface(
                            name, mesh_name, mat_ptr, local_mats, uvs, is_interior
                        )
                        snorms = norms_w if norms_w is not None else compute_smooth_normals(verts_w, idxs)
                        submeshes.append({
                            "name": f"{cur_path}" if len(tris_list) == 1 else f"{cur_path}[{sub_i}]",
                            "surface": surf,
                            "kit_group": kit_grp_idx,
                            "kit_variant": kit_var_idx,
                            "color": col,
                            "roughness": rough,
                            "metallic": metal,
                            "verts": verts_w,
                            "norms": snorms,
                            "uvs": uvs,
                            "indices": idxs,
                        })

        for c_tr in children.get(tr_pid, []):
            traverse(c_tr, world_mat, kit_grp_idx, kit_var_idx, is_interior, skip_kit_children, cur_path)

    I4 = np.eye(4, dtype=np.float64)
    if body_root_tr is not None:
        traverse(body_root_tr, I4, -1, -1, False, False, "")
    if interior_root_tr is not None:
        traverse(interior_root_tr, I4, -1, -1, True, False, "")
    for grp, vars_map in kit_roots.items():
        g_idx = kit_group_index[grp]
        for orig_var, r_tr in sorted(vars_map.items()):
            v_slot = kit_variant_remap[(grp, orig_var)]
            traverse(r_tr, I4, g_idx, v_slot, False, False, "")
    if chassis_root_tr is not None:
        # Apply Y alignment fix so wheels and any non-kit chassis children match b<ID> height
        M_fix = np.eye(4, dtype=np.float64)
        M_fix[1, 3] = chassis_y_fix
        traverse(chassis_root_tr, M_fix, -1, -1, False, True, "")

    if not submeshes:
        raise RuntimeError(f"No meshes extracted from {bundle}")

    # Compute overall bounding box over default configuration (base + variant 0)
    bmin = np.array([1e9, 1e9, 1e9], dtype=np.float64)
    bmax = np.array([-1e9, -1e9, -1e9], dtype=np.float64)
    for sm in submeshes:
        if sm["kit_group"] >= 0 and sm["kit_variant"] != 0:
            continue
        v = sm["verts"]
        bmin = np.minimum(bmin, v.min(axis=0))
        bmax = np.maximum(bmax, v.max(axis=0))

    # Estimate wheel radius nicely from wheel hub Y minus ground (bmin[1] - 0.14)
    for w in wheels:
        # Ground is slightly below lowest body skirt
        hub_y = w["pos"][1]
        est_r = max(0.24, min(0.42, hub_y - (float(bmin[1]) - 0.16)))
        w["radius"] = round(est_r, 3)
        w["width"] = 0.22

    # Write binary .cpmcar file
    if out_path is None:
        out_path = DEFAULT_CACHE_DIR / f"car{car_id}.cpmcar"
    out_path.parent.mkdir(parents=True, exist_ok=True)

    with out_path.open("wb") as f:
        f.write(b"CPMCAR01")
        f.write(struct.pack("<IIII", car_id, len(submeshes), len(kit_groups_meta), len(wheels)))
        f.write(struct.pack("<6f", *bmin.astype(np.float32), *bmax.astype(np.float32)))

        for kg in kit_groups_meta:
            name_b = kg["name"].encode("utf-8")[:31].ljust(32, b"\x00")
            f.write(name_b)
            f.write(struct.pack("<II", kg["count"], kg["default"]))

        for w in wheels:
            name_b = w["name"].encode("utf-8")[:31].ljust(32, b"\x00")
            f.write(name_b)
            f.write(struct.pack("<5f", w["pos"][0], w["pos"][1], w["pos"][2], w["radius"], w["width"]))

        for sm in submeshes:
            name_b = sm["name"].encode("utf-8")[:63].ljust(64, b"\x00")
            f.write(name_b)
            vcount = len(sm["verts"])
            icount = len(sm["indices"])
            r, g, b, a = sm["color"]
            f.write(struct.pack(
                "<Iii6fII",
                sm["surface"], sm["kit_group"], sm["kit_variant"],
                r, g, b, a, sm["roughness"], sm["metallic"],
                vcount, icount,
            ))
            # Interleave [px, py, pz, nx, ny, nz, u, v] float32
            interleaved = np.hstack([sm["verts"], sm["norms"], sm["uvs"]]).astype("<f4", copy=False)
            f.write(interleaved.tobytes())
            f.write(sm["indices"].astype("<u4", copy=False).tobytes())

    # Summary stats
    counts = {SURF_BODY: 0, SURF_WINDOWS: 0, SURF_PLATE: 0, SURF_STATIC: 0}
    tris = {SURF_BODY: 0, SURF_WINDOWS: 0, SURF_PLATE: 0, SURF_STATIC: 0}
    for sm in submeshes:
        counts[sm["surface"]] += 1
        tris[sm["surface"]] += len(sm["indices"]) // 3

    print(
        f"Exported car{car_id} -> {out_path} ({out_path.stat().st_size / 1024:.1f} KB) | "
        f"BODY={counts[0]} ({tris[0]} tris), WIN={counts[1]} ({tris[1]} tris), "
        f"PLATE={counts[2]}, STATIC={counts[3]} ({tris[3]} tris) | "
        f"kits={[(k['name'], k['count']) for k in kit_groups_meta]} | wheels={len(wheels)} | "
        f"y_fix={chassis_y_fix:+.3f}"
    )
    return out_path


def list_all_cars(bundle_dir: Optional[Path] = None) -> List[int]:
    if bundle_dir is None:
        bundle_dir = resolve_bundle_dir()
    ids = []
    for p in bundle_dir.glob("car*_assets_all.bundle"):
        m = re.match(r"^car(\d+)_assets_all\.bundle$", p.name)
        if m:
            ids.append(int(m.group(1)))
    ids.sort()
    return ids


def main():
    ap = argparse.ArgumentParser(description="Export CPM2 Unity car bundles into .cpmcar binary 3D scenes.")
    ap.add_argument("car_ids", nargs="*", type=int, help="One or more car IDs to export (e.g. 187 13)")
    ap.add_argument("--list", action="store_true", help="List all available car IDs in the game folder")
    ap.add_argument("--list-json", default=None, help="Write car ID catalog JSON to path")
    ap.add_argument("--detect-game", action="store_true", help="Print detected CPM2 game directory and exit")
    ap.add_argument("--out", default=None, help="Output .cpmcar file path (single carId only)")
    ap.add_argument("--game-dir", default=None, help="Path to CPM2 root install folder (containing Car Parking 2.exe)")
    ap.add_argument("--bundle-dir", default=None, help="Direct path to CPM2 StandaloneWindows64 bundles")
    args = ap.parse_args()

    bdir = resolve_bundle_dir(args.bundle_dir, args.game_dir)
    if args.detect_game:
        # bdir = <game>/Car Parking 2_Data/StreamingAssets/aa/StandaloneWindows64
        print(str(bdir.parents[3]))
        return
    if args.list or args.list_json:
        ids = list_all_cars(bdir)
        if args.list_json:
            Path(args.list_json).parent.mkdir(parents=True, exist_ok=True)
            Path(args.list_json).write_text(json.dumps({"car_ids": ids}, indent=2), encoding="utf-8")
        print(f"Found {len(ids)} cars: {ids[:20]} ... {ids[-10:]}")
        if not args.car_ids:
            return

    if not args.car_ids:
        ap.error("Provide at least one car_id or --list")

    for cid in args.car_ids:
        out = Path(args.out) if (args.out and len(args.car_ids) == 1) else None
        export_car(cid, out_path=out, bundle_dir=bdir)


if __name__ == "__main__":
    main()
