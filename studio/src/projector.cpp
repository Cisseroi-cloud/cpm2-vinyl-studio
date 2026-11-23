#include "projector.h"
#include "../../third_party/nlohmann/json.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;

namespace cpm {

bool SpriteCatalog::load(const std::string& spritesV4Dir) {
    entries.clear();
    idToSlot.clear();

    std::string metaPath = spritesV4Dir + "\\sprites_meta.json";
    std::ifstream mf(metaPath);
    if (!mf) return false;
    json j;
    try { mf >> j; } catch (...) { return false; }
    if (!j.is_array()) return false;

    for (const auto& item : j) {
        SpriteEntry e;
        e.stickerId = item.value("index", 87);
        e.name = item.value("sprite", "");
        e.nativeW = item.value("native_w", 0.02450f);
        e.nativeH = item.value("native_h", 0.02440f);
        e.category = item.value("category", 0);
        e.allowFlipX = item.value("allow_flip_x", false);
        e.arraySlice = (int)entries.size();

        std::string maskPath = spritesV4Dir + "\\sprite_" + std::to_string(e.stickerId) + ".a256";
        std::ifstream f(maskPath, std::ios::binary);
        e.mask256.assign(256 * 256, 255);
        if (f) {
            f.read(reinterpret_cast<char*>(e.mask256.data()), 256 * 256);
        }

        idToSlot[e.stickerId] = (int)entries.size();
        entries.push_back(std::move(e));
    }
    return !entries.empty();
}

// Compute exact 2x2 Jacobian J = [dUV/dx_world, dUV/dy_world] of the triangle plane
// along ray direction D, given decal orthonormal axes (rightW, upW) and ray direction rayDir.
struct LocalJacobian {
    bool valid = false;
    Vec2 uvOrigin; // UV at the triangle hit point
    Vec3 posOrigin;
    Vec2 dUVdRight; // d(u,v) per 1.0 meter along decal Right axis
    Vec2 dUVdUp;    // d(u,v) per 1.0 meter along decal Up axis
};

static LocalJacobian computeLocalJacobian(
    const RayHit& hit,
    const Vec3& decalRightUnit,
    const Vec3& decalUpUnit,
    const Vec3& rayDirUnit
) {
    LocalJacobian res;
    Vec3 e1 = hit.p1 - hit.p0;
    Vec3 e2 = hit.p2 - hit.p0;
    Vec3 n = e1.cross(e2);
    float nLen = n.len();
    if (nLen < 1e-10f) return res;
    Vec3 nUnit = n * (1.0f / nLen);

    float denom = rayDirUnit.dot(nUnit);
    if (std::fabs(denom) < 0.08f) return res; // grazing angle

    // Oblique projection of a vector V onto triangle plane along rayDirUnit:
    // V_proj = V - rayDirUnit * (V . nUnit) / (rayDirUnit . nUnit)
    auto projToPlane = [&](const Vec3& v) -> Vec3 {
        return v - rayDirUnit * (v.dot(nUnit) / denom);
    };

    Vec3 rPlane = projToPlane(decalRightUnit);
    Vec3 uPlane = projToPlane(decalUpUnit);

    // Solve barycentric coordinates (a, b) such that a * e1 + b * e2 = v_plane
    float d00 = e1.dot(e1);
    float d01 = e1.dot(e2);
    float d11 = e2.dot(e2);
    float det = d00 * d11 - d01 * d01;
    if (std::fabs(det) < 1e-12f) return res;
    float invDet = 1.0f / det;

    Vec2 duv1 = hit.uv1 - hit.uv0;
    Vec2 duv2 = hit.uv2 - hit.uv0;

    auto deltaUVFor = [&](const Vec3& vp) -> Vec2 {
        float d20 = vp.dot(e1);
        float d21 = vp.dot(e2);
        float a = (d11 * d20 - d01 * d21) * invDet;
        float b = (d00 * d21 - d01 * d20) * invDet;
        return duv1 * a + duv2 * b;
    };

    res.valid = true;
    res.uvOrigin = hit.uv;
    res.posOrigin = hit.pos;
    res.dUVdRight = deltaUVFor(rPlane);
    res.dUVdUp = deltaUVFor(uPlane);
    return res;
}

static void emitProjectedShapeOnCluster(
    const DecalShape& sh,
    const RayHit& repHit,
    float repLocalX_m, // offset of repHit from shape center in decal right meters
    float repLocalY_m, // offset of repHit from shape center in decal up meters
    const Vec3& decalRightUnit,
    const Vec3& decalUpUnit,
    const Vec3& rayDirUnit,
    float wMeters,
    float hMeters,
    float shapeAngleDeg, // CCW in decal plane
    bool flipH,
    const SpriteCatalog& catalog,
    std::vector<VinylLayer>& outList
) {
    LocalJacobian jac = computeLocalJacobian(repHit, decalRightUnit, decalUpUnit, rayDirUnit);
    if (!jac.valid) return;

    // Extrapolate UV center from repHit back to shape center (crucial when shape straddles Body/Window seam!)
    Vec2 uvCenter = jac.uvOrigin - jac.dUVdRight * repLocalX_m - jac.dUVdUp * repLocalY_m;

    // Clamp sanity check: ignore wildly extrapolated hits far outside [0, 1] UV space
    if (uvCenter.x < -0.25f || uvCenter.x > 1.25f || uvCenter.y < -0.25f || uvCenter.y > 1.25f) return;

    // Shape's local X and Y axes in decal plane (radians CCW)
    float rad = shapeAngleDeg * kDeg2Rad;
    float ca = std::cos(rad), sa = std::sin(rad);

    // Axis vectors in meters (full width/height) mapped through Jacobian into UV space
    Vec2 uvAxisX = (jac.dUVdRight * ca + jac.dUVdUp * sa) * wMeters;
    Vec2 uvAxisY = (jac.dUVdRight * (-sa) + jac.dUVdUp * ca) * hMeters;

    float uw = uvAxisX.len();
    float uh = uvAxisY.len();
    if (uw < 1e-5f || uh < 1e-5f || uw > 1.8f || uh > 1.8f) return;

    // Rotation in UV plane (CCW degrees)
    float rotDeg = std::atan2(uvAxisX.y, uvAxisX.x) * kRad2Deg;
    rotDeg = std::fmod(rotDeg, 360.0f);
    if (rotDeg < 0.0f) rotDeg += 360.0f;

    // Check handedness (det of [uvAxisX, uvAxisY]) -> if negative, UV island is mirrored
    float detUV = uvAxisX.x * uvAxisY.y - uvAxisX.y * uvAxisY.x;
    bool mirroredUV = (detUV < 0.0f);

    VinylLayer layer;
    layer.px = uvCenter.x - 0.5f;
    layer.py = uvCenter.y - 0.5f;
    layer.rot = rotDeg;
    layer.color = packABGR(sh.r, sh.g, sh.b, sh.a);
    layer.xAngle = (repHit.surface == SURF_BODY) ? 151 : 0;
    layer.yAngle = 0;
    layer.iconPos[0] = repHit.pos.x;
    layer.iconPos[1] = repHit.pos.y;
    layer.iconPos[2] = repHit.pos.z;

    if (sh.isText) {
        layer.stickerIndex = 1;
        layer.isText = 1;
        layer.fontIndex = sh.fontIndex;
        layer.bold = sh.bold ? 1 : 0;
        layer.italic = sh.italic ? 1 : 0;
        layer.text = sh.text;
        // Text scale empirically ~ 1/0.025 per UV unit for single glyph
        layer.sx = uw / 0.025f;
        layer.sy = uh / 0.025f;
        if (sh.flipX ^ flipH ^ mirroredUV) layer.flipX = 1;
    } else {
        layer.stickerIndex = sh.sticker;
        layer.isText = 0;
        float nw = 0.0245f, nh = 0.0244f;
        catalog.nativeSizeFor(sh.sticker, nw, nh);
        layer.sx = uw / nw;
        layer.sy = uh / nh;
        if (sh.flipX ^ flipH ^ mirroredUV) {
            layer.sx = -layer.sx;
        }
    }

    outList.push_back(std::move(layer));
}

void projectLivery(
    const CarModel& car,
    const SpriteCatalog& catalog,
    LiveryProject& project,
    ProjectedLivery& outLivery
) {
    outLivery = ProjectedLivery{};

    // 1. Start with imported raw layers if enabled
    if (project.showImportedLayers) {
        outLivery.bodyLayers.insert(outLivery.bodyLayers.end(), project.importedBody.begin(), project.importedBody.end());
        outLivery.windowLayers.insert(outLivery.windowLayers.end(), project.importedWindows.begin(), project.importedWindows.end());
        outLivery.plateLayers.insert(outLivery.plateLayers.end(), project.importedPlate.begin(), project.importedPlate.end());
    }

    // 2. Project each enabled DecalItem
    for (auto& decal : project.decals) {
        decal.projectedBodyCount = 0;
        decal.projectedWindowCount = 0;
        decal.projectedPlateCount = 0;
        decal.straddleSplitCount = 0;

        if (!decal.enabled || decal.shapes.empty()) continue;

        const float srcW = (float)std::max(1, decal.srcW);
        const float srcH = (float)std::max(1, decal.srcH);
        const auto& xf = decal.xform;

        if (xf.projectionMode == 1) {
            // Direct UV Planar mode (1:1 compatible with build_livery.py / HANDOVER.md §1.3)
            float aspect = srcW / srcH;
            float scaleY = xf.uvHeight;
            float scaleX = scaleY * aspect;
            float offX = xf.uvCenter[0] - 0.5f;
            float offY = xf.uvCenter[1] - 0.5f;
            float rad = xf.uvRotation * kDeg2Rad;
            float ca = std::cos(rad), sa = std::sin(rad);

            auto& targetVec = (xf.uvSurface == SURF_WINDOWS) ? outLivery.windowLayers
                            : (xf.uvSurface == SURF_PLATE) ? outLivery.plateLayers
                            : outLivery.bodyLayers;
            int xa = (xf.uvSurface == SURF_BODY) ? 151 : 0;

            for (const auto& sh : decal.shapes) {
                if (sh.w <= 0.0f || sh.h <= 0.0f) continue;
                float nx = (sh.cx / srcW) - 0.5f;
                float ny = 0.5f - (sh.cy / srcH);
                if (xf.flipH) nx = -nx;
                if (xf.flipV) ny = -ny;
                float effRotSign = (xf.flipH ^ xf.flipV) ? 1.0f : -1.0f;

                float lx = nx * scaleX;
                float ly = ny * scaleY;
                float px = (lx * ca - ly * sa) + offX;
                float py = (lx * sa + ly * ca) + offY;
                float uw = (sh.w / srcW) * scaleX;
                float uh = (sh.h / srcH) * scaleY;
                float rot = std::fmod(effRotSign * sh.angle + xf.uvRotation, 360.0f);
                if (rot < 0.0f) rot += 360.0f;

                VinylLayer layer;
                layer.px = px; layer.py = py;
                layer.rot = rot;
                layer.color = packABGR(sh.r, sh.g, sh.b, sh.a);
                layer.xAngle = xa; layer.yAngle = 0;
                layer.iconPos[0] = xf.pos.x; layer.iconPos[1] = xf.pos.y; layer.iconPos[2] = xf.pos.z;

                if (sh.isText) {
                    layer.stickerIndex = 1;
                    layer.isText = 1;
                    layer.fontIndex = sh.fontIndex;
                    layer.bold = sh.bold ? 1 : 0;
                    layer.italic = sh.italic ? 1 : 0;
                    layer.text = sh.text;
                    layer.sx = uw / 0.025f;
                    layer.sy = uh / 0.025f;
                    layer.flipX = (sh.flipX ^ xf.flipH) ? 1 : 0;
                } else {
                    float nw = 0.0245f, nh = 0.0244f;
                    catalog.nativeSizeFor(sh.sticker, nw, nh);
                    layer.stickerIndex = sh.sticker;
                    layer.sx = uw / nw;
                    layer.sy = uh / nh;
                    if (sh.flipX ^ xf.flipH) layer.sx = -layer.sx;
                }
                targetVec.push_back(std::move(layer));
                if (xf.uvSurface == SURF_WINDOWS) decal.projectedWindowCount++;
                else if (xf.uvSurface == SURF_PLATE) decal.projectedPlateCount++;
                else decal.projectedBodyCount++;
            }
            continue;
        }

        // Mode 0: Full 3D Raycast Jacobian Multi-Surface Projection
        Mat4 rotM = Mat4::FromEulerDeg(xf.rotEuler);
        Vec3 rightUnit = rotM.transformDir({1, 0, 0}).normalized();
        Vec3 upUnit = rotM.transformDir({0, 1, 0}).normalized();
        // Local -Z is the forward projection direction (pointing from decal plane into the car!)
        Vec3 rayDirUnit = rotM.transformDir({0, 0, -1}).normalized();

        float widthM = std::max(0.02f, std::fabs(xf.scale.x));
        float heightM = std::max(0.02f, std::fabs(xf.scale.y));
        float halfDepthM = std::max(0.05f, std::fabs(xf.scale.z));
        float maxRayDist = halfDepthM * 2.0f;

        uint32_t mask = xf.targetMask;
        if (!xf.autoSplitBodyWindows && (mask & MASK_BODY) && (mask & MASK_WINDOWS)) {
            // If auto-split disabled, restrict to body only
            mask &= ~MASK_WINDOWS;
        }

        struct ClusterSample {
            RayHit hit;
            float lx_m;
            float ly_m;
            bool isCenter;
        };

        for (const auto& sh : decal.shapes) {
            if (sh.w <= 0.0f || sh.h <= 0.0f) continue;

            float nx = (sh.cx / srcW) - 0.5f;
            float ny = 0.5f - (sh.cy / srcH);
            if (xf.flipH) nx = -nx;
            if (xf.flipV) ny = -ny;

            float wM = (sh.w / srcW) * widthM;
            float hM = (sh.h / srcH) * heightM;
            float effRotSign = (xf.flipH ^ xf.flipV) ? 1.0f : -1.0f;
            float ccwAngleDeg = effRotSign * sh.angle;
            float rad = ccwAngleDeg * kDeg2Rad;
            float ca = std::cos(rad), sa = std::sin(rad);

            // Center in decal plane (meters)
            float cx_m = nx * widthM;
            float cy_m = ny * heightM;

            // 5 sample points: center + 4 oriented shape extents (at 42% of width/height so tight boundaries catch seams)
            float dxX = ca * (wM * 0.42f), dyX = sa * (wM * 0.42f);
            float dxY = -sa * (hM * 0.42f), dyY = ca * (hM * 0.42f);

            const float offsets[5][2] = {
                {0.0f, 0.0f},
                {+dxX, +dyX},
                {-dxX, -dyX},
                {+dxY, +dyY},
                {-dxY, -dyY},
            };

            std::vector<ClusterSample> clusters;
            clusters.reserve(3);

            for (int k = 0; k < 5; ++k) {
                float lx = offsets[k][0];
                float ly = offsets[k][1];
                Vec3 origin = xf.pos + rightUnit * (cx_m + lx) + upUnit * (cy_m + ly) - rayDirUnit * halfDepthM;
                Ray r{origin, rayDirUnit};
                RayHit h;
                if (!car.raycast(r, maxRayDist, mask, h)) continue;

                // Merge into existing cluster if same surface AND continuous UV (no UV island jump)
                bool merged = false;
                for (auto& c : clusters) {
                    if (c.hit.surface == h.surface) {
                        float uvDist = (c.hit.uv - h.uv).len();
                        if (uvDist < 0.14f) {
                            // Prefer the center hit as representative of the cluster
                            if (k == 0 && !c.isCenter) {
                                c.hit = h; c.lx_m = lx; c.ly_m = ly; c.isCenter = true;
                            }
                            merged = true;
                            break;
                        }
                    }
                }
                if (!merged) {
                    clusters.push_back({h, lx, ly, k == 0});
                }
            }

            if (clusters.empty()) continue;

            bool hitBody = false, hitWin = false;
            for (const auto& c : clusters) {
                auto& dst = (c.hit.surface == SURF_WINDOWS) ? outLivery.windowLayers
                          : (c.hit.surface == SURF_PLATE) ? outLivery.plateLayers
                          : outLivery.bodyLayers;
                size_t before = dst.size();
                emitProjectedShapeOnCluster(
                    sh, c.hit, c.lx_m, c.ly_m, rightUnit, upUnit, rayDirUnit,
                    wM, hM, ccwAngleDeg, xf.flipH, catalog, dst
                );
                if (dst.size() > before) {
                    if (c.hit.surface == SURF_WINDOWS) { decal.projectedWindowCount++; hitWin = true; }
                    else if (c.hit.surface == SURF_PLATE) { decal.projectedPlateCount++; }
                    else { decal.projectedBodyCount++; hitBody = true; }
                }
            }
            if (hitBody && hitWin) {
                decal.straddleSplitCount++;
            }
        }
    }

    // 3. Direct text layers
    for (const auto& t : project.texts) {
        if (!t.enabled || t.text.empty()) continue;
        VinylLayer layer;
        layer.px = t.uvCenter[0] - 0.5f;
        layer.py = t.uvCenter[1] - 0.5f;
        layer.sx = t.scale[0];
        layer.sy = t.scale[1];
        layer.rot = std::fmod(t.rotation, 360.0f);
        if (layer.rot < 0.0f) layer.rot += 360.0f;
        layer.color = packABGR(
            (uint8_t)std::clamp((int)std::round(t.color[0] * 255.0f), 0, 255),
            (uint8_t)std::clamp((int)std::round(t.color[1] * 255.0f), 0, 255),
            (uint8_t)std::clamp((int)std::round(t.color[2] * 255.0f), 0, 255),
            (uint8_t)std::clamp((int)std::round(t.color[3] * 255.0f), 0, 255)
        );
        layer.stickerIndex = 1;
        layer.xAngle = (t.surface == SURF_BODY) ? 151 : 0;
        layer.yAngle = 0;
        layer.iconPos[0] = t.iconPos[0];
        layer.iconPos[1] = t.iconPos[1];
        layer.iconPos[2] = t.iconPos[2];
        layer.isText = 1;
        layer.italic = t.italic ? 1 : 0;
        layer.bold = t.bold ? 1 : 0;
        layer.flipX = t.flipX ? 1 : 0;
        layer.fontIndex = t.fontIndex;
        layer.text = t.text;

        if (t.surface == SURF_WINDOWS) outLivery.windowLayers.push_back(std::move(layer));
        else if (t.surface == SURF_PLATE) outLivery.plateLayers.push_back(std::move(layer));
        else outLivery.bodyLayers.push_back(std::move(layer));
    }

    // 4. Record raw counts & assign sequential Z order (clamped to hard CPM2 server limits)
    outLivery.rawBodyCount = (int)outLivery.bodyLayers.size();
    outLivery.rawWindowCount = (int)outLivery.windowLayers.size();
    outLivery.rawPlateCount = (int)outLivery.plateLayers.size();

    if ((int)outLivery.bodyLayers.size() > kQuotaBody) outLivery.bodyLayers.resize(kQuotaBody);
    if ((int)outLivery.windowLayers.size() > kQuotaWindows) outLivery.windowLayers.resize(kQuotaWindows);
    if ((int)outLivery.plateLayers.size() > kQuotaPlate) outLivery.plateLayers.resize(kQuotaPlate);

    for (int i = 0; i < (int)outLivery.bodyLayers.size(); ++i) outLivery.bodyLayers[(size_t)i].order = i;
    for (int i = 0; i < (int)outLivery.windowLayers.size(); ++i) outLivery.windowLayers[(size_t)i].order = i;
    for (int i = 0; i < (int)outLivery.plateLayers.size(); ++i) outLivery.plateLayers[(size_t)i].order = i;
}

void snapDecalToHit(DecalItem& decal, const RayHit& hit) {
    if (!hit.hit) return;
    Vec3 n = hit.normal.normalized();
    // Place projector slightly off the surface along +n; projection direction (-Z local) points along -n, so +Z local = +n!
    decal.xform.pos = hit.pos + n * 0.08f;

    // Derive Euler (Y * X * Z) such that local +Z aligns with +n
    // In LH coordinates, R_y(yaw) * R_x(pitch) * (0,0,1) = (sin(yaw)*cos(pitch), -sin(pitch), cos(yaw)*cos(pitch))
    float pitch = std::asin(std::clamp(-n.y, -1.0f, 1.0f));
    float yaw = 0.0f;
    if (std::fabs(n.y) < 0.995f) {
        yaw = std::atan2(n.x, n.z);
    }
    decal.xform.rotEuler = {pitch * kRad2Deg, yaw * kRad2Deg, 0.0f};
}

bool loadLayersTxt(const std::string& path, std::vector<VinylLayer>& outLayers) {
    std::ifstream f(path);
    if (!f) return false;
    outLayers.clear();
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::string mainPart = line;
        std::string textMeta, textStr;
        size_t bar1 = line.find('|');
        if (bar1 != std::string::npos) {
            mainPart = line.substr(0, bar1);
            size_t bar2 = line.find('|', bar1 + 1);
            if (bar2 != std::string::npos) {
                textMeta = line.substr(bar1 + 1, bar2 - bar1 - 1);
                textStr = line.substr(bar2 + 1);
            }
        }
        std::istringstream iss(mainPart);
        VinylLayer l;
        if (!(iss >> l.px >> l.py >> l.order >> l.sx >> l.sy >> l.rot >> l.color >> l.stickerIndex >> l.xAngle >> l.yAngle)) {
            continue;
        }
        if (!(iss >> l.iconPos[0] >> l.iconPos[1] >> l.iconPos[2])) {
            l.iconPos[0] = 0.0f; l.iconPos[1] = 0.72f; l.iconPos[2] = 1.15f;
        }
        if (!textMeta.empty()) {
            std::istringstream tss(textMeta);
            tss >> l.isText >> l.italic >> l.bold >> l.flipX >> l.fontIndex;
            l.text = textStr;
        }
        outLayers.push_back(std::move(l));
    }
    return !outLayers.empty();
}

