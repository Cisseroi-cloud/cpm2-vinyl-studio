#include "car_model.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace cpm {

bool CarModel::loadFromFile(const std::string& cpmcarPath) {
    std::ifstream f(cpmcarPath, std::ios::binary);
    if (!f) return false;

    char magic[8] = {};
    f.read(magic, 8);
    if (std::memcmp(magic, "CPMCAR01", 8) != 0) return false;

    uint32_t cid = 0, nSub = 0, nKit = 0, nWheel = 0;
    f.read(reinterpret_cast<char*>(&cid), 4);
    f.read(reinterpret_cast<char*>(&nSub), 4);
    f.read(reinterpret_cast<char*>(&nKit), 4);
    f.read(reinterpret_cast<char*>(&nWheel), 4);

    float bb[6] = {};
    f.read(reinterpret_cast<char*>(bb), 24);
    if (!f) return false;

    carId = (int)cid;
    loadedPath = cpmcarPath;
    bounds.bmin = {bb[0], bb[1], bb[2]};
    bounds.bmax = {bb[3], bb[4], bb[5]};

    kitGroups.clear();
    kitGroups.resize(nKit);
    for (uint32_t i = 0; i < nKit; ++i) {
        char nameBuf[32] = {};
        f.read(nameBuf, 32);
        uint32_t vc = 1, dv = 0;
        f.read(reinterpret_cast<char*>(&vc), 4);
        f.read(reinterpret_cast<char*>(&dv), 4);
        kitGroups[i].name = std::string(nameBuf, strnlen(nameBuf, 32));
        kitGroups[i].variantCount = vc;
        kitGroups[i].defaultVariant = dv;
    }

    wheels.clear();
    wheels.resize(nWheel);
    for (uint32_t i = 0; i < nWheel; ++i) {
        char nameBuf[32] = {};
        f.read(nameBuf, 32);
        float wv[5] = {};
        f.read(reinterpret_cast<char*>(wv), 20);
        wheels[i].name = std::string(nameBuf, strnlen(nameBuf, 32));
        wheels[i].pos = {wv[0], wv[1], wv[2]};
        wheels[i].radius = wv[3];
        wheels[i].width = wv[4];
    }

    submeshes.clear();
    submeshes.reserve(nSub);
    for (uint32_t i = 0; i < nSub; ++i) {
        char nameBuf[64] = {};
        f.read(nameBuf, 64);
        uint32_t surf = 3;
        int32_t kg = -1, kv = -1;
        float matInfo[6] = {};
        uint32_t vCount = 0, iCount = 0;
        f.read(reinterpret_cast<char*>(&surf), 4);
        f.read(reinterpret_cast<char*>(&kg), 4);
        f.read(reinterpret_cast<char*>(&kv), 4);
        f.read(reinterpret_cast<char*>(matInfo), 24);
        f.read(reinterpret_cast<char*>(&vCount), 4);
        f.read(reinterpret_cast<char*>(&iCount), 4);
        if (!f) return false;

        CarSubmesh sm;
        sm.name = std::string(nameBuf, strnlen(nameBuf, 64));
        sm.surface = (SurfaceType)std::min(surf, 3u);
        sm.kitGroupIdx = kg;
        sm.kitVariantIdx = kv;
        sm.baseColor[0] = matInfo[0];
        sm.baseColor[1] = matInfo[1];
        sm.baseColor[2] = matInfo[2];
        sm.baseColor[3] = matInfo[3];
        sm.roughness = matInfo[4];
        sm.metallic = matInfo[5];

        std::vector<float> rawVerts((size_t)vCount * 8u);
        f.read(reinterpret_cast<char*>(rawVerts.data()), (std::streamsize)(rawVerts.size() * sizeof(float)));
        sm.indices.resize(iCount);
        f.read(reinterpret_cast<char*>(sm.indices.data()), (std::streamsize)((size_t)iCount * sizeof(uint32_t)));
        if (!f) return false;

        sm.vertices.resize(vCount);
        for (uint32_t v = 0; v < vCount; ++v) {
            const float* p = &rawVerts[(size_t)v * 8u];
            sm.vertices[v] = {p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], 1.0f};
            sm.bounds.expand({p[0], p[1], p[2]});
        }
        if (sm.surface == SURF_BODY || sm.surface == SURF_WINDOWS || sm.surface == SURF_PLATE) {
            computeUVDistortion(sm);
        }
        submeshes.push_back(std::move(sm));
    }

    buildProceduralWheels();
    std::vector<int> defaults(kitGroups.size(), 0);
    for (size_t i = 0; i < kitGroups.size(); ++i) defaults[i] = (int)kitGroups[i].defaultVariant;
    rebuildBVH(defaults);
    return true;
}

