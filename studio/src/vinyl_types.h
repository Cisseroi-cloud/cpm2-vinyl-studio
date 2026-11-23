#pragma once
#include "math_types.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace cpm {

// CPM2 hard server/engine quotas (CPM.Vinyls.Quota.VinylQuota.Limit)
static constexpr int kQuotaBody = 300;
static constexpr int kQuotaWindows = 150;
static constexpr int kQuotaPlate = 100;

enum SurfaceType : uint32_t {
    SURF_BODY = 0,
    SURF_WINDOWS = 1,
    SURF_PLATE = 2,
    SURF_STATIC = 3,
};

enum SurfaceMask : uint32_t {
    MASK_BODY = 1u << 0,
    MASK_WINDOWS = 1u << 1,
    MASK_PLATE = 1u << 2,
};

inline uint32_t packABGR(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

inline void unpackABGR(uint32_t c, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    r = (uint8_t)(c & 0xFF);
    g = (uint8_t)((c >> 8) & 0xFF);
    b = (uint8_t)((c >> 16) & 0xFF);
    a = (uint8_t)((c >> 24) & 0xFF);
}

struct SpriteEntry {
    int stickerId = 87;
    std::string name;
    float nativeW = 0.02450f;
    float nativeH = 0.02440f;
    int category = 0; // 0=solid, 1=airbrush, 2=curve
    bool allowFlipX = false;
    int arraySlice = 0; // index in GPU Texture2DArray
    std::vector<uint8_t> mask256; // 256x256 alpha mask
};

struct SpriteCatalog {
    std::vector<SpriteEntry> entries;
    std::unordered_map<int, int> idToSlot;

    bool load(const std::string& spritesV4Dir);
    const SpriteEntry* find(int stickerId) const {
        auto it = idToSlot.find(stickerId);
        return (it != idToSlot.end()) ? &entries[(size_t)it->second] : nullptr;
    }
    int sliceForSticker(int stickerId) const {
        auto it = idToSlot.find(stickerId);
        return (it != idToSlot.end()) ? entries[(size_t)it->second].arraySlice : 0;
    }
    void nativeSizeFor(int stickerId, float& nw, float& nh) const {
        auto it = idToSlot.find(stickerId);
        if (it != idToSlot.end()) {
            nw = entries[(size_t)it->second].nativeW;
            nh = entries[(size_t)it->second].nativeH;
        } else {
            nw = 0.02450f; nh = 0.02440f;
        }
    }
};

// Native CPM2 VinylData layer (matches build_livery.py & VinylData memory layout 1:1)
struct VinylLayer {
    float px = 0.0f;       // UV.x - 0.5 in [-0.5, +0.5]
    float py = 0.0f;       // UV.y - 0.5 in [-0.5, +0.5]
    int order = 0;         // Z order (0 = back, N-1 = front)
    float sx = 1.0f;       // localScale.x (negative = horizontal mirror)
    float sy = 1.0f;       // localScale.y (> 0)
    float rot = 0.0f;      // euler.z CCW degrees in [0, 360)
    uint32_t color = 0xFFFFFFFF; // 0xAABBGGRR
    int stickerIndex = 87; // V.AllVinyls index (1 for text)
    int xAngle = 151;      // 151 for body, 0 for windows/plates
    int yAngle = 0;
    float iconPos[3] = {0.0f, 0.72f, 1.15f};
    int isText = 0;
    int italic = 0;
    int bold = 0;
    int flipX = 0;
    int fontIndex = 0;     // 0..18 (0 = LegacyRuntime.ttf Unicode)
    std::string text;
};

// Shape in Decal Image Space (pixels [0..srcW] x [0..srcH], +Y down)
struct DecalShape {
    int sticker = 87;
    bool flipX = false;
    float cx = 0.0f, cy = 0.0f;
    float w = 10.0f, h = 10.0f;
    float angle = 0.0f; // clockwise degrees in image space (+Y down)
    uint8_t r = 255, g = 255, b = 255, a = 255;
    // Optional text/Unicode lineart primitive
    bool isText = false;
    int fontIndex = 0;
    bool bold = false;
    bool italic = false;
    std::string text;
};

struct EvalMetrics {
    bool valid = false;
    float allErr = 0.0f;         // legacy whole-frame RGB distance
    float fgErr = 0.0f;          // FOREGROUND-ONLY RGB distance (zero background dilution!)
    float edgeErr = 0.0f;        // contour / detail pixels RGB distance
    float flatErr = 0.0f;        // interior flat pixels RGB distance
    float silErr = 0.0f;         // silhouette rim RGB distance
    float ssimPct = 0.0f;        // 0..100 % Structural Similarity (8x8 windowed over foreground!)
    float focusSSIMPct = 0.0f;   // 0..100 % SSIM inside Focus / face-eyes zone
    float silIoUPct = 0.0f;      // 0..100 % exact silhouette Intersection-over-Union
    float spillPct = 0.0f;       // % of foreground area spilled outside silhouette
    float holesPct = 0.0f;       // % of foreground pixels left uncovered
    float fidelityPct = 0.0f;    // 0..100 % strict composite visual coherence score!
    int spillPx = 0;
    int holesPx = 0;
    int fgPx = 0;
    std::string verdict;         // "Fidele", "Correct", "Moyen", "Incoherent"
    std::string advice;          // concrete actionable diagnosis
};

enum BgMode : int {
    BG_AUTO = 0,
    BG_FLOOD = 1,
    BG_KEY = 2,
    BG_STICKER_WHITE = 3,
    BG_ALPHA = 4,
    BG_NONE = 5,
};

enum ConversionMode : int {
    CONV_HYBRID_V5 = 0,            // Engine v5 Hybride (Recommande: 10 solides + aerographes + courbes)
    CONV_TRIANGLES_CURVES = 1,     // Double Passe: Aplats Triangles (95) + Lineart Courbes natives
    CONV_TRIANGLES_UNICODE = 2,    // Double Passe: Aplats Triangles (95) + Lineart Unicode Texte (font 0)
};

struct PreprocessParams {
    int bgMode = BG_FLOOD;
    float bgTol = 45.0f;
    bool removeHoles = true;
    int erodePx = 1;
    bool stripWhiteBorder = false;
    int posterizeColors = 8;       // 0 = off, 4..16 clean cel-shading tones
    float contrastBoost = 1.12f;   // 0.6 .. 2.0
    float saturationBoost = 1.05f; // 0.0 .. 2.0
    int workSize = 400;            // 200 .. 512 per tile
    bool useFocus = false;
    float focusRect[4] = {0.25f, 0.25f, 0.75f, 0.75f}; // x0, y0, x1, y1 normalized
    float focusBoost = 4.0f;
};

struct TilingParams {
    int cols = 1;                  // 1..4
    int rows = 1;                  // 1..4
    int bleedPx = 4;               // overlap margin per tile to prevent seams
    bool autoBudgetByDensity = true;
    float manualWeights[16] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
    int lastTileBudgets[16] = {};
};

struct ConversionParams {
    int mode = CONV_HYBRID_V5;
    int totalShapes = 240;
    float flatPassRatio = 0.65f;   // used in Double Passe Triangles + Unicode mode
    float lineartThreshold = 0.20f;// Black-Hat valley threshold for Unicode lineart
    int candidates = 380;
    int mutations = 220;
    float bgPenalty = 2.5f;
    bool adaptiveAlpha = true;
    int seed = 9001;
    // Closed-loop Image-Comparator Auto-Converge settings
    bool autoConverge = true;      // iterate automatically using image-to-image error map feedback!
    float targetFidelityPct = 90.0f; // stop early once strict fidelity reaches this %
    int maxAutoPasses = 4;         // max closed-loop passes (1..6)
};

struct ConversionIteration {
    std::string label;
    int mode = CONV_HYBRID_V5;
    int srcW = 400, srcH = 400;
    EvalMetrics metrics;
    std::vector<DecalShape> shapes;
    std::vector<uint8_t> resultRGBA;
    std::vector<uint8_t> errorMapRGBA;
};

struct DecalTransform {
    // 3D Oriented Projection Volume (world meters, Unity LH coords)
    Vec3 pos{0.0f, 0.80f, 1.15f};
    Vec3 rotEuler{-90.0f, 0.0f, 0.0f}; // degrees: local -Z is projection direction! (rotX=-90 -> -Z points straight DOWN onto hood!)
    Vec3 scale{0.95f, 0.95f, 0.55f};  // width (m), height (m), projection half-depth (m)
    bool lockAspect = true;
    bool flipH = false;
    bool flipV = false;
    uint32_t targetMask = MASK_BODY | MASK_WINDOWS;
    bool autoSplitBodyWindows = true;
    int projectionMode = 0; // 0 = 3D Raycast Jacobian (Multi-Surface), 1 = Direct UV Planar
    // Direct UV mode parameters (used when projectionMode == 1)
    float uvCenter[2] = {0.245f, 0.411f};
    float uvHeight = 0.24f;
    float uvRotation = -90.0f;
    int uvSurface = SURF_BODY;

    Mat4 matrix() const {
        return Mat4::TRS(pos, rotEuler, scale);
    }
};

struct DecalItem {
    std::string name = "Nouveau Decal";
    bool enabled = true;
    std::string imagePath;
    std::string shapesJsonPath; // optional external shapes json

    // Original loaded RGBA image
    int origW = 0, origH = 0;
    std::vector<uint8_t> origRGBA;

    // Preprocessed preview RGBA image
    int prepW = 0, prepH = 0;
    std::vector<uint8_t> prepRGBA;
    bool prepDirty = true;

    PreprocessParams prep;
    TilingParams tiling;
    ConversionParams conv;
    DecalTransform xform;

    // Vectorized shapes in decal space (srcW x srcH)
    int srcW = 400, srcH = 400;
    std::vector<DecalShape> shapes;
    EvalMetrics metrics;

    // Visual diagnostic buffers (updated after preprocess / conversion)
    int previewMode = 0; // 0=Damier, 1=Sur Peinture, 2=Contours Black-Hat, 3=Resultat Vectorise, 4=Carte Erreur
    int lastContourCount = 0;
    std::vector<uint8_t> diagPreviewRGBA; // same size as prepW x prepH
    std::vector<uint8_t> resultRGBA;      // rendered vectorized preview on paint (srcW x srcH)
    std::vector<uint8_t> errorMapRGBA;    // pixel-by-pixel error heatmap (srcW x srcH)
    std::vector<ConversionIteration> history;
    int activeHistoryIdx = -1;

    // Live projected counts for this decal (updated by Projector)
    int projectedBodyCount = 0;
    int projectedWindowCount = 0;
    int projectedPlateCount = 0;
    int straddleSplitCount = 0; // shapes that automatically bridged Body <-> Windows!
};

struct DirectTextItem {
    std::string name = "Texte";
    bool enabled = true;
    std::string text = "CPM2";
    int fontIndex = 0;
    bool bold = false;
    bool italic = false;
    bool flipX = false;
    int surface = SURF_WINDOWS; // BODY / WINDOWS / PLATE
    float uvCenter[2] = {0.50f, 0.45f};
    float scale[2] = {2.5f, 2.5f};
    float rotation = 0.0f;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float iconPos[3] = {0.0f, 0.88f, 0.35f};
};

struct ProjectedLivery {
    std::vector<VinylLayer> bodyLayers;
    std::vector<VinylLayer> windowLayers;
    std::vector<VinylLayer> plateLayers;
    int rawBodyCount = 0;    // before hard clamp at 300
    int rawWindowCount = 0;  // before hard clamp at 150
    int rawPlateCount = 0;   // before hard clamp at 100
};

struct LiveryProject {
    int carId = 187;
    std::vector<int> kitVariants; // selected variant index per kitGroup
    float paintColor[3] = {0.77f, 0.09f, 0.11f}; // #C4161C
    float paintMetallic = 0.35f;
    float paintRoughness = 0.18f;
    float windowTint[4] = {0.06f, 0.08f, 0.11f, 0.58f};

    std::vector<DecalItem> decals;
    std::vector<DirectTextItem> texts;

    // Optional imported raw CPM2 layer files (e.g. existing car187_body_layers.txt)
    bool showImportedLayers = true;
    std::vector<VinylLayer> importedBody;
    std::vector<VinylLayer> importedWindows;
    std::vector<VinylLayer> importedPlate;
};

} // namespace cpm