bool saveLayersTxt(const std::string& path, const std::vector<VinylLayer>& layers, int hardQuota) {
    std::ofstream f(path);
    if (!f) return false;
    f << "# px py order sx sy rot color sticker xAngle yAngle ix iy iz [|isText italic bold flipX fontIndex|text]\n";
    int n = std::min((int)layers.size(), hardQuota);
    f << std::fixed;
    for (int i = 0; i < n; ++i) {
        const auto& l = layers[(size_t)i];
        f << std::setprecision(6) << l.px << " " << l.py << " " << i << " "
          << l.sx << " " << l.sy << " "
          << std::setprecision(3) << l.rot << " "
          << l.color << " " << l.stickerIndex << " " << l.xAngle << " " << l.yAngle << " "
          << std::setprecision(4) << l.iconPos[0] << " " << l.iconPos[1] << " " << l.iconPos[2];
        if (l.isText || !l.text.empty()) {
            f << " |" << l.isText << " " << l.italic << " " << l.bold << " " << l.flipX << " " << l.fontIndex << "|" << l.text;
        }
        f << "\n";
    }
    return true;
}

bool loadShapesJson(const std::string& path, DecalItem& decal) {
    std::ifstream f(path);
    if (!f) return false;
    json j;
    try { f >> j; } catch (...) { return false; }

    const json* arr = nullptr;
    if (j.is_object() && j.contains("shapes")) {
        decal.srcW = j.value("width", 400);
        decal.srcH = j.value("height", 400);
        arr = &j["shapes"];
    } else if (j.is_array()) {
        arr = &j;
    } else {
        return false;
    }

    decal.shapes.clear();
    decal.shapes.reserve(arr->size());
    for (const auto& s : *arr) {
        DecalShape ds;
        ds.sticker = s.value("sticker", 87);
        ds.flipX = s.value("flipX", false);
        ds.cx = s.value("cx", 0.0f);
        ds.cy = s.value("cy", 0.0f);
        ds.w = s.value("w", 10.0f);
        ds.h = s.value("h", 10.0f);
        ds.angle = s.value("angle", 0.0f);
        if (s.contains("color") && s["color"].is_array() && s["color"].size() >= 4) {
            ds.r = (uint8_t)s["color"][0].get<int>();
            ds.g = (uint8_t)s["color"][1].get<int>();
            ds.b = (uint8_t)s["color"][2].get<int>();
            ds.a = (uint8_t)s["color"][3].get<int>();
        }
        ds.isText = s.value("isText", false);
        ds.fontIndex = s.value("fontIndex", 0);
        ds.bold = s.value("bold", false);
        ds.italic = s.value("italic", false);
        ds.text = s.value("text", "");
        decal.shapes.push_back(std::move(ds));
    }
    decal.shapesJsonPath = path;
    if (decal.xform.lockAspect && decal.srcH > 0) {
        decal.xform.scale.x = decal.xform.scale.y * ((float)decal.srcW / (float)decal.srcH);
    }
    return true;
}

