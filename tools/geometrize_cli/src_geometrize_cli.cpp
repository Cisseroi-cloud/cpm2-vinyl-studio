// CPM2 Vinyls Pro Anime Layered Vectorizer — Engine v5
//
// Upgrades addressing all root causes from DIAGNOSTIC.md:
//   1. [C3] High-resolution 256x256 sprite masks (*.a256) with fast fixed-point bilinear
//      sampling and automatic distance-transform stroke spine anchors (anchorX, anchorY,
//      tangentDeg, thicknessRatio) so curved arcs/crescents/rings pivot and spawn on
//      their actual opaque stroke centerline instead of empty bounding-box air.
//   2. [C6] Exact Premultiplied-RGBA associative compositing + color-unbiased background
//      alpha-spill penalty + alpha-complete foreground objective (eliminates gray border
//      bias and unpremultiplied edge explosion on dark outlines & white borders).
//   3. [C2] Preserves true line-art contours (coarse base fills only for first 16% of budget,
//      then 84% true-target contour + detail optimization with residual-focused ink color
//      solver and sub-pixel hairline support down to 0.5 px).
//   4. [C1] Top-to-Bottom Exact Full-Stack Refinement Sweep + Gauss-Newton Analytical
//      Sub-Pixel Gradient Line-Search + Anchor-Pivoted Pattern Search & Closed-Form
//      Occlusion-Aware Optimal Color Solver.
//
// Usage:
//   geometrize_cli input.rgba W H out.json [shapeCount] [alpha] [candidates] [mutations] [threads] [seed] [bgPenalty] [previewRawOut] [weightsRawIn] [adaptiveAlpha] [spritesDir] [colorTargetRaw] [contoursBin]

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <future>
#include <random>
#include <string>
#include <thread>
#include <vector>

static constexpr float kPi = 3.14159265358979323846f;
static constexpr int kMaskSize = 256;

struct RGBA {
    std::uint8_t r, g, b, a;
};

struct AbovePix {
    float r, g, b, t; // premultiplied RGB of upper layers + transmittance t in [0, 1]
};

struct SpriteDef {
    int stickerId;
    int category; // 0=solid, 1=airbrush_grad, 2=contour_curve
    bool flipX;
    float anchorX = 0.0f;       // normalized [-0.5, +0.5] stroke spine anchor X
    float anchorY = 0.0f;       // normalized [-0.5, +0.5] stroke spine anchor Y
    float tangentDeg = 90.0f;   // local stroke tangent angle in mask space
    float thickRatio = 0.75f;   // median stroke width fraction of mask width
    std::uint8_t mask[kMaskSize * kMaskSize];
    // Pre-smoothed mask gradients (in [-255..255]) for wide-capture Gauss-Newton refinement
    std::int16_t gradU[kMaskSize * kMaskSize];
    std::int16_t gradV[kMaskSize * kMaskSize];
};

struct ContourSeg {
    float cx, cy, length, thickness, angleDeg, curvature;
};

struct Shape {
    int spriteSlot = 0;
    float cx = 0.0f;
    float cy = 0.0f;
    float w = 10.0f;
    float h = 10.0f;
    float angle = 0.0f; // clockwise degrees in image space (+Y down)
    RGBA color{0, 0, 0, 255}; // straight RGBA color of shape
};

