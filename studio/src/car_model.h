#pragma once
#include "math_types.h"
#include "vinyl_types.h"
#include <string>
#include <vector>

namespace cpm {

struct CarVertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float uvDistortion; // >= 1.0 (1.0 = isotropic, >2.0 = high UV stretch)
};

struct KitGroup {
    std::string name;
    uint32_t variantCount = 1;
    uint32_t defaultVariant = 0;
};

struct WheelMount {
    std::string name;
    Vec3 pos;
    float radius = 0.33f;
    float width = 0.22f;
};

struct CarSubmesh {
    std::string name;
    SurfaceType surface = SURF_STATIC;
    int32_t kitGroupIdx = -1;
    int32_t kitVariantIdx = -1;
    float baseColor[4] = {0.4f, 0.4f, 0.4f, 1.0f};
    float roughness = 0.5f;
    float metallic = 0.1f;
    std::vector<CarVertex> vertices;
    std::vector<uint32_t> indices;
    AABB bounds;
};

struct RayHit {
    bool hit = false;
    float t = 1e30f;
    Vec3 pos;
    Vec3 normal;
    Vec2 uv;
    SurfaceType surface = SURF_BODY;
    int submeshIdx = -1;
    int triIdx = -1;
    // Triangle geometry for exact local 3D -> UV Jacobian computation
    Vec3 p0, p1, p2;
    Vec2 uv0, uv1, uv2;
};

class CarModel {
public:
    int carId = 0;
    std::string loadedPath;
    AABB bounds;
    std::vector<KitGroup> kitGroups;
    std::vector<WheelMount> wheels;
    std::vector<CarSubmesh> submeshes;
    std::vector<CarSubmesh> proceduralWheels;

    bool loadFromFile(const std::string& cpmcarPath);
    bool isSubmeshActive(const CarSubmesh& sm, const std::vector<int>& selectedVariants) const;

    // Rebuild BVH over all active vinylable surfaces (BODY, WINDOWS, PLATE)
    void rebuildBVH(const std::vector<int>& selectedVariants);

    // Closest raycast hit against triangles matching surfaceMask (MASK_BODY | MASK_WINDOWS | MASK_PLATE)
    bool raycast(const Ray& ray, float maxDist, uint32_t surfaceMask, RayHit& outHit) const;

    // Collect ALL hits within [0, maxDist] (useful for straddle/double-layer checks)
    void raycastAll(const Ray& ray, float maxDist, uint32_t surfaceMask, std::vector<RayHit>& outHits) const;

private:
    struct BVHTri {
        int submeshIdx;
        int triIdx;
        SurfaceType surface;
        Vec3 p0, p1, p2;
        Vec2 uv0, uv1, uv2;
        Vec3 faceNormal;
        Vec3 centroid;
        AABB box;
    };
    struct BVHNode {
        AABB box;
        int left = -1;
        int right = -1;
        int firstTri = 0;
        int triCount = 0;
        uint32_t surfMask = 0;
    };

    std::vector<BVHTri> m_bvhTris;
    std::vector<BVHNode> m_bvhNodes;

    int buildBVHNode(int start, int count);
    void buildProceduralWheels();
    void computeUVDistortion(CarSubmesh& sm);
    static bool intersectAABB(const Ray& r, const Vec3& invDir, float maxT, const AABB& b, float& tNear);
    static bool intersectTri(const Ray& r, const BVHTri& tri, float maxT, float& outT, float& outU, float& outV);
};

} // namespace cpm