bool saveProjectJson(const std::string& path, const LiveryProject& project) {
    json j;
    j["version"] = 1;
    j["carId"] = project.carId;
    j["kitVariants"] = project.kitVariants;
    j["paintColor"] = {project.paintColor[0], project.paintColor[1], project.paintColor[2]};
    j["paintMetallic"] = project.paintMetallic;
    j["paintRoughness"] = project.paintRoughness;
    j["windowTint"] = {project.windowTint[0], project.windowTint[1], project.windowTint[2], project.windowTint[3]};

    json decalsJ = json::array();
    for (const auto& d : project.decals) {
        json dj;
        dj["name"] = d.name;
        dj["enabled"] = d.enabled;
        dj["imagePath"] = d.imagePath;
        dj["shapesJsonPath"] = d.shapesJsonPath;
        dj["srcW"] = d.srcW;
        dj["srcH"] = d.srcH;
        dj["prep"] = {
            {"bgMode", d.prep.bgMode}, {"bgTol", d.prep.bgTol}, {"removeHoles", d.prep.removeHoles},
            {"erodePx", d.prep.erodePx}, {"stripWhiteBorder", d.prep.stripWhiteBorder},
            {"posterizeColors", d.prep.posterizeColors}, {"contrastBoost", d.prep.contrastBoost},
            {"saturationBoost", d.prep.saturationBoost},
            {"workSize", d.prep.workSize}, {"useFocus", d.prep.useFocus},
            {"focusRect", {d.prep.focusRect[0], d.prep.focusRect[1], d.prep.focusRect[2], d.prep.focusRect[3]}},
            {"focusBoost", d.prep.focusBoost}
        };
        dj["tiling"] = {
            {"cols", d.tiling.cols}, {"rows", d.tiling.rows}, {"bleedPx", d.tiling.bleedPx},
            {"autoBudgetByDensity", d.tiling.autoBudgetByDensity}
        };
        dj["conv"] = {
            {"mode", d.conv.mode}, {"totalShapes", d.conv.totalShapes},
            {"flatPassRatio", d.conv.flatPassRatio}, {"lineartThreshold", d.conv.lineartThreshold},
            {"candidates", d.conv.candidates}, {"mutations", d.conv.mutations},
            {"bgPenalty", d.conv.bgPenalty}, {"adaptiveAlpha", d.conv.adaptiveAlpha},
            {"seed", d.conv.seed}
        };
        dj["xform"] = {
            {"pos", {d.xform.pos.x, d.xform.pos.y, d.xform.pos.z}},
            {"rotEuler", {d.xform.rotEuler.x, d.xform.rotEuler.y, d.xform.rotEuler.z}},
            {"scale", {d.xform.scale.x, d.xform.scale.y, d.xform.scale.z}},
            {"lockAspect", d.xform.lockAspect}, {"flipH", d.xform.flipH}, {"flipV", d.xform.flipV},
            {"targetMask", d.xform.targetMask}, {"autoSplitBodyWindows", d.xform.autoSplitBodyWindows},
            {"projectionMode", d.xform.projectionMode},
            {"uvCenter", {d.xform.uvCenter[0], d.xform.uvCenter[1]}},
            {"uvHeight", d.xform.uvHeight}, {"uvRotation", d.xform.uvRotation}, {"uvSurface", d.xform.uvSurface}
        };
        json shArr = json::array();
        for (const auto& s : d.shapes) {
            shArr.push_back({
                {"sticker", s.sticker}, {"flipX", s.flipX},
                {"cx", s.cx}, {"cy", s.cy}, {"w", s.w}, {"h", s.h}, {"angle", s.angle},
                {"color", {(int)s.r, (int)s.g, (int)s.b, (int)s.a}},
                {"isText", s.isText}, {"fontIndex", s.fontIndex},
                {"bold", s.bold}, {"italic", s.italic}, {"text", s.text}
            });
        }
        dj["shapes"] = std::move(shArr);
        decalsJ.push_back(std::move(dj));
    }
    j["decals"] = std::move(decalsJ);

    json textsJ = json::array();
    for (const auto& t : project.texts) {
        textsJ.push_back({
            {"name", t.name}, {"enabled", t.enabled}, {"text", t.text},
            {"fontIndex", t.fontIndex}, {"bold", t.bold}, {"italic", t.italic}, {"flipX", t.flipX},
            {"surface", t.surface}, {"uvCenter", {t.uvCenter[0], t.uvCenter[1]}},
            {"scale", {t.scale[0], t.scale[1]}}, {"rotation", t.rotation},
            {"color", {t.color[0], t.color[1], t.color[2], t.color[3]}},
            {"iconPos", {t.iconPos[0], t.iconPos[1], t.iconPos[2]}}
        });
    }
    j["texts"] = std::move(textsJ);

    std::ofstream f(path);
    if (!f) return false;
    f << j.dump(2);
    return true;
}