static bool readFile(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    if (n <= 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize((size_t)n);
    f.read(reinterpret_cast<char*>(out.data()), n);
    return f.good();
}

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline std::uint8_t clampU8(int v) {
    return (std::uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Compute stroke spine anchor, local tangent angle, thickness ratio, and smooth spatial derivatives
static void analyzeSpriteMask(SpriteDef& sd) {
    // 1. Iterative L1 distance transform on opaque mask (mask >= 170)
    std::vector<int> dist(kMaskSize * kMaskSize, 0);
    double sumX = 0.0, sumY = 0.0, sumW = 0.0;
    int maxD = 0;
    for (int y = 0; y < kMaskSize; ++y) {
        for (int x = 0; x < kMaskSize; ++x) {
            const int idx = (y << 8) + x;
            const std::uint8_t a = sd.mask[idx];
            if (a >= 128) {
                sumX += (double)x * a;
                sumY += (double)y * a;
                sumW += (double)a;
            }
            if (a >= 170) {
                int up = (y > 0) ? dist[((y - 1) << 8) + x] : 0;
                int left = (x > 0) ? dist[(y << 8) + (x - 1)] : 0;
                dist[idx] = 1 + std::min(up, left);
            }
        }
    }
    for (int y = kMaskSize - 1; y >= 0; --y) {
        for (int x = kMaskSize - 1; x >= 0; --x) {
            const int idx = (y << 8) + x;
            if (dist[idx] > 0) {
                int dn = (y + 1 < kMaskSize) ? dist[((y + 1) << 8) + x] : 0;
                int rt = (x + 1 < kMaskSize) ? dist[(y << 8) + (x + 1)] : 0;
                dist[idx] = std::min(dist[idx], 1 + std::min(dn, rt));
                if (dist[idx] > maxD) maxD = dist[idx];
            }
        }
    }

    if (sumW > 0.0 && maxD > 0) {
        const double cx = sumX / sumW;
        const double cy = sumY / sumW;
        const int threshD = std::max(1, (int)std::lround(maxD * 0.72));
        int bestX = kMaskSize / 2, bestY = kMaskSize / 2;
        double bestDist2 = 1e30;
        for (int y = 0; y < kMaskSize; ++y) {
            for (int x = 0; x < kMaskSize; ++x) {
                if (dist[(y << 8) + x] >= threshD) {
                    double d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                    if (d2 < bestDist2) {
                        bestDist2 = d2;
                        bestX = x;
                        bestY = y;
                    }
                }
            }
        }
        sd.anchorX = ((float)bestX - 128.0f) / 256.0f;
        sd.anchorY = ((float)bestY - 128.0f) / 256.0f;
        sd.thickRatio = clampf((float)(2 * maxD) / 256.0f, 0.08f, 1.0f);

        // Local principal axis tangent within radius 52 texels of anchor
        double mx = 0.0, my = 0.0, mw = 0.0;
        for (int y = std::max(0, bestY - 52); y <= std::min(kMaskSize - 1, bestY + 52); ++y) {
            for (int x = std::max(0, bestX - 52); x <= std::min(kMaskSize - 1, bestX + 52); ++x) {
                if ((x - bestX) * (x - bestX) + (y - bestY) * (y - bestY) <= 52 * 52) {
                    double w = (double)sd.mask[(y << 8) + x];
                    if (w >= 128.0) { mx += w * x; my += w * y; mw += w; }
                }
            }
        }
        if (mw > 0.0) {
            mx /= mw; my /= mw;
            double cxx = 0.0, cyy = 0.0, cxy = 0.0;
            for (int y = std::max(0, bestY - 52); y <= std::min(kMaskSize - 1, bestY + 52); ++y) {
                for (int x = std::max(0, bestX - 52); x <= std::min(kMaskSize - 1, bestX + 52); ++x) {
                    if ((x - bestX) * (x - bestX) + (y - bestY) * (y - bestY) <= 52 * 52) {
                        double w = (double)sd.mask[(y << 8) + x];
                        if (w >= 128.0) {
                            double dx = x - mx, dy = y - my;
                            cxx += w * dx * dx;
                            cyy += w * dy * dy;
                            cxy += w * dx * dy;
                        }
                    }
                }
            }
            sd.tangentDeg = (float)(0.5 * std::atan2(2.0 * cxy, cxx - cyy) * (180.0 / kPi));
        }
    }

    // 2. Smooth 5-tap spatial derivative of mask for wide-capture Gauss-Newton edge alignment
    // Box-blur 3x3 first so derivatives span a ~4-texel capture band
    std::vector<int> blur(kMaskSize * kMaskSize, 0);
    for (int y = 0; y < kMaskSize; ++y) {
        for (int x = 0; x < kMaskSize; ++x) {
            int sum = 0;
            for (int dy = -2; dy <= 2; ++dy) {
                int yy = std::max(0, std::min(kMaskSize - 1, y + dy));
                for (int dx = -2; dx <= 2; ++dx) {
                    int xx = std::max(0, std::min(kMaskSize - 1, x + dx));
                    sum += sd.mask[(yy << 8) + xx];
                }
            }
            blur[(y << 8) + x] = sum / 25;
        }
    }
    for (int y = 0; y < kMaskSize; ++y) {
        for (int x = 0; x < kMaskSize; ++x) {
            int x0 = std::max(0, x - 2), x1 = std::min(kMaskSize - 1, x + 2);
            int y0 = std::max(0, y - 2), y1 = std::min(kMaskSize - 1, y + 2);
            sd.gradU[(y << 8) + x] = (std::int16_t)((blur[(y << 8) + x1] - blur[(y << 8) + x0]) / std::max(1, x1 - x0));
            sd.gradV[(y << 8) + x] = (std::int16_t)((blur[(y1 << 8) + x] - blur[(y0 << 8) + x]) / std::max(1, y1 - y0));
        }
    }
}

static bool loadSpritesV5(const std::string& dir, std::vector<SpriteDef>& out) {
    struct Spec { int id; int cat; bool allowFlip; };
    const Spec specs[] = {
        { 87,  0, false }, // square
        { 89,  0, false }, // circle
        { 95,  0, false }, // triangle
        { 97,  0, false }, // almond
        { 5,   0, false }, // pentagon
        { 6,   0, false }, // diamond
        { 4,   0, true  }, // parallelogram
        { 9,   0, false }, // sharp teardrop
        { 103, 0, false }, // heart
        { 125, 1, false }, // radial airbrush glow
        { 124, 1, false }, // linear bar fade
        { 18,  1, true  }, // one-sided fade
        { 127, 1, true  }, // soft shaded crescent
        { 128, 1, true  }, // soft tapered stroke
        { 130, 2, true  }, // sharp tapered crescent
        { 101, 2, false }, // thin C-arc
        { 100, 2, true  }, // thick quarter-arc
        { 90,  2, false }, // thin ring
        { 11,  2, false }, // thick donut ring
        { 8,   2, true  }, // S-wave ribbon
    };
    out.clear();
    for (const auto& sp : specs) {
        std::string p256 = dir + "\\sprite_" + std::to_string(sp.id) + ".a256";
        std::vector<std::uint8_t> raw;
        SpriteDef sd{};
        sd.stickerId = sp.id;
        sd.category = sp.cat;
        sd.flipX = false;

        if (readFile(p256, raw) && raw.size() == (size_t)(kMaskSize * kMaskSize)) {
            std::memcpy(sd.mask, raw.data(), kMaskSize * kMaskSize);
        } else {
            // Fallback: bilinearly upsample 64x64 mask to 256x256
            std::string p64 = dir + "\\sprite_" + std::to_string(sp.id) + ".a64";
            if (!readFile(p64, raw) || raw.size() != 64u * 64u) continue;
            for (int y = 0; y < kMaskSize; ++y) {
                float fy = ((float)y + 0.5f) * (64.0f / 256.0f) - 0.5f;
                int y0 = std::max(0, std::min(63, (int)std::floor(fy)));
                int y1 = std::min(63, y0 + 1);
                float wy = clampf(fy - (float)y0, 0.0f, 1.0f);
                for (int x = 0; x < kMaskSize; ++x) {
                    float fx = ((float)x + 0.5f) * (64.0f / 256.0f) - 0.5f;
                    int x0 = std::max(0, std::min(63, (int)std::floor(fx)));
                    int x1 = std::min(63, x0 + 1);
                    float wx = clampf(fx - (float)x0, 0.0f, 1.0f);
                    float v0 = (1.0f - wx) * raw[y0 * 64 + x0] + wx * raw[y0 * 64 + x1];
                    float v1 = (1.0f - wx) * raw[y1 * 64 + x0] + wx * raw[y1 * 64 + x1];
                    sd.mask[(y << 8) + x] = clampU8((int)std::lround((1.0f - wy) * v0 + wy * v1));
                }
            }
        }
        analyzeSpriteMask(sd);
        out.push_back(sd);

        if (sp.allowFlip) {
            SpriteDef mf{};
            mf.stickerId = sp.id;
            mf.category = sp.cat;
            mf.flipX = true;
            for (int y = 0; y < kMaskSize; ++y) {
                for (int x = 0; x < kMaskSize; ++x) {
                    mf.mask[(y << 8) + x] = sd.mask[(y << 8) + (kMaskSize - 1 - x)];
                }
            }
            analyzeSpriteMask(mf);
            out.push_back(mf);
        }
    }
    if (out.empty()) {
        SpriteDef sq{}; sq.stickerId = 87; sq.category = 0; sq.flipX = false;
        std::memset(sq.mask, 255, sizeof(sq.mask));
        analyzeSpriteMask(sq);
        out.push_back(sq);
    }
    return true;
}

static void loadContours(const std::string& path, std::vector<ContourSeg>& out) {
    out.clear();
    std::vector<std::uint8_t> raw;
    if (path.empty() || path == "none" || !readFile(path, raw) || raw.size() < 4) return;
    std::uint32_t count = 0;
    std::memcpy(&count, raw.data(), 4);
    const size_t expected = 4u + (size_t)count * sizeof(ContourSeg);
    if (raw.size() < expected || count > 500000) return;
    out.resize(count);
    if (count > 0) std::memcpy(out.data(), raw.data() + 4, (size_t)count * sizeof(ContourSeg));
}

// Convert between bounding-box center (s.cx, s.cy) and actual opaque stroke spine anchor (px, py)
static inline void shapeGetAnchor(const Shape& s, const SpriteDef& sd, float& outPx, float& outPy) {
    const float rad = s.angle * (kPi / 180.0f);
    const float c = std::cos(rad), sn = std::sin(rad);
    const float lx = sd.anchorX * s.w;
    const float ly = sd.anchorY * s.h;
    outPx = s.cx + (lx * c - ly * sn);
    outPy = s.cy + (lx * sn + ly * c);
}

static inline void shapeSetAnchor(Shape& s, const SpriteDef& sd, float px, float py) {
    const float rad = s.angle * (kPi / 180.0f);
    const float c = std::cos(rad), sn = std::sin(rad);
    const float lx = sd.anchorX * s.w;
    const float ly = sd.anchorY * s.h;
    s.cx = px - (lx * c - ly * sn);
    s.cy = py - (lx * sn + ly * c);
}

// Enforce natural geometric bounds per primitive family so airbrushes, hearts, parallelograms,
// and closed rings NEVER degenerate into 30:1 aliased needle scars across faces/chests,
// while allowing true open line-art brushes (130, 101, 100, 8, 128, 9, 97, 87, 89) full sharpness!
static inline void clampShapeGeometry(Shape& s, const SpriteDef& sd, int W, int H) {
    float minDim = 0.90f;
    float maxAspect = 18.0f;

    if (sd.stickerId == 125 || sd.stickerId == 124 || sd.stickerId == 18 || sd.stickerId == 127) {
        // Soft airbrush gradients: strictly soft shading/blush, never thin razor lines!
        minDim = (sd.stickerId == 125) ? 6.5f : 5.0f;
        maxAspect = (sd.stickerId == 125) ? 2.6f : 4.2f;
    } else if (sd.stickerId == 103 || sd.stickerId == 5 || sd.stickerId == 95 || sd.stickerId == 6 || sd.stickerId == 4) {
        // Hearts, pentagons, triangles, diamonds, sheared parallelograms:
        // cap aspect ratio so sheared parallelogram tips and heart lobes never become 25:1 needle spikes!
        minDim = 2.0f;
        maxAspect = (sd.stickerId == 103 || sd.stickerId == 5) ? 2.8f : 4.2f;
    } else if (sd.stickerId == 90 || sd.stickerId == 11) {
        // Closed 360-degree rings: strictly small, nearly-circular eye/iris/earring details!
        minDim = 4.5f;
        maxAspect = 1.45f;
        s.w = clampf(s.w, minDim, 28.0f);
        s.h = clampf(s.h, minDim, 28.0f);
    } else if (sd.stickerId == 101) {
        // Thin C-arc (ribbon is 13% of W)
        minDim = 2.2f;
        maxAspect = 14.0f;
    } else if (sd.stickerId == 130 || sd.stickerId == 100 || sd.stickerId == 128) {
        // Tapered crescent, quarter-arc, tapered stroke: primary curved ink brushes!
        minDim = 1.35f;
        maxAspect = 18.0f;
    } else {
        // Solid stroke & curve primitives (87, 89, 97, 9, 8)
        minDim = 0.90f;
        maxAspect = 18.0f;
    }

    s.w = clampf(s.w, minDim, (float)W * 1.05f);
    s.h = clampf(s.h, minDim, (float)H * 1.05f);

    if (s.w > s.h * maxAspect) s.w = s.h * maxAspect;
    if (s.h > s.w * maxAspect) s.h = s.w * maxAspect;
}

// Fast 8-bit fixed-point bilinear lookup on 256x256 mask
static inline std::uint32_t sampleMaskBilinear(const std::uint8_t* mask, float u, float v) {
    const float uf = u - 0.5f;
    const float vf = v - 0.5f;
    if (uf < 0.0f || uf >= 255.0f || vf < 0.0f || vf >= 255.0f) {
        const int iu = (int)u;
        const int iv = (int)v;
        if ((unsigned)iu < 256u && (unsigned)iv < 256u) return mask[(iv << 8) + iu];
        return 0u;
    }
    const int iu = (int)uf;
    const int iv = (int)vf;
    const int fu = (int)((uf - (float)iu) * 256.0f);
    const int fv = (int)((vf - (float)iv) * 256.0f);
    const std::uint8_t* p = mask + (iv << 8) + iu;
    const int m0 = ((int)p[0] << 8) + fu * ((int)p[1] - (int)p[0]);
    const int m1 = ((int)p[256] << 8) + fu * ((int)p[257] - (int)p[256]);
    return (std::uint32_t)(((m0 << 8) + fv * (m1 - m0) + 32768) >> 16);
}

struct AABB {
    int x0, y0, x1, y1;
    float cosA, sinA, invW, invH;
    bool empty;
};

static inline AABB computeAABB(const Shape& s, int W, int H) {
    AABB b{};
    if (s.w < 0.55f || s.h < 0.55f) { b.empty = true; return b; }
    const float rad = s.angle * (kPi / 180.0f);
    b.cosA = std::cos(rad);
    b.sinA = std::sin(rad);
    const float hw = s.w * 0.5f + 0.75f;
    const float hh = s.h * 0.5f + 0.75f;
    const float ex = std::fabs(b.cosA) * hw + std::fabs(b.sinA) * hh;
    const float ey = std::fabs(b.sinA) * hw + std::fabs(b.cosA) * hh;
    b.x0 = std::max(0, (int)std::floor(s.cx - ex));
    b.y0 = std::max(0, (int)std::floor(s.cy - ey));
    b.x1 = std::min(W - 1, (int)std::ceil(s.cx + ex));
    b.y1 = std::min(H - 1, (int)std::ceil(s.cy + ey));
    if (b.x0 > b.x1 || b.y0 > b.y1) { b.empty = true; return b; }
    b.invW = (float)kMaskSize / s.w;
    b.invH = (float)kMaskSize / s.h;
    b.empty = false;
    return b;
}

// Pixel error in Premultiplied RGBA space:
// - Background (t.a == 0): color-unbiased alpha spill penalty `effBgPenalty * 4.0 * c.a^2`
// - Foreground (t.a > 0):  `weights[idx] * ((t.r - c.r)^2 + (t.g - c.g)^2 + (t.b - c.b)^2 + 3*(t.a - c.a)^2)`
static inline double pixelErrorPremul(RGBA t, RGBA c, float w, double effBgPenalty) {
    if (t.a == 0) {
        return effBgPenalty * 4.0 * (double)((int)c.a * (int)c.a);
    }
    const int dr = (int)t.r - (int)c.r;
    const int dg = (int)t.g - (int)c.g;
    const int db = (int)t.b - (int)c.b;
    const int da = (int)t.a - (int)c.a;
    return (double)w * (double)(dr * dr + dg * dg + db * db + 3 * da * da);
}

// Evaluate shape `s` at any layer slot (with `below` = composite of 0..i-1, and optional `above` = composite of i+1..N-1).
// When `above == nullptr`, `s` is evaluated at the top of the stack (fT = 1, fRGB = 0).
static double evalShapeExact(
    const Shape& s,
    const std::vector<SpriteDef>& sprites,
    const std::vector<RGBA>& target, // premultiplied RGBA
    const std::vector<RGBA>& below,  // premultiplied RGBA
    const AbovePix* above,           // optional upper layers (nullptr if top of stack)
    const std::vector<float>& weights,
    int W, int H,
    double effBgPenalty,
    std::uint8_t baseAlpha,
    bool adaptiveAlpha,
    bool useExistingColorOnly,
    RGBA& outColor)
{
    const AABB b = computeAABB(s, W, H);
    if (b.empty) return 1e30;
    const SpriteDef& sd = sprites[s.spriteSlot];
    const std::uint8_t* mask = sd.mask;
    const bool isSoftGrad = (sd.category == 1);

    // If shape is thinner than 0.45 pixels along its stroke ribbon, reject hollow sprites to prevent sub-pixel skipping
    if (sd.thickRatio * std::min(s.w, s.h) < 0.45f) return 1e30;

    const float du_dx = b.cosA * b.invW;
    const float dv_dx = -b.sinA * b.invH;
    const float du_dy = b.sinA * b.invW;
    const float dv_dy = b.cosA * b.invH;

    const float dx0 = (float)b.x0 + 0.5f - s.cx;
    const float dy0 = (float)b.y0 + 0.5f - s.cy;
    const float uRow0 = (dx0 * b.cosA + dy0 * b.sinA) * b.invW + (float)kMaskSize * 0.5f;
    const float vRow0 = (-dx0 * b.sinA + dy0 * b.cosA) * b.invH + (float)kMaskSize * 0.5f;

    // Single-pass exact quadratic error decomposition over AABB:
    // For any candidate opaque RGB color C = (Cr, Cg, Cb) at fixed shape alpha `colA`:
    //   delta(C) = deltaAlphaBg + D0 + sum_{c in R,G,B} (A2 * C_c^2 - 2 * B1_c * C_c)
    // This evaluates the EXACT weighted error AND solves for the optimal color in ONE single pass!
    auto accumulateQuadratic = [&](std::uint32_t colA,
                                   double& outDeltaAlphaBg,
                                   double& outA2,
                                   double& outB1R, double& outB1G, double& outB1B,
                                   double& outD0,
                                   double& outUnwDen,
                                   double& outUnwR, double& outUnwG, double& outUnwB,
                                   int& outFgCount,
                                   bool& outAllCovered) -> bool {
        outDeltaAlphaBg = 0.0;
        outA2 = 0.0;
        outB1R = 0.0; outB1G = 0.0; outB1B = 0.0;
        outD0 = 0.0;
        outUnwDen = 0.0;
        outUnwR = 0.0; outUnwG = 0.0; outUnwB = 0.0;
        outFgCount = 0;
        outAllCovered = true;

        float ur = uRow0, vr = vRow0;
        const double wBg = effBgPenalty * 4.0;
        for (int y = b.y0; y <= b.y1; ++y) {
            float u = ur, v = vr;
            const int rowOff = y * W;
            for (int x = b.x0; x <= b.x1; ++x) {
                if (u >= 0.0f && u < 256.0f && v >= 0.0f && v < 256.0f) {
                    const std::uint32_t ma = sampleMaskBilinear(mask, u, v);
                    if (ma >= 4u) {
                        const int idx = rowOff + x;
                        const float fT = above ? above[idx].t : 1.0f;
                        if (fT > 0.002f) {
                            const RGBA t = target[idx];
                            const RGBA bel = below[idx];
                            const std::uint32_t sa = (colA * ma + 127u) / 255u;
                            const double a = (double)sa * (1.0 / 255.0);
                            const double k = (double)fT * a;
                            const double i0A = (above ? 255.0 * (1.0 - (double)fT) : 0.0) + (double)fT * (double)bel.a;
                            const double dBelA = 255.0 - (double)bel.a;
                            const double i1A = i0A + k * dBelA;

                            if (t.a == 0) {
                                outAllCovered = false;
                                if (isSoftGrad) return false;
                                outDeltaAlphaBg += wBg * (i1A * i1A - i0A * i0A);
                            } else {
                                outFgCount++;
                                if (bel.a < 250) {
                                    outAllCovered = false;
                                    if (isSoftGrad) return false;
                                }
                                const double wp = (double)weights[idx];
                                const double r0A = i0A - (double)t.a;
                                const double r1A = i1A - (double)t.a;
                                outDeltaAlphaBg += wp * 3.0 * (r1A * r1A - r0A * r0A);

                                const double i0R = (above ? (double)above[idx].r : 0.0) + (double)fT * (double)bel.r;
                                const double i0G = (above ? (double)above[idx].g : 0.0) + (double)fT * (double)bel.g;
                                const double i0B = (above ? (double)above[idx].b : 0.0) + (double)fT * (double)bel.b;

                                // Residual needed from layer i: R_c = t_c - i0_c
                                const double rR = (double)t.r - i0R;
                                const double rG = (double)t.g - i0G;
                                const double rB = (double)t.b - i0B;

                                // New error - Old error for channel c:
                                // (k*(C_c - bel_c) - r_c)^2 - r_c^2 = k^2 * C_c^2 - 2*k*(r_c + k*bel_c)*C_c + k*bel_c*(k*bel_c + 2*r_c)
                                const double k2 = k * k;
                                const double kbR = k * (double)bel.r;
                                const double kbG = k * (double)bel.g;
                                const double kbB = k * (double)bel.b;
                                const double yR = rR + kbR;
                                const double yG = rG + kbG;
                                const double yB = rB + kbB;

                                outA2  += wp * k2;
                                outB1R += wp * k * yR;
                                outB1G += wp * k * yG;
                                outB1B += wp * k * yB;
                                outD0  += wp * (kbR * (kbR + 2.0 * rR) + kbG * (kbG + 2.0 * rG) + kbB * (kbB + 2.0 * rB));

                                outUnwDen += k2;
                                outUnwR   += k * yR;
                                outUnwG   += k * yG;
                                outUnwB   += k * yB;
                            }
                        }
                    }
                }
                u += du_dx;
                v += dv_dx;
            }
            ur += du_dy;
            vr += dv_dy;
        }
        return (outFgCount > 0 && outA2 > 1e-9);
    };

    double deltaAB = 0.0, A2 = 0.0, B1R = 0.0, B1G = 0.0, B1B = 0.0, D0 = 0.0;
    double unwDen = 0.0, unwR = 0.0, unwG = 0.0, unwB = 0.0;
    int fgCount = 0;
    bool allCovered = true;
    if (!accumulateQuadratic(useExistingColorOnly ? s.color.a : baseAlpha,
                             deltaAB, A2, B1R, B1G, B1B, D0, unwDen, unwR, unwG, unwB, fgCount, allCovered)) {
        return 1e30;
    }

    auto evalQuadColor = [&](RGBA col) -> double {
        const double cr = (double)col.r, cg = (double)col.g, cb = (double)col.b;
        return deltaAB + D0
             + cr * (A2 * cr - 2.0 * B1R)
             + cg * (A2 * cg - 2.0 * B1G)
             + cb * (A2 * cb - 2.0 * B1B);
    };

    if (useExistingColorOnly) {
        outColor = s.color;
        return evalQuadColor(s.color);
    }

    // Unweighted least-squares color (NEVER greyed or muddied by high-weight ink lines!):
    RGBA cUnw{
        clampU8((int)std::lround(unwR / std::max(1e-9, unwDen))),
        clampU8((int)std::lround(unwG / std::max(1e-9, unwDen))),
        clampU8((int)std::lround(unwB / std::max(1e-9, unwDen))),
        baseAlpha
    };
    double bestDelta = evalQuadColor(cUnw);
    RGBA bestCol = cUnw;

    // For thin contour strokes & fine eye details (effective stroke thickness <= 5.5 px),
    // also test the saliency-weighted quadratic minimizer cOpt so thin strokes lock onto dark ink / bright highlights!
    const float effThick = sd.thickRatio * std::min(s.w, s.h);
    if (effThick <= 5.5f) {
        RGBA cOpt{
            clampU8((int)std::lround(B1R / A2)),
            clampU8((int)std::lround(B1G / A2)),
            clampU8((int)std::lround(B1B / A2)),
            baseAlpha
        };
        const double dOpt = evalQuadColor(cOpt);
        if (dOpt < bestDelta) {
            bestDelta = dOpt;
            bestCol = cOpt;
        }
    }

    // Optional soft opacity (alpha=145) ONLY for already-covered interior shading when it beats opaque
    if (adaptiveAlpha && allCovered && baseAlpha == 255 && !isSoftGrad) {
        const std::uint8_t softA = 145;
        const double scaleA = 255.0 / (double)softA;
        // Under uniform alpha scaling by r = softA/255:
        // k_soft = r * k => A2_soft = r^2 * A2, and B1_soft = r * sum(wp*k*r_c) + r^2 * sum(wp*k^2*bel_c)
        // Approximate optimal soft color and evaluate if promising:
        RGBA cSoft{
            clampU8((int)std::lround((B1R / A2) * scaleA - (scaleA - 1.0) * 128.0)),
            clampU8((int)std::lround((B1G / A2) * scaleA - (scaleA - 1.0) * 128.0)),
            clampU8((int)std::lround((B1B / A2) * scaleA - (scaleA - 1.0) * 128.0)),
            softA
        };
        double sAB = 0.0, sA2 = 0.0, sB1R = 0.0, sB1G = 0.0, sB1B = 0.0, sD0 = 0.0, suD = 0.0, suR = 0.0, suG = 0.0, suB = 0.0;
        int sFg = 0; bool sCov = true;
        if (accumulateQuadratic(softA, sAB, sA2, sB1R, sB1G, sB1B, sD0, suD, suR, suG, suB, sFg, sCov)) {
            cSoft = RGBA{
                clampU8((int)std::lround(sB1R / sA2)),
                clampU8((int)std::lround(sB1G / sA2)),
                clampU8((int)std::lround(sB1B / sA2)),
                softA
            };
            const double cr = cSoft.r, cg = cSoft.g, cb = cSoft.b;
            const double dSoft = sAB + sD0 + cr * (sA2 * cr - 2.0 * sB1R) + cg * (sA2 * cg - 2.0 * sB1G) + cb * (sA2 * cb - 2.0 * sB1B);
            if (dSoft < bestDelta - 1.5e4) {
                bestDelta = dSoft;
                bestCol = cSoft;
            }
        }
    }

    outColor = bestCol;
    return bestDelta;
}

// Draw shape `s` into Premultiplied RGBA buffer `img`
static void drawShapePremul(
    const Shape& s,
    const std::vector<SpriteDef>& sprites,
    std::vector<RGBA>& img,
    int W, int H)
{
    const AABB b = computeAABB(s, W, H);
    if (b.empty) return;
    const std::uint8_t* mask = sprites[s.spriteSlot].mask;
    const float du_dx = b.cosA * b.invW;
    const float dv_dx = -b.sinA * b.invH;
    const float du_dy = b.sinA * b.invW;
    const float dv_dy = b.cosA * b.invH;
    const float dx0 = (float)b.x0 + 0.5f - s.cx;
    const float dy0 = (float)b.y0 + 0.5f - s.cy;
    float ur = (dx0 * b.cosA + dy0 * b.sinA) * b.invW + (float)kMaskSize * 0.5f;
    float vr = (-dx0 * b.sinA + dy0 * b.cosA) * b.invH + (float)kMaskSize * 0.5f;

    const std::uint32_t colA = s.color.a;
    for (int y = b.y0; y <= b.y1; ++y) {
        float u = ur, v = vr;
        const int rowOff = y * W;
        for (int x = b.x0; x <= b.x1; ++x) {
            if (u >= 0.0f && u < 256.0f && v >= 0.0f && v < 256.0f) {
                const std::uint32_t ma = sampleMaskBilinear(mask, u, v);
                if (ma >= 4u) {
                    const std::uint32_t sa = (colA * ma + 127u) / 255u;
                    const std::uint32_t invSa = 255u - sa;
                    const int idx = rowOff + x;
                    const RGBA cur = img[idx];
                    img[idx] = RGBA{
                        (std::uint8_t)((cur.r * invSa + s.color.r * sa + 127u) / 255u),
                        (std::uint8_t)((cur.g * invSa + s.color.g * sa + 127u) / 255u),
                        (std::uint8_t)((cur.b * invSa + s.color.b * sa + 127u) / 255u),
                        (std::uint8_t)((cur.a * invSa + 255u      * sa + 127u) / 255u)
                    };
                }
            }
            u += du_dx; v += dv_dx;
        }
        ur += du_dy; vr += dv_dy;
    }
}

// Accumulate shape `s` front-to-back into `above` (used during Top-to-Bottom sweep i = N-1..0)
static void foldShapeIntoAbove(
    const Shape& s,
    const std::vector<SpriteDef>& sprites,
    std::vector<AbovePix>& above,
    int W, int H)
{
    const AABB b = computeAABB(s, W, H);
    if (b.empty) return;
    const std::uint8_t* mask = sprites[s.spriteSlot].mask;
    const float du_dx = b.cosA * b.invW;
    const float dv_dx = -b.sinA * b.invH;
    const float du_dy = b.sinA * b.invW;
    const float dv_dy = b.cosA * b.invH;
    const float dx0 = (float)b.x0 + 0.5f - s.cx;
    const float dy0 = (float)b.y0 + 0.5f - s.cy;
    float ur = (dx0 * b.cosA + dy0 * b.sinA) * b.invW + (float)kMaskSize * 0.5f;
    float vr = (-dx0 * b.sinA + dy0 * b.cosA) * b.invH + (float)kMaskSize * 0.5f;

    const float colAf = (float)s.color.a / 255.0f;
    for (int y = b.y0; y <= b.y1; ++y) {
        float u = ur, v = vr;
        const int rowOff = y * W;
        for (int x = b.x0; x <= b.x1; ++x) {
            if (u >= 0.0f && u < 256.0f && v >= 0.0f && v < 256.0f) {
                const std::uint32_t ma = sampleMaskBilinear(mask, u, v);
                if (ma >= 4u) {
                    const float a = colAf * ((float)ma / 255.0f);
                    const int idx = rowOff + x;
                    AbovePix& ab = above[idx];
                    const float k = ab.t * a;
                    ab.r += k * (float)s.color.r;
                    ab.g += k * (float)s.color.g;
                    ab.b += k * (float)s.color.b;
                    ab.t *= (1.0f - a);
                }
            }
            u += du_dx; v += dv_dx;
        }
        ur += du_dy; vr += dv_dy;
    }
}

static void renderAllPremul(
    const std::vector<Shape>& shapes,
    const std::vector<SpriteDef>& sprites,
    std::vector<RGBA>& img,
    int W, int H,
    RGBA bg)
{
    std::fill(img.begin(), img.end(), bg);
    for (const auto& s : shapes) drawShapePremul(s, sprites, img, W, H);
}

static double totalWeightedError(
    const std::vector<RGBA>& target,
    const std::vector<RGBA>& current,
    const std::vector<float>& weights,
    double effBgPenalty)
{
    double tot = 0.0;
    for (size_t i = 0; i < target.size(); ++i) {
        tot += pixelErrorPremul(target[i], current[i], weights[i], effBgPenalty);
    }
    return tot;
}

static void buildErrorCdf(
    const std::vector<RGBA>& target,
    const std::vector<RGBA>& current,
    const std::vector<float>& weights,
    double effBgPenalty,
    std::vector<double>& cdf)
{
    double run = 0.0;
    for (size_t i = 0; i < target.size(); ++i) {
        const RGBA t = target[i];
        if (t.a > 0) {
            run += pixelErrorPremul(t, current[i], weights[i], effBgPenalty);
        }
        cdf[i] = run;
    }
}

struct SpriteBuckets {
    std::vector<int> solid;
    std::vector<int> grad;
    std::vector<int> curve;
    std::vector<int> all;
};

static SpriteBuckets makeBuckets(const std::vector<SpriteDef>& sprites) {
    SpriteBuckets b;
    for (int i = 0; i < (int)sprites.size(); ++i) {
        // Exclude closed 360-degree rings (90, 11) and hard-cut rectangular gradient bars (124, 18)
        // so shading/blush uses true 4-side feather airbrushes (125, 127, 128) with zero rectangular box edges!
        const int id = sprites[i].stickerId;
        if (id == 90 || id == 11 || id == 124 || id == 18) continue;
        b.all.push_back(i);
        if (sprites[i].category == 0) b.solid.push_back(i);
        else if (sprites[i].category == 1) b.grad.push_back(i);
        else b.curve.push_back(i);
        // Also include tapered stroke 128, teardrop 9, and almond 97 in curve/line-art bucket!
        if (id == 128 || id == 9 || id == 97) {
            b.curve.push_back(i);
        }
    }
    if (b.solid.empty()) b.solid = b.all;
    if (b.grad.empty()) b.grad = b.all;
    if (b.curve.empty()) b.curve = b.all;
    return b;
}

// Phase modes:
//   0 = Phase 1 (Base Fills & Silhouette Coverage)
//   1 = Phase 2 (Contour-Guided Anime Line-Art & Stretched Curves)
//   2 = Phase 3 (Eyes, Highlights, Hair Strands & Fine Details)
static Shape spawnShape(
    std::mt19937& rng,
    int W, int H,
    const std::vector<SpriteDef>& sprites,
    const SpriteBuckets& buckets,
    const std::vector<double>& errorCdf,
    const std::vector<ContourSeg>& contours,
    const std::vector<double>& contourCdf,
    int phaseMode,
    float progress)
{
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    Shape s{};

    // Direct contour-guided spawning in Phase 2 (and 35% of Phase 3)!
    const double contourProb = (phaseMode == 1) ? 0.72 : ((phaseMode == 2) ? 0.35 : 0.0);
    if (!contours.empty() && !contourCdf.empty() && contourCdf.back() > 0.0 && u01(rng) < contourProb) {
        const double r = u01(rng) * contourCdf.back();
        const auto it = std::lower_bound(contourCdf.begin(), contourCdf.end(), r);
        const size_t ci = (size_t)std::min<ptrdiff_t>((ptrdiff_t)contours.size() - 1, it - contourCdf.begin());
        const ContourSeg& seg = contours[ci];
        const float px = clampf(seg.cx + (float)(u01(rng) - 0.5) * 2.5f, 0.0f, (float)(W - 1));
        const float py = clampf(seg.cy + (float)(u01(rng) - 0.5) * 2.5f, 0.0f, (float)(H - 1));

        const bool curved = std::fabs(seg.curvature) > 0.030f;
        if (curved && !buckets.curve.empty() && u01(rng) < 0.75) {
            s.spriteSlot = buckets.curve[rng() % buckets.curve.size()];
            const SpriteDef& sd = sprites[s.spriteSlot];
            const float stretchL = seg.length * (0.75f + (float)u01(rng) * 0.90f);
            // Scale bounding box width by 1 / thickRatio so the actual opaque arc ribbon matches seg.thickness!
            const float desiredThick = std::max(1.2f, seg.thickness * (0.65f + (float)u01(rng) * 0.95f));
            const float depthW = desiredThick / std::max(0.12f, sd.thickRatio);
            s.h = clampf(stretchL, 3.0f, (float)H * 0.75f);
            s.w = clampf(depthW, 2.0f, (float)W * 0.45f);
            // Align the sprite's stretched local tangent with seg.angleDeg
            const float tr = sd.tangentDeg * (kPi / 180.0f);
            const float effNativeDeg = std::atan2(std::sin(tr) * s.h, std::cos(tr) * s.w) * (180.0f / kPi);
            const float flip180 = (u01(rng) < 0.5) ? 0.0f : 180.0f;
            s.angle = std::fmod(seg.angleDeg - effNativeDeg + flip180 + (float)(u01(rng) - 0.5) * 16.0f + 720.0f, 360.0f);
        } else {
            // Crisp straight/tapered stroke along tangent (teardrop 9, almond 97, square 87, triangle 95, crescent 130)
            const auto& pool = (u01(rng) < 0.65) ? buckets.solid : buckets.curve;
            s.spriteSlot = pool[rng() % pool.size()];
            const SpriteDef& sd = sprites[s.spriteSlot];
            const float desiredThick = std::max(0.85f, seg.thickness * (0.55f + (float)u01(rng) * 0.90f));
            s.w = clampf(seg.length * (0.70f + (float)u01(rng) * 0.85f), 2.0f, (float)W * 0.65f);
            s.h = clampf(desiredThick / std::max(0.15f, sd.thickRatio), 0.85f, 18.0f);
            s.angle = std::fmod(seg.angleDeg + (float)(u01(rng) - 0.5) * 14.0f + 360.0f, 360.0f);
        }
        // Pin the sprite's actual opaque stroke spine directly on (px, py)!
        clampShapeGeometry(s, sprites[s.spriteSlot], W, H);
        shapeSetAnchor(s, sprites[s.spriteSlot], px, py);
        return s;
    }

    // Error-CDF guided spawn point (px, py)
    float px, py;
    if (!errorCdf.empty() && errorCdf.back() > 0.0 && u01(rng) < 0.92) {
        const double r = u01(rng) * errorCdf.back();
        const auto it = std::lower_bound(errorCdf.begin(), errorCdf.end(), r);
        const size_t idx = (size_t)std::min<ptrdiff_t>((ptrdiff_t)errorCdf.size() - 1, it - errorCdf.begin());
        px = (float)(idx % (size_t)W);
        py = (float)(idx / (size_t)W);
    } else {
        px = (float)(u01(rng) * W);
        py = (float)(u01(rng) * H);
    }

    if (phaseMode == 0) {
        const double bp = u01(rng);
        if (progress < 0.30f) {
            s.spriteSlot = buckets.solid[rng() % buckets.solid.size()];
        } else if (bp < 0.68) {
            s.spriteSlot = buckets.solid[rng() % buckets.solid.size()];
        } else if (bp < 0.86) {
            s.spriteSlot = buckets.grad[rng() % buckets.grad.size()];
        } else {
            s.spriteSlot = buckets.curve[rng() % buckets.curve.size()];
        }
    } else if (phaseMode == 1) {
        const auto& pool = (u01(rng) < 0.60) ? buckets.curve : buckets.solid;
        s.spriteSlot = pool[rng() % pool.size()];
    } else {
        s.spriteSlot = buckets.all[rng() % buckets.all.size()];
    }

    const float maxDim = (float)std::max(W, H);
    const double bucket = u01(rng);
    const bool elongated = (u01(rng) < (phaseMode == 1 ? 0.68 : 0.42));

    if (elongated) {
        const float longS = 4.0f + (float)u01(rng) * (maxDim * (phaseMode == 0 ? 0.38f : 0.24f));
        const float thinS = 0.85f + (float)u01(rng) * (phaseMode == 1 ? 8.5f : 14.0f);
        if (u01(rng) < 0.5) { s.w = longS; s.h = thinS; }
        else { s.w = thinS; s.h = longS; }
    } else {
        float maxS;
        if (phaseMode == 0 && progress < 0.35f) {
            maxS = (bucket < 0.52) ? maxDim * 0.50f : (bucket < 0.82 ? maxDim * 0.24f : maxDim * 0.09f);
        } else if (phaseMode == 0) {
            maxS = (bucket < 0.32) ? maxDim * 0.28f : (bucket < 0.70 ? maxDim * 0.12f : maxDim * 0.045f);
        } else {
            maxS = (bucket < 0.16) ? maxDim * 0.14f : (bucket < 0.56 ? maxDim * 0.065f : maxDim * 0.025f);
        }
        s.w = 1.2f + (float)u01(rng) * std::max(2.0f, maxS);
        s.h = 1.2f + (float)u01(rng) * std::max(2.0f, maxS);
    }
    s.angle = (float)(u01(rng) * 360.0);
    clampShapeGeometry(s, sprites[s.spriteSlot], W, H);
    shapeSetAnchor(s, sprites[s.spriteSlot], px, py);
    return s;
}

// Anchor-pivoted mutation: rotating, stretching, or swapping sprites keeps the opaque stroke spine pinned at (px, py)!
static Shape mutateShape(
    const Shape& s,
    std::mt19937& rng,
    int W, int H,
    const std::vector<SpriteDef>& sprites,
    const std::vector<int>& allowedPool,
    float scale,
    bool allowSpriteSwap)
{
    Shape m = s;
    float px, py;
    shapeGetAnchor(m, sprites[m.spriteSlot], px, py);

    std::normal_distribution<float> norm(0.0f, 1.0f);
    std::uniform_int_distribution<int> pick(0, allowSpriteSwap ? 6 : 5);
    const float posStep = 13.0f * scale;
    const float szStep = 15.0f * scale;
    const float angStep = 24.0f * scale;
    const int mode = pick(rng);
    if (mode == 0) {
        px = clampf(px + norm(rng) * posStep, -8.0f, (float)W + 8.0f);
        py = clampf(py + norm(rng) * posStep, -8.0f, (float)H + 8.0f);
    } else if (mode == 1) {
        m.w = clampf(m.w + norm(rng) * szStep, 0.65f, (float)W * 1.15f);
    } else if (mode == 2) {
        m.h = clampf(m.h + norm(rng) * szStep, 0.65f, (float)H * 1.15f);
    } else if (mode == 3) {
        m.angle = std::fmod(m.angle + norm(rng) * angStep + 360.0f, 360.0f);
    } else if (mode == 4) {
        const float factor = std::exp(norm(rng) * 0.35f * scale);
        m.w = clampf(m.w * factor, 0.65f, (float)W * 1.15f);
        m.h = clampf(m.h / factor, 0.65f, (float)H * 1.15f);
    } else if (mode == 5) {
        px = clampf(px + norm(rng) * posStep * 0.45f, -8.0f, (float)W + 8.0f);
        py = clampf(py + norm(rng) * posStep * 0.45f, -8.0f, (float)H + 8.0f);
        m.w = clampf(m.w + norm(rng) * szStep * 0.45f, 0.65f, (float)W * 1.15f);
        m.h = clampf(m.h + norm(rng) * szStep * 0.45f, 0.65f, (float)H * 1.15f);
        m.angle = std::fmod(m.angle + norm(rng) * angStep * 0.35f + 360.0f, 360.0f);
    } else if (!allowedPool.empty()) {
        m.spriteSlot = allowedPool[rng() % allowedPool.size()];
    }
    clampShapeGeometry(m, sprites[m.spriteSlot], W, H);
    shapeSetAnchor(m, sprites[m.spriteSlot], px, py);
    return m;
}

// Gauss-Newton analytical sub-pixel gradient step on (cx, cy, w, h, angle) using pre-smoothed 256x256 mask derivatives
static bool gaussNewtonStepShape(
    Shape& s,
    double& ioBestDelta,
    const std::vector<SpriteDef>& sprites,
    const std::vector<RGBA>& target,
    const std::vector<RGBA>& below,
    const AbovePix* above,
    const std::vector<float>& weights,
    int W, int H,
    double effBgPenalty,
    std::uint8_t baseA,
    bool adaptiveAlpha)
{
    const AABB b = computeAABB(s, W, H);
    if (b.empty) return false;
    const SpriteDef& sd = sprites[s.spriteSlot];
    const std::uint8_t* mask = sd.mask;
    const std::int16_t* gU = sd.gradU;
    const std::int16_t* gV = sd.gradV;

    const float du_dx = b.cosA * b.invW;
    const float dv_dx = -b.sinA * b.invH;
    const float du_dy = b.sinA * b.invW;
    const float dv_dy = b.cosA * b.invH;
    const float dx0 = (float)b.x0 + 0.5f - s.cx;
    const float dy0 = (float)b.y0 + 0.5f - s.cy;
    float ur = (dx0 * b.cosA + dy0 * b.sinA) * b.invW + (float)kMaskSize * 0.5f;
    float vr = (-dx0 * b.sinA + dy0 * b.cosA) * b.invH + (float)kMaskSize * 0.5f;

    // Accumulate Gauss-Newton gradient G[5] and diagonal Hessian H[5] for (cx, cy, w, h, angleRad)
    double G[5] = {0, 0, 0, 0, 0};
    double H_diag[5] = {0, 0, 0, 0, 0};
    const float colAf = (float)s.color.a / 255.0f;
    const float hOverW = s.h / std::max(0.5f, s.w);
    const float wOverH = s.w / std::max(0.5f, s.h);

    for (int y = b.y0; y <= b.y1; ++y) {
        float u = ur, v = vr;
        const int rowOff = y * W;
        for (int x = b.x0; x <= b.x1; ++x) {
            const int iu = (int)u;
            const int iv = (int)v;
            if ((unsigned)iu < 256u && (unsigned)iv < 256u) {
                const int mIdx = (iv << 8) + iu;
                const int dmu_i = gU[mIdx];
                const int dmv_i = gV[mIdx];
                if ((dmu_i | dmv_i) != 0) {
                    const int idx = rowOff + x;
                    const float fT = above ? above[idx].t : 1.0f;
                    if (fT > 0.005f) {
                        const float dmu = (float)dmu_i * (1.0f / 255.0f);
                        const float dmv = (float)dmv_i * (1.0f / 255.0f);
                        const float uBar = u - 128.0f;
                        const float vBar = v - 128.0f;

                        // Partial derivatives of mask coverage m(u,v) w.r.t. (cx, cy, w, h, angleRad)
                        const float dm_dcx  = -(dmu * du_dx + dmv * dv_dx);
                        const float dm_dcy  = -(dmu * du_dy + dmv * dv_dy);
                        const float dm_dw   = -dmu * (uBar / s.w);
                        const float dm_dh   = -dmv * (vBar / s.h);
                        const float dm_dang =  dmu * (vBar * hOverW) - dmv * (uBar * wOverH);

                        const std::uint32_t ma = sampleMaskBilinear(mask, u, v);
                        const float a = colAf * ((float)ma / 255.0f);
                        const float kScale = fT * colAf;
                        const RGBA t = target[idx];
                        const RGBA bel = below[idx];

                        const float dR = (float)s.color.r - (float)bel.r;
                        const float dG = (float)s.color.g - (float)bel.g;
                        const float dB = (float)s.color.b - (float)bel.b;
                        const float dA = 255.0f           - (float)bel.a;

                        const float i0R = (above ? above[idx].r : 0.0f) + fT * bel.r;
                        const float i0G = (above ? above[idx].g : 0.0f) + fT * bel.g;
                        const float i0B = (above ? above[idx].b : 0.0f) + fT * bel.b;
                        const float i0A = 255.0f * (1.0f - fT)          + fT * bel.a;

                        const float k = fT * a;
                        const float rR = (i0R + k * dR) - (float)t.r;
                        const float rG = (i0G + k * dG) - (float)t.g;
                        const float rB = (i0B + k * dB) - (float)t.b;
                        const float rA = (i0A + k * dA) - (float)t.a;

                        double Dp, Ep;
                        if (t.a == 0) {
                            const double wBg = effBgPenalty * 4.0;
                            Dp = wBg * (double)(rA * dA);
                            Ep = wBg * (double)(dA * dA);
                        } else {
                            const double wp = (double)weights[idx];
                            Dp = wp * (double)(rR * dR + rG * dG + rB * dB + 3.0f * rA * dA);
                            Ep = wp * (double)(dR * dR + dG * dG + dB * dB + 3.0f * dA * dA);
                        }

                        const float j[5] = {
                            kScale * dm_dcx,
                            kScale * dm_dcy,
                            kScale * dm_dw,
                            kScale * dm_dh,
                            kScale * dm_dang
                        };
                        for (int c = 0; c < 5; ++c) {
                            G[c]      += Dp * (double)j[c];
                            H_diag[c] += Ep * (double)j[c] * (double)j[c];
                        }
                    }
                }
            }
            u += du_dx; v += dv_dx;
        }
        ur += du_dy; vr += dv_dy;
    }

    // Compute Gauss-Newton step direction
    float step[5];
    float normStep = 0.0f;
    for (int c = 0; c < 5; ++c) {
        if (H_diag[c] > 1e-6) {
            step[c] = (float)(-G[c] / (H_diag[c] + 1e3));
        } else {
            step[c] = 0.0f;
        }
        normStep += step[c] * step[c];
    }
    if (normStep < 1e-6f) return false;

    // Clamp maximum single step excursion
    step[0] = clampf(step[0], -6.0f, 6.0f);
    step[1] = clampf(step[1], -6.0f, 6.0f);
    step[2] = clampf(step[2], -s.w * 0.35f, s.w * 0.35f);
    step[3] = clampf(step[3], -s.h * 0.35f, s.h * 0.35f);
    const float stepDeg = clampf(step[4] * (180.0f / kPi), -18.0f, 18.0f);

    bool improved = false;
    const float alphas[3] = { 1.0f, 0.45f, 1.8f };
    for (float eta : alphas) {
        Shape cand = s;
        cand.cx = clampf(s.cx + eta * step[0], -10.0f, (float)W + 10.0f);
        cand.cy = clampf(s.cy + eta * step[1], -10.0f, (float)H + 10.0f);
        cand.w  = clampf(s.w  + eta * step[2], 0.65f, (float)W * 1.15f);
        cand.h  = clampf(s.h  + eta * step[3], 0.65f, (float)H * 1.15f);
        cand.angle = std::fmod(s.angle + eta * stepDeg + 360.0f, 360.0f);
        clampShapeGeometry(cand, sd, W, H);
        RGBA col = cand.color;
        double d = evalShapeExact(cand, sprites, target, below, above, weights, W, H, effBgPenalty, baseA, adaptiveAlpha, false, col);
        if (d < ioBestDelta) {
            cand.color = col;
            s = cand;
            ioBestDelta = d;
            improved = true;
            break;
        }
    }
    return improved;
}

static bool findBestNextShape(
    const std::vector<SpriteDef>& sprites,
    const SpriteBuckets& buckets,
    const std::vector<RGBA>& target,
    const std::vector<RGBA>& current,
    const AbovePix* above,
    const std::vector<float>& weights,
    const std::vector<double>& errorCdf,
    const std::vector<ContourSeg>& contours,
    const std::vector<double>& contourCdf,
    int W, int H,
    double effBgPenalty,
    std::uint8_t baseA,
    bool adaptiveAlpha,
    int candidates,
    int mutations,
    int threads,
    unsigned stepSeed,
    int phaseMode,
    float progress,
    Shape& outBest)
{
    const int candPerThread = std::max(12, (candidates + threads - 1) / threads);
    const std::vector<int>& swapPool = (phaseMode == 0 && progress < 0.25f) ? buckets.solid : buckets.all;

    struct Res { Shape sh; double delta = 1e30; bool ok = false; };
    std::vector<std::future<Res>> futs;
    futs.reserve((size_t)threads);

    for (int tid = 0; tid < threads; ++tid) {
        const unsigned tseed = stepSeed + (unsigned)tid * 7919u;
        futs.push_back(std::async(std::launch::async, [&, tseed, candPerThread]() -> Res {
            std::mt19937 rng(tseed);
            Res best;
            for (int c = 0; c < candPerThread; ++c) {
                Shape sh = spawnShape(rng, W, H, sprites, buckets, errorCdf, contours, contourCdf, phaseMode, progress);
                RGBA col{};
                double d = evalShapeExact(sh, sprites, target, current, above, weights, W, H, effBgPenalty, baseA, adaptiveAlpha, false, col);
                if (d < best.delta) {
                    sh.color = col;
                    best.sh = sh; best.delta = d; best.ok = true;
                }
            }
            if (!best.ok) return best;

            const float scales[3] = { 1.0f, 0.28f, 0.07f };
            const int ages[3] = { mutations, std::max(55, mutations / 2), std::max(35, mutations / 3) };
            for (int st = 0; st < 3; ++st) {
                int age = 0;
                while (age < ages[st]) {
                    Shape mut = mutateShape(best.sh, rng, W, H, sprites, swapPool, scales[st], st == 0);
                    RGBA col{};
                    double d = evalShapeExact(mut, sprites, target, current, above, weights, W, H, effBgPenalty, baseA, adaptiveAlpha, false, col);
                    if (d < best.delta) {
                        mut.color = col;
                        best.sh = mut; best.delta = d;
                        age = 0;
                    } else {
                        age++;
                    }
                }
                // Sub-pixel Gauss-Newton edge snap at end of each stage
                for (int gn = 0; gn < 3; ++gn) {
                    if (!gaussNewtonStepShape(best.sh, best.delta, sprites, target, current, above, weights, W, H, effBgPenalty, baseA, adaptiveAlpha)) break;
                }
            }
            return best;
        }));
    }

    Res win;
    for (auto& f : futs) {
        Res r = f.get();
        if (r.ok && r.delta < win.delta) win = r;
    }
    if (!win.ok || win.delta >= 0.0) return false;
    outBest = win.sh;
    return true;
}

static void buildContourCdf(
    const std::vector<ContourSeg>& contours,
    const std::vector<RGBA>& target,
    const std::vector<RGBA>& current,
    const std::vector<float>& weights,
    double effBgPenalty,
    int W, int H,
    std::vector<double>& outCdf)
{
    outCdf.resize(contours.size());
    double run = 0.0;
    for (size_t i = 0; i < contours.size(); ++i) {
        int ix = std::max(0, std::min(W - 1, (int)std::lround(contours[i].cx)));
        int iy = std::max(0, std::min(H - 1, (int)std::lround(contours[i].cy)));
        double eSum = 0.0;
        for (int dy = -1; dy <= 1; ++dy) {
            int y = std::max(0, std::min(H - 1, iy + dy));
            for (int dx = -1; dx <= 1; ++dx) {
                int x = std::max(0, std::min(W - 1, ix + dx));
                int idx = y * W + x;
                const RGBA t = target[idx], c = current[idx];
                if (t.a > 0) {
                    eSum += pixelErrorPremul(t, c, weights[idx], effBgPenalty);
                }
            }
        }
        run += eSum * std::min(40.0, (double)contours[i].length);
        outCdf[i] = run;
    }
}

// Compute exact final-image error contribution of each layer i using Top-to-Bottom associative decomposition!
// O(N * average_AABB_area) instead of O(N^2 * AABB_area)!
static std::vector<double> computeContributionsExact(
    const std::vector<Shape>& shapes,
    const std::vector<SpriteDef>& sprites,
    const std::vector<RGBA>& target,
    const std::vector<float>& weights,
    int W, int H,
    RGBA bg,
    double effBgPenalty)
{
    const size_t N = shapes.size();
    const size_t nPix = (size_t)W * (size_t)H;
    std::vector<double> contrib(N, 0.0);
    if (N == 0) return contrib;

    // 1. Checkpoints of `below` every 16 layers (saves memory while keeping O(1) amortised rebuild)
    constexpr int kStride = 16;
    const size_t nChk = (N + kStride - 1) / kStride;
    std::vector<std::vector<RGBA>> chk(nChk, std::vector<RGBA>(nPix, bg));
    std::vector<RGBA> cur(nPix, bg);
    for (size_t i = 0; i < N; ++i) {
        if (i % kStride == 0) chk[i / kStride] = cur;
        drawShapePremul(shapes[i], sprites, cur, W, H);
    }

    // 2. Sweep Top-to-Bottom (i = N-1 down to 0)
    std::vector<AbovePix> above(nPix, AbovePix{0.0f, 0.0f, 0.0f, 1.0f});
    std::vector<RGBA> below(nPix, bg);
    for (int i = (int)N - 1; i >= 0; --i) {
        const int baseIdx = (i / kStride) * kStride;
        below = chk[(size_t)(i / kStride)];
        for (int k = baseIdx; k < i; ++k) drawShapePremul(shapes[(size_t)k], sprites, below, W, H);

        RGBA dummyCol = shapes[(size_t)i].color;
        // delta = errWith - errWithout, so contribution (errWithout - errWith) = -delta!
        const double delta = evalShapeExact(shapes[(size_t)i], sprites, target, below, above.data(),
                                            weights, W, H, effBgPenalty, 255, false, true, dummyCol);
        contrib[(size_t)i] = -delta;
        foldShapeIntoAbove(shapes[(size_t)i], sprites, above, W, H);
    }
    return contrib;
}

// Top-to-Bottom Exact Associative Global Refinement Sweep:
// Optimizes every shape i = N-1 down to 0 against the EXACT 300-layer composite objective
// using Gauss-Newton analytical gradient steps + Hooke-Jeeves pattern search + anchor-pivoted mutations!
static void globalExactTopDownPolishSweep(
    std::vector<Shape>& shapes,
    const std::vector<SpriteDef>& sprites,
    const SpriteBuckets& buckets,
    const std::vector<RGBA>& target,
    const std::vector<float>& weights,
    int W, int H,
    RGBA bg,
    double effBgPenalty,
    std::uint8_t baseA,
    bool adaptiveAlpha,
    unsigned sweepSeed)
{
    const size_t N = shapes.size();
    const size_t nPix = (size_t)W * (size_t)H;
    if (N == 0) return;

    constexpr int kStride = 16;
    const size_t nChk = (N + kStride - 1) / kStride;
    std::vector<std::vector<RGBA>> chk(nChk, std::vector<RGBA>(nPix, bg));
    std::vector<RGBA> cur(nPix, bg);
    for (size_t i = 0; i < N; ++i) {
        if (i % kStride == 0) chk[i / kStride] = cur;
        drawShapePremul(shapes[i], sprites, cur, W, H);
    }

    std::vector<AbovePix> above(nPix, AbovePix{0.0f, 0.0f, 0.0f, 1.0f});
    std::vector<RGBA> below(nPix, bg);
    std::mt19937 rng(sweepSeed);

    for (int i = (int)N - 1; i >= 0; --i) {
        const int baseIdx = (i / kStride) * kStride;
        below = chk[(size_t)(i / kStride)];
        for (int k = baseIdx; k < i; ++k) drawShapePremul(shapes[(size_t)k], sprites, below, W, H);

        Shape best = shapes[(size_t)i];
        RGBA bestCol = best.color;
        // First evaluate current shape with its existing color, then allow optimal closed-form color update
        double bestDelta = evalShapeExact(best, sprites, target, below, above.data(), weights, W, H, effBgPenalty, baseA, adaptiveAlpha, true, bestCol);
        RGBA optCol = best.color;
        double optDelta = evalShapeExact(best, sprites, target, below, above.data(), weights, W, H, effBgPenalty, baseA, adaptiveAlpha, false, optCol);
        if (optDelta < bestDelta) {
            bestDelta = optDelta;
            best.color = optCol;
        }

        // 1. Gauss-Newton sub-pixel gradient steps
        for (int gn = 0; gn < 5; ++gn) {
            if (!gaussNewtonStepShape(best, bestDelta, sprites, target, below, above.data(), weights, W, H, effBgPenalty, baseA, adaptiveAlpha)) break;
        }

        // 2. Anchor-pivoted mutations (coarse + medium + sub-pixel fine)
        const float scales[3] = { 0.35f, 0.12f, 0.035f };
        const int tries[3] = { 45, 35, 25 };
        for (int st = 0; st < 3; ++st) {
            int age = 0;
            while (age < tries[st]) {
                Shape mut = mutateShape(best, rng, W, H, sprites, buckets.all, scales[st], st == 0);
                RGBA col{};
                double d = evalShapeExact(mut, sprites, target, below, above.data(), weights, W, H, effBgPenalty, baseA, adaptiveAlpha, false, col);
                if (d < bestDelta) {
                    mut.color = col;
                    best = mut; bestDelta = d;
                    age = 0;
                } else {
                    age++;
                }
            }
            for (int gn = 0; gn < 3; ++gn) {
                if (!gaussNewtonStepShape(best, bestDelta, sprites, target, below, above.data(), weights, W, H, effBgPenalty, baseA, adaptiveAlpha)) break;
            }
        }

        shapes[(size_t)i] = best;
        foldShapeIntoAbove(shapes[(size_t)i], sprites, above, W, H);
    }
}

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: geometrize_cli input.rgba W H out.json [shapeCount] [alpha] [candidates] [mutations] [threads] [seed] [bgPenalty] [previewRawOut] [weightsRawIn] [adaptiveAlpha] [spritesDir] [colorTargetRaw] [contoursBin]\n";
        return 2;
    }
    const std::string inPath = argv[1];
    const int W = std::atoi(argv[2]);
    const int H = std::atoi(argv[3]);
    const std::string outPath = argv[4];
    const int shapeCount = argc > 5 ? std::atoi(argv[5]) : 298;
    const int alpha = argc > 6 ? std::atoi(argv[6]) : 255;
    const int candidates = argc > 7 ? std::atoi(argv[7]) : 420;
    const int mutations = argc > 8 ? std::atoi(argv[8]) : 240;
    int threads = argc > 9 ? std::atoi(argv[9]) : 0;
    const unsigned seed = argc > 10 ? (unsigned)std::strtoul(argv[10], nullptr, 10) : 9001u;
    const double bgPenalty = argc > 11 ? std::atof(argv[11]) : 0.0;
    const std::string previewPath = argc > 12 ? argv[12] : "";
    const std::string weightsPath = argc > 13 ? argv[13] : "";
    const bool adaptiveAlpha = argc > 14 ? (std::atoi(argv[14]) != 0) : true;
    std::string spritesDir = argc > 15 ? argv[15] : "";
    const std::string colorPath = argc > 16 ? argv[16] : "";
    const std::string contoursPath = argc > 17 ? argv[17] : "";
    // Optional poolMode: 0 = Engine v5 hybrid (default), 1 = pure triangles (sticker 95 only), 2 = triangles (95) fills + native curves
    const int poolMode = argc > 18 ? std::atoi(argv[18]) : 0;
    // Optional polishSweeps: default 3, up to 6 for ultra-precision Auto-Converge passes
    const int polishSweeps = argc > 19 ? std::max(1, std::min(6, std::atoi(argv[19]))) : 3;

    if (spritesDir.empty()) {
        std::string exe = argv[0];
        size_t sl = exe.find_last_of("\\/");
        std::string binDir = (sl == std::string::npos) ? "." : exe.substr(0, sl);
        spritesDir = binDir + "\\..\\sprites_v4";
    }

    if (W <= 0 || H <= 0 || W > 8192 || H > 8192) { std::cerr << "bad dimensions\n"; return 2; }
    if (threads <= 0) threads = std::max(1, (int)std::thread::hardware_concurrency());

    std::vector<SpriteDef> sprites;
    loadSpritesV5(spritesDir, sprites);
    SpriteBuckets buckets = makeBuckets(sprites);
    if (poolMode == 1 || poolMode == 2) {
        std::vector<int> triOnly;
        for (int i = 0; i < (int)sprites.size(); ++i) {
            if (sprites[(size_t)i].stickerId == 95) triOnly.push_back(i);
        }
        if (!triOnly.empty()) {
            buckets.solid = triOnly;
            buckets.grad = triOnly;
            if (poolMode == 1) {
                buckets.curve = triOnly;
                buckets.all = triOnly;
            } else {
                // poolMode == 2: triangles (95) for fills + native curves for lineart
                buckets.all = triOnly;
                for (int ci : buckets.curve) buckets.all.push_back(ci);
            }
        }
    }
    std::vector<ContourSeg> contours;
    loadContours(contoursPath, contours);
    std::cerr << "Engine v5 (256x256 bilinear + spine anchors + Gauss-Newton, poolMode=" << poolMode << "): " << sprites.size() << " sprite variants ("
              << buckets.solid.size() << " solid, " << buckets.grad.size() << " airbrush, "
              << buckets.curve.size() << " curves), contours=" << contours.size() << "\n";

    std::vector<std::uint8_t> rawIn;
    if (!readFile(inPath, rawIn)) { std::cerr << "cannot read " << inPath << "\n"; return 2; }
    const size_t nPix = (size_t)W * (size_t)H;
    if (rawIn.size() != nPix * 4u) { std::cerr << "raw size mismatch\n"; return 2; }

    const bool transparentMode = (bgPenalty > 0.0);
    // Load target directly into Premultiplied RGBA!
    auto loadPremulRGBA = [&](const std::vector<std::uint8_t>& buf) {
        std::vector<RGBA> img(nPix);
        for (size_t i = 0; i < nPix; ++i) {
            std::uint32_t r = buf[i * 4 + 0], g = buf[i * 4 + 1], b = buf[i * 4 + 2], a = buf[i * 4 + 3];
            if (transparentMode && a == 0) {
                img[i] = RGBA{0, 0, 0, 0};
            } else {
                img[i] = RGBA{
                    (std::uint8_t)((r * a + 127u) / 255u),
                    (std::uint8_t)((g * a + 127u) / 255u),
                    (std::uint8_t)((b * a + 127u) / 255u),
                    (std::uint8_t)a
                };
            }
        }
        return img;
    };

    std::vector<RGBA> origTarget = loadPremulRGBA(rawIn);
    std::vector<RGBA> colorTarget = origTarget;
    bool hasColorTarget = false;
    if (!colorPath.empty() && colorPath != "none") {
        std::vector<std::uint8_t> rawCol;
        if (readFile(colorPath, rawCol) && rawCol.size() == nPix * 4u) {
            colorTarget = loadPremulRGBA(rawCol);
            size_t diffPix = 0;
            for (size_t i = 0; i < nPix; ++i) {
                if (origTarget[i].r != colorTarget[i].r || origTarget[i].g != colorTarget[i].g || origTarget[i].b != colorTarget[i].b) diffPix++;
            }
            hasColorTarget = (diffPix > 50);
        }
    }

    std::vector<float> weights(nPix, 1.0f);
    std::vector<float> fillWeights(nPix, 1.0f);
    float maxWeight = 1.0f;
    if (!weightsPath.empty() && weightsPath != "none") {
        std::vector<std::uint8_t> wRaw;
        if (readFile(weightsPath, wRaw) && wRaw.size() == nPix) {
            for (size_t i = 0; i < nPix; ++i) {
                weights[i] = std::max(0.25f, (float)wRaw[i] / 16.0f);
                fillWeights[i] = std::sqrt(weights[i]);
                if (origTarget[i].a > 0 && weights[i] > maxWeight) maxWeight = weights[i];
            }
        }
    }

    const double effBgPenalty = transparentMode ? (bgPenalty * (double)maxWeight * 0.68) : 0.0;
    const double fillBgPenalty = transparentMode ? (bgPenalty * std::sqrt((double)maxWeight) * 0.85) : 0.0;
    const RGBA bg = transparentMode ? RGBA{0, 0, 0, 0} : RGBA{255, 255, 255, 255};
    const std::uint8_t baseA = (std::uint8_t)std::max(1, std::min(255, alpha));

    std::vector<RGBA> current(nPix, bg);
    std::vector<double> errorCdf(nPix, 0.0);
    std::vector<double> contourCdf;
    std::vector<Shape> shapes;
    shapes.reserve((size_t)shapeCount);

    // Fix C2: Never erase line-art across half the budget!
    // Use colorTarget ONLY for the first ~16% coarse base silhouette/mass shapes,
    // then optimize all remaining 84% of shapes against the TRUE origTarget (with all line-art intact!).
    const int nCoarseVivid = hasColorTarget ? std::min(48, (int)(shapeCount * 0.16f)) : 0;
    const int nPhase1 = (int)(shapeCount * 0.36f);
    const int nPhase2 = (int)(shapeCount * 0.36f);
    const int nPhase3 = shapeCount - nPhase1 - nPhase2;
    std::cerr << "budgets: Phase1(fills)=" << nPhase1 << " (coarseVivid=" << nCoarseVivid << ")"
              << " Phase2(contours)=" << nPhase2
              << " Phase3(eyes/details)=" << nPhase3 << "\n";

    // PHASE 1: Base Fills & Silhouette Coverage
    for (int step = 0; step < nPhase1; ++step) {
        const bool useVivid = (step < nCoarseVivid);
        const auto& tgt = useVivid ? colorTarget : origTarget;
        const auto& wts = useVivid ? fillWeights : weights;
        const double bp = useVivid ? fillBgPenalty : effBgPenalty;
        buildErrorCdf(tgt, current, wts, bp, errorCdf);
        if (errorCdf.back() <= 1.0) break;
        const float prog = (float)step / (float)std::max(1, nPhase1);
        Shape best{};
        if (!findBestNextShape(sprites, buckets, tgt, current, nullptr, wts, errorCdf, contours, contourCdf,
                               W, H, bp, baseA, adaptiveAlpha, candidates, mutations,
                               threads, seed + (unsigned)step * 1009u, 0, prog, best)) {
            break;
        }
        drawShapePremul(best, sprites, current, W, H);
        shapes.push_back(best);
    }

    // Mid-pipeline polish on Phase 1 fills so base boundaries already hug contours cleanly before Phase 2 line-art!
    if (shapes.size() >= 16) {
        globalExactTopDownPolishSweep(shapes, sprites, buckets, origTarget, weights, W, H, bg, effBgPenalty, baseA, adaptiveAlpha, seed + 77777u);
        renderAllPremul(shapes, sprites, current, W, H, bg);
    }

    // PHASE 2: Contour-Guided Anime Line-Art & Stretched Curves (on EXACT Original Target!)
    for (int step = 0; step < nPhase2; ++step) {
        buildErrorCdf(origTarget, current, weights, effBgPenalty, errorCdf);
        if (!contours.empty()) buildContourCdf(contours, origTarget, current, weights, effBgPenalty, W, H, contourCdf);
        if (errorCdf.back() <= 1.0) break;
        const float prog = (float)step / (float)std::max(1, nPhase2);
        Shape best{};
        if (!findBestNextShape(sprites, buckets, origTarget, current, nullptr, weights, errorCdf, contours, contourCdf,
                               W, H, effBgPenalty, baseA, false /* crisp opaque ink */, candidates, mutations,
                               threads, seed + 100000u + (unsigned)step * 1009u, 1, prog, best)) {
            break;
        }
        drawShapePremul(best, sprites, current, W, H);
        shapes.push_back(best);
    }

    // PHASE 3: Eyes, Pupils, Specular Sparkles & High-Saliency Contours/Details
    for (int step = 0; step < nPhase3; ++step) {
        buildErrorCdf(origTarget, current, weights, effBgPenalty, errorCdf);
        if (!contours.empty()) buildContourCdf(contours, origTarget, current, weights, effBgPenalty, W, H, contourCdf);
        if (errorCdf.back() <= 1.0) break;
        const float prog = (float)step / (float)std::max(1, nPhase3);
        Shape best{};
        if (!findBestNextShape(sprites, buckets, origTarget, current, nullptr, weights, errorCdf, contours, contourCdf,
                               W, H, effBgPenalty, baseA, adaptiveAlpha, candidates, mutations,
                               threads, seed + 250000u + (unsigned)step * 1009u, 2, prog, best)) {
            break;
        }
        drawShapePremul(best, sprites, current, W, H);
        shapes.push_back(best);
    }

    double errNow = totalWeightedError(origTarget, current, weights, effBgPenalty);
    std::cerr << "after 3 phases: shapes=" << shapes.size() << " weightedErr=" << (long long)errNow << "\n";

    // PHASE 4: Global Refinement Rounds (Exact Top-to-Bottom Contribution Pruning + Top-to-Bottom Gauss-Newton Polish!)
    if ((int)shapes.size() >= 20) {
        for (int sw = 0; sw < polishSweeps; ++sw) {
            // 1. Exact Top-Down Polish Sweep first (monotonically improves every shape in-place!)
            globalExactTopDownPolishSweep(shapes, sprites, buckets, origTarget, weights, W, H, bg, effBgPenalty, baseA, adaptiveAlpha, seed + 800000u + (unsigned)sw * 31337u);
            renderAllPremul(shapes, sprites, current, W, H, bg);
            errNow = totalWeightedError(origTarget, current, weights, effBgPenalty);

            // 2. Recycle dead/buried shapes (bottom K contributors) into fresh top-layer contour & detail strokes
            const int K = std::min(28, std::max(6, (int)shapes.size() / 9));
            auto contribs = computeContributionsExact(shapes, sprites, origTarget, weights, W, H, bg, effBgPenalty);
            std::vector<size_t> idx(shapes.size());
            for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
            std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return contribs[a] < contribs[b]; });

            std::vector<bool> drop(shapes.size(), false);
            int actualDrop = 0;
            for (int k = 0; k < K; ++k) {
                // Always drop shapes with negative/tiny contribution, up to K shapes
                if (k < K / 2 || contribs[idx[k]] < 1.5e6) {
                    drop[idx[k]] = true;
                    actualDrop++;
                }
            }

            if (actualDrop > 0) {
                std::vector<Shape> kept;
                kept.reserve(shapes.size());
                for (size_t i = 0; i < shapes.size(); ++i) {
                    if (!drop[i]) kept.push_back(shapes[i]);
                }
                renderAllPremul(kept, sprites, current, W, H, bg);

                for (int r = 0; r < actualDrop; ++r) {
                    buildErrorCdf(origTarget, current, weights, effBgPenalty, errorCdf);
                    if (!contours.empty()) buildContourCdf(contours, origTarget, current, weights, effBgPenalty, W, H, contourCdf);
                    if (errorCdf.back() <= 1.0) break;
                    Shape fresh{};
                    const int mode = (r % 2 == 0) ? 1 : 2; // alternate contour precision & eye/detail
                    if (findBestNextShape(sprites, buckets, origTarget, current, nullptr, weights, errorCdf, contours, contourCdf,
                                          W, H, effBgPenalty, baseA, (mode == 2) && adaptiveAlpha, candidates, mutations,
                                          threads, seed + 500000u + (unsigned)sw * 100000u + (unsigned)r * 1009u, mode, 0.9f, fresh)) {
                        drawShapePremul(fresh, sprites, current, W, H);
                        kept.push_back(fresh);
                    }
                }
                const double errRecycle = totalWeightedError(origTarget, current, weights, effBgPenalty);
                if (errRecycle < errNow) {
                    shapes = std::move(kept);
                    errNow = errRecycle;
                } else {
                    renderAllPremul(shapes, sprites, current, W, H, bg);
                }
            }
            std::cerr << "sweep " << (sw + 1) << "/3 shapes=" << shapes.size() << " weightedErr=" << (long long)errNow << "\n";
        }
        // Final sub-pixel Gauss-Newton + coordinate polish pass across all layers
        globalExactTopDownPolishSweep(shapes, sprites, buckets, origTarget, weights, W, H, bg, effBgPenalty, baseA, adaptiveAlpha, seed + 999983u);
        renderAllPremul(shapes, sprites, current, W, H, bg);
        errNow = totalWeightedError(origTarget, current, weights, effBgPenalty);
        std::cerr << "final polish: shapes=" << shapes.size() << " weightedErr=" << (long long)errNow << "\n";
    }

    // Export native CPM2 JSON (including flipX)
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "{\"width\":" << W << ",\"height\":" << H << ",\"shapes\":[\n";
    for (size_t i = 0; i < shapes.size(); ++i) {
        const auto& s = shapes[i];
        const auto& sd = sprites[s.spriteSlot];
        oss << "{\"sticker\":" << sd.stickerId
            << ",\"flipX\":" << (sd.flipX ? "true" : "false")
            << ",\"cx\":" << s.cx << ",\"cy\":" << s.cy
            << ",\"w\":" << s.w << ",\"h\":" << s.h
            << ",\"angle\":" << s.angle
            << ",\"color\":[" << (int)s.color.r << "," << (int)s.color.g << "," << (int)s.color.b << "," << (int)s.color.a << "]}";
        if (i + 1 < shapes.size()) oss << ",";
        oss << "\n";
    }
    oss << "]}\n";

    std::ofstream out(outPath, std::ios::binary);
    if (!out) { std::cerr << "cannot write " << outPath << "\n"; return 2; }
    const std::string jstr = oss.str();
    out.write(jstr.data(), (std::streamsize)jstr.size());
    out.close();

    if (!previewPath.empty() && previewPath != "none") {
        // Convert Premultiplied RGBA back to Straight RGBA for PNG preview compositing
        std::vector<RGBA> straight(nPix);
        for (size_t i = 0; i < nPix; ++i) {
            const RGBA p = current[i];
            if (p.a == 0) {
                straight[i] = RGBA{0, 0, 0, 0};
            } else if (p.a == 255) {
                straight[i] = p;
            } else {
                straight[i] = RGBA{
                    clampU8(((int)p.r * 255 + (p.a >> 1)) / (int)p.a),
                    clampU8(((int)p.g * 255 + (p.a >> 1)) / (int)p.a),
                    clampU8(((int)p.b * 255 + (p.a >> 1)) / (int)p.a),
                    p.a
                };
            }
        }
        std::ofstream pf(previewPath, std::ios::binary);
        if (pf) pf.write(reinterpret_cast<const char*>(straight.data()), (std::streamsize)(nPix * 4u));
    }

    std::cerr << "wrote " << outPath << " shapes=" << shapes.size() << "\n";
    return 0;
}