void CarModel::computeUVDistortion(CarSubmesh& sm) {
    // Compute per-triangle UV stretch / anisotropy ratio sigma_max / sigma_min of 2D-UV -> 3D mapping
    // Ratio == 1.0 means perfectly isotropic (zero distortion), > 2.0 means heavy stretching on fenders/curves!
    const size_t nV = sm.vertices.size();
    std::vector<float> acc(nV, 0.0f);
    std::vector<float> cnt(nV, 0.0f);

    for (size_t t = 0; t + 2 < sm.indices.size(); t += 3) {
        uint32_t i0 = sm.indices[t], i1 = sm.indices[t + 1], i2 = sm.indices[t + 2];
        if (i0 >= nV || i1 >= nV || i2 >= nV) continue;
        const auto& v0 = sm.vertices[i0];
        const auto& v1 = sm.vertices[i1];
        const auto& v2 = sm.vertices[i2];

        Vec3 e1(v1.px - v0.px, v1.py - v0.py, v1.pz - v0.pz);
        Vec3 e2(v2.px - v0.px, v2.py - v0.py, v2.pz - v0.pz);
        float du1 = v1.u - v0.u, dv1 = v1.v - v0.v;
        float du2 = v2.u - v0.u, dv2 = v2.v - v0.v;
        float det = du1 * dv2 - du2 * dv1;
        float ratio = 1.0f;
        if (std::fabs(det) > 1e-10f) {
            float inv = 1.0f / det;
            Vec3 dpdu = (e1 * dv2 - e2 * dv1) * inv;
            Vec3 dpdv = (e2 * du1 - e1 * du2) * inv;
            // First fundamental form G = [E F; F G] -> singular values squared are eigenvalues of G
            float E = dpdu.dot(dpdu);
            float F = dpdu.dot(dpdv);
            float G = dpdv.dot(dpdv);
            float tr = E + G;
            float disc = std::sqrt(std::max(0.0f, (E - G) * (E - G) + 4.0f * F * F));
            float l1 = std::max(1e-8f, 0.5f * (tr + disc));
            float l2 = std::max(1e-8f, 0.5f * (tr - disc));
            ratio = std::sqrt(l1 / l2);
            ratio = std::clamp(ratio, 1.0f, 6.0f);
        } else {
            ratio = 4.0f; // degenerate UV
        }
        float area = 0.5f * e1.cross(e2).len() + 1e-6f;
        acc[i0] += ratio * area; cnt[i0] += area;
        acc[i1] += ratio * area; cnt[i1] += area;
        acc[i2] += ratio * area; cnt[i2] += area;
    }

    for (size_t i = 0; i < nV; ++i) {
        sm.vertices[i].uvDistortion = (cnt[i] > 0.0f) ? (acc[i] / cnt[i]) : 1.0f;
    }
}