bool loadProjectJson(const std::string& path, LiveryProject& project) {
    std::ifstream f(path);
    if (!f) return false;
    json j;
    try { f >> j; } catch (...) { return false; }

    project = LiveryProject{};
    project.carId = j.value("carId", 187);
    if (j.contains("kitVariants") && j["kitVariants"].is_array()) {
        project.kitVariants = j["kitVariants"].get<std::vector<int>>();
    }
    if (j.contains("paintColor") && j["paintColor"].size() == 3) {
        for (int i = 0; i < 3; ++i) project.paintColor[i] = j["paintColor"][i];
    }
    project.paintMetallic = j.value("paintMetallic", 0.35f);
    project.paintRoughness = j.value("paintRoughness", 0.18f);
    if (j.contains("windowTint") && j["windowTint"].size() == 4) {
        for (int i = 0; i < 4; ++i) project.windowTint[i] = j["windowTint"][i];
    }

    if (j.contains("decals") && j["decals"].is_array()) {
        for (const auto& dj : j["decals"]) {
            DecalItem d;
            d.name = dj.value("name", "Decal");
            d.enabled = dj.value("enabled", true);
            d.imagePath = dj.value("imagePath", "");
            d.shapesJsonPath = dj.value("shapesJsonPath", "");
            d.srcW = dj.value("srcW", 400);
            d.srcH = dj.value("srcH", 400);
            if (dj.contains("prep")) {
                const auto& p = dj["prep"];
                d.prep.bgMode = p.value("bgMode", (int)BG_FLOOD);
                d.prep.bgTol = p.value("bgTol", 45.0f);
                d.prep.removeHoles = p.value("removeHoles", true);
                d.prep.erodePx = p.value("erodePx", 1);
                d.prep.stripWhiteBorder = p.value("stripWhiteBorder", false);
                d.prep.posterizeColors = p.value("posterizeColors", 8);
                d.prep.contrastBoost = p.value("contrastBoost", 1.12f);
                d.prep.saturationBoost = p.value("saturationBoost", 1.05f);
                d.prep.workSize = p.value("workSize", 400);
                d.prep.useFocus = p.value("useFocus", false);
                if (p.contains("focusRect") && p["focusRect"].size() == 4)
                    for (int i = 0; i < 4; ++i) d.prep.focusRect[i] = p["focusRect"][i];
                d.prep.focusBoost = p.value("focusBoost", 4.0f);
            }
            if (dj.contains("tiling")) {
                const auto& t = dj["tiling"];
                d.tiling.cols = t.value("cols", 1);
                d.tiling.rows = t.value("rows", 1);
                d.tiling.bleedPx = t.value("bleedPx", 4);
                d.tiling.autoBudgetByDensity = t.value("autoBudgetByDensity", true);
            }
            if (dj.contains("conv")) {
                const auto& c = dj["conv"];
                d.conv.mode = c.value("mode", (int)CONV_HYBRID_V5);
                d.conv.totalShapes = c.value("totalShapes", 180);
                d.conv.flatPassRatio = c.value("flatPassRatio", 0.65f);
                d.conv.lineartThreshold = c.value("lineartThreshold", 0.20f);
                d.conv.candidates = c.value("candidates", 320);
                d.conv.mutations = c.value("mutations", 180);
                d.conv.bgPenalty = c.value("bgPenalty", 2.4f);
                d.conv.adaptiveAlpha = c.value("adaptiveAlpha", true);
                d.conv.seed = c.value("seed", 9001);
            }
            if (dj.contains("xform")) {
                const auto& x = dj["xform"];
                if (x.contains("pos") && x["pos"].size() == 3) d.xform.pos = {x["pos"][0], x["pos"][1], x["pos"][2]};
                if (x.contains("rotEuler") && x["rotEuler"].size() == 3) d.xform.rotEuler = {x["rotEuler"][0], x["rotEuler"][1], x["rotEuler"][2]};
                if (x.contains("scale") && x["scale"].size() == 3) d.xform.scale = {x["scale"][0], x["scale"][1], x["scale"][2]};
                d.xform.lockAspect = x.value("lockAspect", true);
                d.xform.flipH = x.value("flipH", false);
                d.xform.flipV = x.value("flipV", false);
                d.xform.targetMask = x.value("targetMask", (uint32_t)(MASK_BODY | MASK_WINDOWS));
                d.xform.autoSplitBodyWindows = x.value("autoSplitBodyWindows", true);
                d.xform.projectionMode = x.value("projectionMode", 0);
                if (x.contains("uvCenter") && x["uvCenter"].size() == 2) {
                    d.xform.uvCenter[0] = x["uvCenter"][0]; d.xform.uvCenter[1] = x["uvCenter"][1];
                }
                d.xform.uvHeight = x.value("uvHeight", 0.24f);
                d.xform.uvRotation = x.value("uvRotation", -90.0f);
                d.xform.uvSurface = x.value("uvSurface", (int)SURF_BODY);
            }
            if (dj.contains("shapes") && dj["shapes"].is_array()) {
                for (const auto& s : dj["shapes"]) {
                    DecalShape ds;
                    ds.sticker = s.value("sticker", 87);
                    ds.flipX = s.value("flipX", false);
                    ds.cx = s.value("cx", 0.0f); ds.cy = s.value("cy", 0.0f);
                    ds.w = s.value("w", 10.0f); ds.h = s.value("h", 10.0f);
                    ds.angle = s.value("angle", 0.0f);
                    if (s.contains("color") && s["color"].size() >= 4) {
                        ds.r = (uint8_t)s["color"][0].get<int>();
                        ds.g = (uint8_t)s["color"][1].get<int>();
                        ds.b = (uint8_t)s["color"][2].get<int>();
                        ds.a = (uint8_t)s["color"][3].get<int>();
                    }
                    ds.isText = s.value("isText", false);
                    ds.fontIndex = s.value("fontIndex", 0);
                    ds.bold = s.value("bold", false);
                    ds.italic = s.value("italic", false);
                    ds.text = s.value("text", "");
                    d.shapes.push_back(std::move(ds));
                }
            }
            project.decals.push_back(std::move(d));
        }
    }

    if (j.contains("texts") && j["texts"].is_array()) {
        for (const auto& tj : j["texts"]) {
            DirectTextItem t;
            t.name = tj.value("name", "Texte");
            t.enabled = tj.value("enabled", true);
            t.text = tj.value("text", "");
            t.fontIndex = tj.value("fontIndex", 0);
            t.bold = tj.value("bold", false);
            t.italic = tj.value("italic", false);
            t.flipX = tj.value("flipX", false);
            t.surface = tj.value("surface", (int)SURF_WINDOWS);
            if (tj.contains("uvCenter") && tj["uvCenter"].size() == 2) { t.uvCenter[0] = tj["uvCenter"][0]; t.uvCenter[1] = tj["uvCenter"][1]; }
            if (tj.contains("scale") && tj["scale"].size() == 2) { t.scale[0] = tj["scale"][0]; t.scale[1] = tj["scale"][1]; }
            t.rotation = tj.value("rotation", 0.0f);
            if (tj.contains("color") && tj["color"].size() == 4) for (int i = 0; i < 4; ++i) t.color[i] = tj["color"][i];
            if (tj.contains("iconPos") && tj["iconPos"].size() == 3) for (int i = 0; i < 3; ++i) t.iconPos[i] = tj["iconPos"][i];
            project.texts.push_back(std::move(t));
        }
    }
    return true;
}

} // namespace cpm