void CarModel::buildProceduralWheels() {
    proceduralWheels.clear();
    const int segs = 28;
    for (const auto& w : wheels) {
        // Tire cylinder + rim face aligned along X axis (wheel axle)
        CarSubmesh tire;
        tire.name = "Proc_" + w.name + "_Tire";
        tire.surface = SURF_STATIC;
        tire.baseColor[0] = 0.08f; tire.baseColor[1] = 0.08f; tire.baseColor[2] = 0.09f; tire.baseColor[3] = 1.0f;
        tire.roughness = 0.85f;
        tire.metallic = 0.02f;

        CarSubmesh rim;
        rim.name = "Proc_" + w.name + "_Rim";
        rim.surface = SURF_STATIC;
        rim.baseColor[0] = 0.72f; rim.baseColor[1] = 0.74f; rim.baseColor[2] = 0.78f; rim.baseColor[3] = 1.0f;
        rim.roughness = 0.22f;
        rim.metallic = 0.88f;

        auto addCyl = [&](CarSubmesh& dst, float r, float halfW) {
            uint32_t base = (uint32_t)dst.vertices.size();
            // Tread band
            for (int i = 0; i <= segs; ++i) {
                float a = (float)i / (float)segs * 2.0f * kPi;
                float cy = std::cos(a), sz = std::sin(a);
                dst.vertices.push_back({w.pos.x - halfW, w.pos.y + r * cy, w.pos.z + r * sz, 0, cy, sz, 0, 0, 1});
                dst.vertices.push_back({w.pos.x + halfW, w.pos.y + r * cy, w.pos.z + r * sz, 0, cy, sz, 0, 0, 1});
            }
            for (int i = 0; i < segs; ++i) {
                uint32_t b = base + (uint32_t)(i * 2);
                dst.indices.insert(dst.indices.end(), {b, b + 2, b + 1, b + 1, b + 2, b + 3});
            }
            // Side caps (left -X and right +X)
            for (int side = -1; side <= 1; side += 2) {
                uint32_t cIdx = (uint32_t)dst.vertices.size();
                float sx = w.pos.x + side * halfW;
                dst.vertices.push_back({sx, w.pos.y, w.pos.z, (float)side, 0, 0, 0, 0, 1});
                for (int i = 0; i <= segs; ++i) {
                    float a = (float)i / (float)segs * 2.0f * kPi;
                    float cy = std::cos(a), sz = std::sin(a);
                    dst.vertices.push_back({sx, w.pos.y + r * cy, w.pos.z + r * sz, (float)side, 0, 0, 0, 0, 1});
                }
                for (int i = 0; i < segs; ++i) {
                    uint32_t v1 = cIdx + 1 + (uint32_t)i;
                    uint32_t v2 = cIdx + 1 + (uint32_t)(i + 1);
                    if (side > 0) dst.indices.insert(dst.indices.end(), {cIdx, v1, v2});
                    else dst.indices.insert(dst.indices.end(), {cIdx, v2, v1});
                }
            }
        };

        addCyl(tire, w.radius, w.width * 0.5f);
        addCyl(rim, w.radius * 0.64f, w.width * 0.52f);
        proceduralWheels.push_back(std::move(tire));
        proceduralWheels.push_back(std::move(rim));
    }
}

bool CarModel::isSubmeshActive(const CarSubmesh& sm, const std::vector<int>& selectedVariants) const {
    if (sm.kitGroupIdx < 0) return true;
    if ((size_t)sm.kitGroupIdx >= selectedVariants.size()) return sm.kitVariantIdx == 0;
    return sm.kitVariantIdx == selectedVariants[(size_t)sm.kitGroupIdx];
}

void CarModel::rebuildBVH(const std::vector<int>& selectedVariants) {
    m_bvhTris.clear();
    m_bvhNodes.clear();

    for (int s = 0; s < (int)submeshes.size(); ++s) {
        const auto& sm = submeshes[(size_t)s];
        if (sm.surface == SURF_STATIC) continue;
        if (!isSubmeshActive(sm, selectedVariants)) continue;
        for (size_t t = 0; t + 2 < sm.indices.size(); t += 3) {
            uint32_t i0 = sm.indices[t], i1 = sm.indices[t + 1], i2 = sm.indices[t + 2];
            if (i0 >= sm.vertices.size() || i1 >= sm.vertices.size() || i2 >= sm.vertices.size()) continue;
            const auto& v0 = sm.vertices[i0];
            const auto& v1 = sm.vertices[i1];
            const auto& v2 = sm.vertices[i2];
            BVHTri bt;
            bt.submeshIdx = s;
            bt.triIdx = (int)(t / 3);
            bt.surface = sm.surface;
            bt.p0 = {v0.px, v0.py, v0.pz};
            bt.p1 = {v1.px, v1.py, v1.pz};
            bt.p2 = {v2.px, v2.py, v2.pz};
            bt.uv0 = {v0.u, v0.v};
            bt.uv1 = {v1.u, v1.v};
            bt.uv2 = {v2.u, v2.v};
            bt.faceNormal = (bt.p1 - bt.p0).cross(bt.p2 - bt.p0).normalized();
            bt.centroid = (bt.p0 + bt.p1 + bt.p2) * (1.0f / 3.0f);
            bt.box.expand(bt.p0); bt.box.expand(bt.p1); bt.box.expand(bt.p2);
            m_bvhTris.push_back(bt);
        }
    }

    if (!m_bvhTris.empty()) {
        m_bvhNodes.reserve(m_bvhTris.size() * 2);
        buildBVHNode(0, (int)m_bvhTris.size());
    }
}

int CarModel::buildBVHNode(int start, int count) {
    int nodeIdx = (int)m_bvhNodes.size();
    m_bvhNodes.push_back({});

    AABB box, cbox;
    uint32_t mask = 0;
    for (int i = 0; i < count; ++i) {
        const auto& tr = m_bvhTris[(size_t)(start + i)];
        box.expand(tr.box);
        cbox.expand(tr.centroid);
        mask |= (1u << (uint32_t)tr.surface);
    }

    m_bvhNodes[(size_t)nodeIdx].box = box;
    m_bvhNodes[(size_t)nodeIdx].surfMask = mask;

    if (count <= 8) {
        m_bvhNodes[(size_t)nodeIdx].firstTri = start;
        m_bvhNodes[(size_t)nodeIdx].triCount = count;
        return nodeIdx;
    }

    Vec3 ext = cbox.extent();
    int axis = 0;
    if (ext.y > ext.x && ext.y >= ext.z) axis = 1;
    else if (ext.z > ext.x && ext.z >= ext.y) axis = 2;

    if (ext[axis] < 1e-6f) {
        m_bvhNodes[(size_t)nodeIdx].firstTri = start;
        m_bvhNodes[(size_t)nodeIdx].triCount = count;
        return nodeIdx;
    }

    int mid = start + count / 2;
    std::nth_element(
        m_bvhTris.begin() + start,
        m_bvhTris.begin() + mid,
        m_bvhTris.begin() + start + count,
        [axis](const BVHTri& a, const BVHTri& b) { return a.centroid[axis] < b.centroid[axis]; }
    );

    int left = buildBVHNode(start, mid - start);
    int right = buildBVHNode(mid, start + count - mid);
    m_bvhNodes[(size_t)nodeIdx].left = left;
    m_bvhNodes[(size_t)nodeIdx].right = right;
    return nodeIdx;
}

bool CarModel::intersectAABB(const Ray& r, const Vec3& invDir, float maxT, const AABB& b, float& tNear) {
    float tmin = 0.0f, tmax = maxT;
    for (int a = 0; a < 3; ++a) {
        float t0 = (b.bmin[a] - r.origin[a]) * invDir[a];
        float t1 = (b.bmax[a] - r.origin[a]) * invDir[a];
        if (invDir[a] < 0.0f) std::swap(t0, t1);
        tmin = std::max(tmin, t0);
        tmax = std::min(tmax, t1);
        if (tmax < tmin) return false;
    }
    tNear = tmin;
    return true;
}

bool CarModel::intersectTri(const Ray& r, const BVHTri& tri, float maxT, float& outT, float& outU, float& outV) {
    Vec3 e1 = tri.p1 - tri.p0;
    Vec3 e2 = tri.p2 - tri.p0;
    Vec3 pvec = r.dir.cross(e2);
    float det = e1.dot(pvec);
    if (std::fabs(det) < 1e-9f) return false;
    float invDet = 1.0f / det;
    Vec3 tvec = r.origin - tri.p0;
    float u = tvec.dot(pvec) * invDet;
    if (u < -1e-5f || u > 1.0f + 1e-5f) return false;
    Vec3 qvec = tvec.cross(e1);
    float v = r.dir.dot(qvec) * invDet;
    if (v < -1e-5f || u + v > 1.0f + 1e-5f) return false;
    float t = e2.dot(qvec) * invDet;
    if (t < 1e-5f || t > maxT) return false;
    outT = t;
    outU = std::clamp(u, 0.0f, 1.0f);
    outV = std::clamp(v, 0.0f, 1.0f);
    return true;
}

bool CarModel::raycast(const Ray& ray, float maxDist, uint32_t surfaceMask, RayHit& outHit) const {
    outHit = RayHit{};
    if (m_bvhNodes.empty()) return false;

    Vec3 invDir(
        std::fabs(ray.dir.x) > 1e-9f ? 1.0f / ray.dir.x : 1e9f,
        std::fabs(ray.dir.y) > 1e-9f ? 1.0f / ray.dir.y : 1e9f,
        std::fabs(ray.dir.z) > 1e-9f ? 1.0f / ray.dir.z : 1e9f
    );

    int stack[64];
    int sp = 0;
    stack[sp++] = 0;
    float bestT = maxDist;

    while (sp > 0) {
        const auto& n = m_bvhNodes[(size_t)stack[--sp]];
        if ((n.surfMask & surfaceMask) == 0) continue;
        float tNear = 0.0f;
        if (!intersectAABB(ray, invDir, bestT, n.box, tNear)) continue;

        if (n.triCount > 0) {
            for (int i = 0; i < n.triCount; ++i) {
                const auto& tr = m_bvhTris[(size_t)(n.firstTri + i)];
                if (((1u << (uint32_t)tr.surface) & surfaceMask) == 0) continue;
                float t, bu, bv;
                if (intersectTri(ray, tr, bestT, t, bu, bv)) {
                    // Prefer front-facing surfaces (ray moving opposite to outward normal)
                    if (ray.dir.dot(tr.faceNormal) > 0.15f) continue;
                    bestT = t;
                    float bw = 1.0f - bu - bv;
                    outHit.hit = true;
                    outHit.t = t;
                    outHit.pos = ray.origin + ray.dir * t;
                    outHit.normal = tr.faceNormal;
                    outHit.uv = tr.uv0 * bw + tr.uv1 * bu + tr.uv2 * bv;
                    outHit.surface = tr.surface;
                    outHit.submeshIdx = tr.submeshIdx;
                    outHit.triIdx = tr.triIdx;
                    outHit.p0 = tr.p0; outHit.p1 = tr.p1; outHit.p2 = tr.p2;
                    outHit.uv0 = tr.uv0; outHit.uv1 = tr.uv1; outHit.uv2 = tr.uv2;
                }
            }
        } else {
            if (n.left >= 0 && sp < 62) stack[sp++] = n.left;
            if (n.right >= 0 && sp < 62) stack[sp++] = n.right;
        }
    }
    return outHit.hit;
}

void CarModel::raycastAll(const Ray& ray, float maxDist, uint32_t surfaceMask, std::vector<RayHit>& outHits) const {
    outHits.clear();
    if (m_bvhNodes.empty()) return;
    Vec3 invDir(
        std::fabs(ray.dir.x) > 1e-9f ? 1.0f / ray.dir.x : 1e9f,
        std::fabs(ray.dir.y) > 1e-9f ? 1.0f / ray.dir.y : 1e9f,
        std::fabs(ray.dir.z) > 1e-9f ? 1.0f / ray.dir.z : 1e9f
    );
    int stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const auto& n = m_bvhNodes[(size_t)stack[--sp]];
        if ((n.surfMask & surfaceMask) == 0) continue;
        float tNear = 0.0f;
        if (!intersectAABB(ray, invDir, maxDist, n.box, tNear)) continue;
        if (n.triCount > 0) {
            for (int i = 0; i < n.triCount; ++i) {
                const auto& tr = m_bvhTris[(size_t)(n.firstTri + i)];
                if (((1u << (uint32_t)tr.surface) & surfaceMask) == 0) continue;
                float t, bu, bv;
                if (intersectTri(ray, tr, maxDist, t, bu, bv)) {
                    if (ray.dir.dot(tr.faceNormal) > 0.15f) continue;
                    float bw = 1.0f - bu - bv;
                    RayHit h;
                    h.hit = true;
                    h.t = t;
                    h.pos = ray.origin + ray.dir * t;
                    h.normal = tr.faceNormal;
                    h.uv = tr.uv0 * bw + tr.uv1 * bu + tr.uv2 * bv;
                    h.surface = tr.surface;
                    h.submeshIdx = tr.submeshIdx;
                    h.triIdx = tr.triIdx;
                    h.p0 = tr.p0; h.p1 = tr.p1; h.p2 = tr.p2;
                    h.uv0 = tr.uv0; h.uv1 = tr.uv1; h.uv2 = tr.uv2;
                    outHits.push_back(h);
                }
            }
        } else {
            if (n.left >= 0 && sp < 62) stack[sp++] = n.left;
            if (n.right >= 0 && sp < 62) stack[sp++] = n.right;
        }
    }
    std::sort(outHits.begin(), outHits.end(), [](const RayHit& a, const RayHit& b) { return a.t < b.t; });
}

} // namespace cpm
