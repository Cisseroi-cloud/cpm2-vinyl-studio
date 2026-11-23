#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "converter.h"
#include "../../third_party/nlohmann/json.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <queue>
#include <random>
#include <sstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace cpm {

static inline uint8_t clampU8(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static inline float rgbLuma(uint8_t r, uint8_t g, uint8_t b) {
    return (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
}

bool loadDecalImage(const std::string& path, DecalItem& decal) {
    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!data || w <= 0 || h <= 0) return false;

    decal.imagePath = path;
    decal.origW = w;
    decal.origH = h;
    decal.origRGBA.assign(data, data + (size_t)w * (size_t)h * 4u);
    stbi_image_free(data);

    decal.prepDirty = true;
    preprocessDecalImage(decal);
    if (decal.xform.lockAspect && decal.prepH > 0) {
        decal.xform.scale.x = decal.xform.scale.y * ((float)decal.prepW / (float)decal.prepH);
    }
    return true;
}

static void erodeMask(std::vector<uint8_t>& m, int w, int h, int steps) {
    std::vector<uint8_t> tmp(m.size(), 0);
    for (int s = 0; s < steps; ++s) {
        std::fill(tmp.begin(), tmp.end(), 0);
        for (int y = 1; y < h - 1; ++y) {
            for (int x = 1; x < w - 1; ++x) {
                int i = y * w + x;
                tmp[(size_t)i] = (m[(size_t)i] && m[(size_t)(i - 1)] && m[(size_t)(i + 1)] &&
                                  m[(size_t)(i - w)] && m[(size_t)(i + w)]) ? 1 : 0;
            }
        }
        m.swap(tmp);
    }
}

static void decontaminateEdgeRGB(std::vector<uint8_t>& rgba, const std::vector<uint8_t>& fg, int w, int h, int rimErode, int outDilate) {
    std::vector<uint8_t> valid = fg;
    erodeMask(valid, w, h, rimErode);
    bool anyValid = false;
    for (uint8_t v : valid) if (v) { anyValid = true; break; }
    if (!anyValid) valid = fg;

    std::vector<uint8_t> nextValid = valid;
    std::vector<uint8_t> nextRGBA = rgba;
    const int dx[8] = {-1, 1, 0, 0, -1, -1, 1, 1};
    const int dy[8] = {0, 0, -1, 1, -1, 1, -1, 1};

    for (int it = 0; it < rimErode + outDilate; ++it) {
        bool changed = false;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                size_t idx = (size_t)y * (size_t)w + (size_t)x;
                if (valid[idx]) continue;
                int sr = 0, sg = 0, sb = 0, cnt = 0;
                for (int k = 0; k < 8; ++k) {
                    int nx = x + dx[k], ny = y + dy[k];
                    if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                    size_t ni = (size_t)ny * (size_t)w + (size_t)nx;
                    if (valid[ni]) {
                        sr += rgba[ni * 4 + 0];
                        sg += rgba[ni * 4 + 1];
                        sb += rgba[ni * 4 + 2];
                        cnt++;
                    }
                }
                if (cnt > 0) {
                    nextRGBA[idx * 4 + 0] = (uint8_t)(sr / cnt);
                    nextRGBA[idx * 4 + 1] = (uint8_t)(sg / cnt);
                    nextRGBA[idx * 4 + 2] = (uint8_t)(sb / cnt);
                    nextValid[idx] = 1;
                    changed = true;
                }
            }
        }
        rgba.swap(nextRGBA);
        valid.swap(nextValid);
        nextRGBA = rgba;
        nextValid = valid;
        if (!changed) break;
    }
}

static void posterizeKMeans(std::vector<uint8_t>& rgba, int w, int h, int K) {
    if (K < 2 || K > 32) return;
    std::vector<size_t> fgIdx;
    fgIdx.reserve((size_t)w * (size_t)h);
    for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) {
        if (rgba[i * 4 + 3] >= 128) fgIdx.push_back(i);
    }
    if ((int)fgIdx.size() <= K) return;

    // Subsample for fast K-means++ initialization
    std::mt19937 rng(42);
    std::vector<Vec3> centers;
    centers.reserve((size_t)K);
    size_t first = fgIdx[rng() % fgIdx.size()];
    centers.push_back({(float)rgba[first * 4], (float)rgba[first * 4 + 1], (float)rgba[first * 4 + 2]});

    const size_t stride = std::max<size_t>(1, fgIdx.size() / 4096);
    for (int c = 1; c < K; ++c) {
        double bestDist = -1.0;
        Vec3 bestCol = centers[0];
        for (size_t s = 0; s < fgIdx.size(); s += stride) {
            size_t pi = fgIdx[s];
            Vec3 p{(float)rgba[pi * 4], (float)rgba[pi * 4 + 1], (float)rgba[pi * 4 + 2]};
            double minD = 1e30;
            for (const auto& cen : centers) {
                Vec3 d = p - cen;
                double d2 = d.dot(d);
                if (d2 < minD) minD = d2;
            }
            if (minD > bestDist) { bestDist = minD; bestCol = p; }
        }
        centers.push_back(bestCol);
    }

    // 8 Lloyd iterations
    std::vector<Vec3> sums((size_t)K);
    std::vector<int> cnts((size_t)K);
    for (int it = 0; it < 8; ++it) {
        std::fill(sums.begin(), sums.end(), Vec3{0, 0, 0});
        std::fill(cnts.begin(), cnts.end(), 0);
        for (size_t s = 0; s < fgIdx.size(); s += stride) {
            size_t pi = fgIdx[s];
            Vec3 p{(float)rgba[pi * 4], (float)rgba[pi * 4 + 1], (float)rgba[pi * 4 + 2]};
            int bestK = 0;
            float bestD = 1e30f;
            for (int k = 0; k < K; ++k) {
                Vec3 d = p - centers[(size_t)k];
                float d2 = d.dot(d);
                if (d2 < bestD) { bestD = d2; bestK = k; }
            }
            sums[(size_t)bestK] += p;
            cnts[(size_t)bestK]++;
        }
        for (int k = 0; k < K; ++k) {
            if (cnts[(size_t)k] > 0) centers[(size_t)k] = sums[(size_t)k] * (1.0f / (float)cnts[(size_t)k]);
        }
    }

    // Assign all foreground pixels to nearest palette center
    for (size_t pi : fgIdx) {
        Vec3 p{(float)rgba[pi * 4], (float)rgba[pi * 4 + 1], (float)rgba[pi * 4 + 2]};
        int bestK = 0;
        float bestD = 1e30f;
        for (int k = 0; k < K; ++k) {
            Vec3 d = p - centers[(size_t)k];
            float d2 = d.dot(d);
            if (d2 < bestD) { bestD = d2; bestK = k; }
        }
        rgba[pi * 4 + 0] = clampU8((int)std::round(centers[(size_t)bestK].x));
        rgba[pi * 4 + 1] = clampU8((int)std::round(centers[(size_t)bestK].y));
        rgba[pi * 4 + 2] = clampU8((int)std::round(centers[(size_t)bestK].z));
    }
}

static void resizeRGBA(
    const std::vector<uint8_t>& src, int sw, int sh,
    std::vector<uint8_t>& dst, int dw, int dh
) {
    dst.resize((size_t)dw * (size_t)dh * 4u);
    float sx = (float)sw / (float)dw;
    float sy = (float)sh / (float)dh;
    for (int y = 0; y < dh; ++y) {
        float y0 = y * sy, y1 = (y + 1) * sy;
        int iy0 = std::clamp((int)std::floor(y0), 0, sh - 1);
        int iy1 = std::clamp((int)std::ceil(y1), iy0 + 1, sh);
        for (int x = 0; x < dw; ++x) {
            float x0 = x * sx, x1 = (x + 1) * sx;
            int ix0 = std::clamp((int)std::floor(x0), 0, sw - 1);
            int ix1 = std::clamp((int)std::ceil(x1), ix0 + 1, sw);
            float sr = 0, sg = 0, sb = 0, sa = 0, cnt = 0;
            for (int yy = iy0; yy < iy1; ++yy) {
                for (int xx = ix0; xx < ix1; ++xx) {
                    size_t si = ((size_t)yy * (size_t)sw + (size_t)xx) * 4u;
                    sr += src[si + 0]; sg += src[si + 1]; sb += src[si + 2]; sa += src[si + 3];
                    cnt += 1.0f;
                }
            }
            size_t di = ((size_t)y * (size_t)dw + (size_t)x) * 4u;
            dst[di + 0] = clampU8((int)std::round(sr / cnt));
            dst[di + 1] = clampU8((int)std::round(sg / cnt));
            dst[di + 2] = clampU8((int)std::round(sb / cnt));
            dst[di + 3] = (sa / cnt >= 128.0f) ? 255 : 0;
            if (dst[di + 3] == 0) { dst[di + 0] = dst[di + 1] = dst[di + 2] = 0; }
        }
    }
}

bool preprocessDecalImage(DecalItem& decal, std::vector<uint8_t>* outPosterizedRGBA) {
    if (decal.origRGBA.empty() || decal.origW <= 0 || decal.origH <= 0) return false;
    const int w = decal.origW, h = decal.origH;
    const size_t nPix = (size_t)w * (size_t)h;
    const auto& p = decal.prep;

    std::vector<uint8_t> work = decal.origRGBA;
    std::vector<uint8_t> fg(nPix, 1);

    // 1. Sample border median RGB
    std::vector<uint8_t> br, bg, bb;
    for (int x = 0; x < w; ++x) {
        size_t i0 = (size_t)x * 4u, i1 = ((size_t)(h - 1) * (size_t)w + (size_t)x) * 4u;
        br.push_back(work[i0]); bg.push_back(work[i0 + 1]); bb.push_back(work[i0 + 2]);
        br.push_back(work[i1]); bg.push_back(work[i1 + 1]); bb.push_back(work[i1 + 2]);
    }
    for (int y = 0; y < h; ++y) {
        size_t i0 = ((size_t)y * (size_t)w) * 4u, i1 = ((size_t)y * (size_t)w + (size_t)(w - 1)) * 4u;
        br.push_back(work[i0]); bg.push_back(work[i0 + 1]); bb.push_back(work[i0 + 2]);
        br.push_back(work[i1]); bg.push_back(work[i1 + 1]); bb.push_back(work[i1 + 2]);
    }
    std::nth_element(br.begin(), br.begin() + br.size() / 2, br.end());
    std::nth_element(bg.begin(), bg.begin() + bg.size() / 2, bg.end());
    std::nth_element(bb.begin(), bb.begin() + bb.size() / 2, bb.end());
    float bgR = (float)br[br.size() / 2], bgG = (float)bg[bg.size() / 2], bgB = (float)bb[bb.size() / 2];

    // Check real alpha
    int transpCnt = 0;
    for (size_t i = 0; i < nPix; ++i) if (work[i * 4 + 3] < 128) transpCnt++;
    bool hasRealAlpha = ((float)transpCnt / (float)nPix) > 0.01f;
    int effMode = (p.bgMode == BG_AUTO) ? (hasRealAlpha ? BG_ALPHA : BG_FLOOD) : p.bgMode;

    auto colorDist = [&](size_t i) -> float {
        float dr = (float)work[i * 4 + 0] - bgR;
        float dg = (float)work[i * 4 + 1] - bgG;
        float db = (float)work[i * 4 + 2] - bgB;
        return std::sqrt(dr * dr + dg * dg + db * db);
    };

    if (effMode == BG_NONE) {
        std::fill(fg.begin(), fg.end(), 1);
    } else if (effMode == BG_ALPHA) {
        for (size_t i = 0; i < nPix; ++i) fg[i] = (work[i * 4 + 3] >= 128) ? 1 : 0;
    } else if (effMode == BG_KEY) {
        for (size_t i = 0; i < nPix; ++i) fg[i] = (colorDist(i) > p.bgTol) ? 1 : 0;
    } else if (effMode == BG_STICKER_WHITE) {
        std::vector<uint8_t> isBg(nPix, 0);
        std::queue<int> q;
        auto isWall = [&](size_t i) {
            uint8_t r = work[i * 4], g = work[i * 4 + 1], b = work[i * 4 + 2];
            float lum = 0.299f * r + 0.587f * g + 0.114f * b;
            int ch = std::max({r, g, b}) - std::min({r, g, b});
            return lum >= 235.0f && ch <= 32;
        };
        auto seed = [&](int x, int y) {
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            if (!isWall(i) && !isBg[i]) { isBg[i] = 1; q.push((int)i); }
        };
        for (int x = 0; x < w; ++x) { seed(x, 0); seed(x, h - 1); }
        for (int y = 0; y < h; ++y) { seed(0, y); seed(w - 1, y); }
        while (!q.empty()) {
            int cur = q.front(); q.pop();
            int cx = cur % w, cy = cur / w;
            const int d4[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
            for (auto& d : d4) {
                int nx = cx + d[0], ny = cy + d[1];
                if (nx >= 0 && nx < w && ny >= 0 && ny < h) seed(nx, ny);
            }
        }
        for (size_t i = 0; i < nPix; ++i) fg[i] = isBg[i] ? 0 : 1;
    } else {
        // BG_FLOOD + Hysteresis interior hole opening (exact match with prepare_image.py!)
        std::vector<uint8_t> isBg(nPix, 0);
        std::queue<int> q;
        auto seed = [&](int x, int y) {
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            if (!isBg[i] && colorDist(i) <= p.bgTol) { isBg[i] = 1; q.push((int)i); }
        };
        for (int x = 0; x < w; ++x) { seed(x, 0); seed(x, h - 1); }
        for (int y = 0; y < h; ++y) { seed(0, y); seed(w - 1, y); }
        while (!q.empty()) {
            int cur = q.front(); q.pop();
            int cx = cur % w, cy = cur / w;
            const int d4[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
            for (auto& d : d4) {
                int nx = cx + d[0], ny = cy + d[1];
                if (nx >= 0 && nx < w && ny >= 0 && ny < h) seed(nx, ny);
            }
        }
        if (p.removeHoles) {
            float strictTol = std::min(14.0f, p.bgTol * 0.28f);
            std::vector<uint8_t> vis(nPix, 0);
            std::vector<int> holeSeeds;
            for (int y = 1; y < h - 1; ++y) {
                for (int x = 1; x < w - 1; ++x) {
                    size_t i = (size_t)y * (size_t)w + (size_t)x;
                    if (isBg[i] || vis[i] || colorDist(i) > strictTol) continue;
                    std::vector<int> comp;
                    std::queue<int> cq;
                    vis[i] = 1; cq.push((int)i);
                    while (!cq.empty()) {
                        int c = cq.front(); cq.pop();
                        comp.push_back(c);
                        int cx = c % w, cy = c / w;
                        const int d4[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
                        for (auto& d : d4) {
                            int nx = cx + d[0], ny = cy + d[1];
                            if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                            size_t ni = (size_t)ny * (size_t)w + (size_t)nx;
                            if (!isBg[ni] && !vis[ni] && colorDist(ni) <= strictTol) {
                                vis[ni] = 1; cq.push((int)ni);
                            }
                        }
                    }
                    if (comp.size() >= 25) {
                        for (int c : comp) { isBg[(size_t)c] = 1; holeSeeds.push_back(c); }
                    }
                }
            }
            std::queue<int> hq;
            for (int c : holeSeeds) hq.push(c);
            while (!hq.empty()) {
                int c = hq.front(); hq.pop();
                int cx = c % w, cy = c / w;
                const int d4[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
                for (auto& d : d4) {
                    int nx = cx + d[0], ny = cy + d[1];
                    if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                    size_t ni = (size_t)ny * (size_t)w + (size_t)nx;
                    if (!isBg[ni] && colorDist(ni) <= p.bgTol) { isBg[ni] = 1; hq.push((int)ni); }
                }
            }
        }
        for (size_t i = 0; i < nPix; ++i) fg[i] = isBg[i] ? 0 : 1;
    }

    // Optional strip external die-cut white border
    if (p.stripWhiteBorder && effMode != BG_NONE) {
        std::queue<int> q;
        std::vector<uint8_t> peeled(nPix, 0);
        auto isWhite = [&](size_t i) {
            uint8_t r = work[i * 4], g = work[i * 4 + 1], b = work[i * 4 + 2];
            float lum = 0.299f * r + 0.587f * g + 0.114f * b;
            int ch = std::max({r, g, b}) - std::min({r, g, b});
            return lum >= 218.0f && ch <= 38;
        };
        for (int y = 1; y < h - 1; ++y) {
            for (int x = 1; x < w - 1; ++x) {
                size_t i = (size_t)y * (size_t)w + (size_t)x;
                if (!fg[i] || !isWhite(i)) continue;
                if (!fg[i - 1] || !fg[i + 1] || !fg[i - (size_t)w] || !fg[i + (size_t)w]) {
                    peeled[i] = 1; q.push((int)i);
                }
            }
        }
        while (!q.empty()) {
            int c = q.front(); q.pop();
            fg[(size_t)c] = 0;
            int cx = c % w, cy = c / w;
            const int d4[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
            for (auto& d : d4) {
                int nx = cx + d[0], ny = cy + d[1];
                if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                size_t ni = (size_t)ny * (size_t)w + (size_t)nx;
                if (fg[ni] && !peeled[ni] && isWhite(ni)) { peeled[ni] = 1; q.push((int)ni); }
            }
        }
    }

    if (p.erodePx > 0 && effMode != BG_NONE) {
        erodeMask(fg, w, h, p.erodePx);
    }

    // Decontaminate outer 2px rim before resize
    if (effMode != BG_NONE) {
        decontaminateEdgeRGB(work, fg, w, h, 2, 3);
    }

    // Apply contrast & saturation boost on foreground pixels
    if (std::fabs(p.contrastBoost - 1.0f) > 0.01f || std::fabs(p.saturationBoost - 1.0f) > 0.01f) {
        float c = p.contrastBoost;
        float sat = p.saturationBoost;
        for (size_t i = 0; i < nPix; ++i) {
            if (!fg[i]) continue;
            float r = (float)work[i * 4 + 0] / 255.0f;
            float g = (float)work[i * 4 + 1] / 255.0f;
            float b = (float)work[i * 4 + 2] / 255.0f;
            if (std::fabs(c - 1.0f) > 0.01f) {
                r = std::clamp((r - 0.5f) * c + 0.5f, 0.0f, 1.0f);
                g = std::clamp((g - 0.5f) * c + 0.5f, 0.0f, 1.0f);
                b = std::clamp((b - 0.5f) * c + 0.5f, 0.0f, 1.0f);
            }
            if (std::fabs(sat - 1.0f) > 0.01f) {
                float lum = 0.299f * r + 0.587f * g + 0.114f * b;
                r = std::clamp(lum + (r - lum) * sat, 0.0f, 1.0f);
                g = std::clamp(lum + (g - lum) * sat, 0.0f, 1.0f);
                b = std::clamp(lum + (b - lum) * sat, 0.0f, 1.0f);
            }
            work[i * 4 + 0] = clampU8((int)std::round(r * 255.0f));
            work[i * 4 + 1] = clampU8((int)std::round(g * 255.0f));
            work[i * 4 + 2] = clampU8((int)std::round(b * 255.0f));
        }
    }

    for (size_t i = 0; i < nPix; ++i) work[i * 4 + 3] = fg[i] ? 255 : 0;

    // Trim bounding box around foreground
    int x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (effMode != BG_NONE) {
        int minX = w, minY = h, maxX = -1, maxY = -1;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (fg[(size_t)y * (size_t)w + (size_t)x]) {
                    minX = std::min(minX, x); maxX = std::max(maxX, x);
                    minY = std::min(minY, y); maxY = std::max(maxY, y);
                }
            }
        }
        if (maxX >= minX && maxY >= minY) {
            x0 = std::max(0, minX - 2); y0 = std::max(0, minY - 2);
            x1 = std::min(w, maxX + 3); y1 = std::min(h, maxY + 3);
        }
    }

    int cw = std::max(1, x1 - x0);
    int ch = std::max(1, y1 - y0);
    std::vector<uint8_t> cropped((size_t)cw * (size_t)ch * 4u);
    for (int y = 0; y < ch; ++y) {
        std::memcpy(&cropped[(size_t)y * (size_t)cw * 4u],
                    &work[((size_t)(y0 + y) * (size_t)w + (size_t)x0) * 4u],
                    (size_t)cw * 4u);
    }

    // Resize so max(dw, dh) == workSize * max(cols, rows) (capped at 1200px for memory sanity)
    int tileFactor = std::max(decal.tiling.cols, decal.tiling.rows);
    int targetMax = std::clamp(p.workSize * tileFactor, 128, 1280);
    float sc = std::min(1.0f, (float)targetMax / (float)std::max(cw, ch));
    if (std::max(cw, ch) < p.workSize) sc = (float)p.workSize / (float)std::max(cw, ch);
    int dw = std::max(16, (int)std::round(cw * sc));
    int dh = std::max(16, (int)std::round(ch * sc));

    std::vector<uint8_t> resized;
    resizeRGBA(cropped, cw, ch, resized, dw, dh);

    decal.prepW = dw;
    decal.prepH = dh;
    decal.prepRGBA = resized;
    decal.prepDirty = false;

    if (outPosterizedRGBA) {
        *outPosterizedRGBA = resized;
        if (p.posterizeColors >= 2) {
            posterizeKMeans(*outPosterizedRGBA, dw, dh, p.posterizeColors);
        }
    } else if (p.posterizeColors >= 2) {
        // Preview shows posterized flats lightly blended or direct posterized if requested
        // Keep prepRGBA crisp for conversion; UI can toggle posterized preview
    }
    return true;
}

std::vector<ContourSeg> extractBlackHatContours(
    const uint8_t* rgba, int w, int h, float minValleyStrength
) {
    std::vector<ContourSeg> segs;
    if (w < 8 || h < 8) return segs;

    const size_t n = (size_t)w * (size_t)h;
    std::vector<float> luma(n, 1.0f);
    std::vector<uint8_t> fg(n, 0);
    for (size_t i = 0; i < n; ++i) {
        fg[i] = (rgba[i * 4 + 3] >= 128) ? 1 : 0;
        luma[i] = fg[i] ? rgbLuma(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]) : 1.0f;
    }

    // Separable 7x7 local maximum (Black-Hat valley = local_max_7x7 - luma)
    std::vector<float> maxH(n, 0.0f), valley(n, 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float mx = 0.0f;
            for (int dx = -3; dx <= 3; ++dx) {
                int nx = std::clamp(x + dx, 0, w - 1);
                mx = std::max(mx, luma[(size_t)y * (size_t)w + (size_t)nx]);
            }
            maxH[(size_t)y * (size_t)w + (size_t)x] = mx;
        }
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            if (!fg[i]) continue;
            float mx = 0.0f;
            for (int dy = -3; dy <= 3; ++dy) {
                int ny = std::clamp(y + dy, 0, h - 1);
                mx = std::max(mx, maxH[(size_t)ny * (size_t)w + (size_t)x]);
            }
            float v = std::max(0.0f, mx - luma[i]);
            // Also include dark silhouette rim
            bool rim = (x > 0 && !fg[i - 1]) || (x + 1 < w && !fg[i + 1]) ||
                       (y > 0 && !fg[i - (size_t)w]) || (y + 1 < h && !fg[i + (size_t)w]);
            if (rim && luma[i] < 0.45f) v = std::max(v, 0.5f - luma[i]);
            valley[i] = v;
        }
    }

    // Ridge mask via simple local peak check
    std::vector<uint8_t> ridge(n, 0);
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            float v = valley[i];
            if (v < minValleyStrength) continue;
            bool peakH = (v >= valley[i - 1] && v >= valley[i + 1]);
            bool peakV = (v >= valley[i - (size_t)w] && v >= valley[i + (size_t)w]);
            if (peakH || peakV) ridge[i] = 1;
        }
    }

    // Trace connected ridge components and break into fitted segments
    std::vector<uint8_t> vis(n, 0);
    const int d8[8][2] = {{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1}};
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            size_t startIdx = (size_t)y * (size_t)w + (size_t)x;
            if (!ridge[startIdx] || vis[startIdx]) continue;

            // Walk an ordered chain
            std::vector<Vec2> chain;
            int cx = x, cy = y;
            while (true) {
                size_t ci = (size_t)cy * (size_t)w + (size_t)cx;
                vis[ci] = 1;
                chain.push_back({(float)cx, (float)cy});
                if (chain.size() >= 28) break;
                int bestNx = -1, bestNy = -1;
                float bestV = -1.0f;
                for (auto& d : d8) {
                    int nx = cx + d[0], ny = cy + d[1];
                    if (nx <= 0 || nx >= w - 1 || ny <= 0 || ny >= h - 1) continue;
                    size_t ni = (size_t)ny * (size_t)w + (size_t)nx;
                    if (ridge[ni] && !vis[ni] && valley[ni] > bestV) {
                        bestV = valley[ni]; bestNx = nx; bestNy = ny;
                    }
                }
                if (bestNx < 0) break;
                cx = bestNx; cy = bestNy;
            }

            if (chain.size() < 4) continue;
            Vec2 p0 = chain.front(), p1 = chain.back();
            Vec2 chord = p1 - p0;
            float len = chord.len();
            if (len < 3.0f) continue;

            Vec2 mid = (p0 + p1) * 0.5f;
            Vec2 perp(-chord.y / len, chord.x / len);
            float maxSag = 0.0f;
            float sumR = 0, sumG = 0, sumB = 0;
            for (const auto& pt : chain) {
                float sag = (pt - mid).dot(perp);
                if (std::fabs(sag) > std::fabs(maxSag)) maxSag = sag;
                size_t pi = ((size_t)std::clamp((int)pt.y, 0, h - 1) * (size_t)w + (size_t)std::clamp((int)pt.x, 0, w - 1)) * 4u;
                sumR += rgba[pi]; sumG += rgba[pi + 1]; sumB += rgba[pi + 2];
            }

            ContourSeg seg;
            seg.cx = mid.x + perp.x * (maxSag * 0.5f);
            seg.cy = mid.y + perp.y * (maxSag * 0.5f);
            seg.length = len;
            seg.thickness = std::clamp(1.6f + std::fabs(maxSag) * 0.25f, 1.2f, 5.5f);
            seg.angleDeg = std::atan2(chord.y, chord.x) * kRad2Deg;
            seg.curvature = maxSag / std::max(4.0f, len);
            seg.r = clampU8((int)(sumR / (float)chain.size()));
            seg.g = clampU8((int)(sumG / (float)chain.size()));
            seg.b = clampU8((int)(sumB / (float)chain.size()));
            segs.push_back(seg);
        }
    }

    // Sort longest / strongest contours first
    std::sort(segs.begin(), segs.end(), [](const ContourSeg& a, const ContourSeg& b) {
        return a.length > b.length;
    });
    return segs;
}

static void writeContoursBin(const std::string& path, const std::vector<ContourSeg>& segs) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return;
    uint32_t cnt = (uint32_t)segs.size();
    f.write(reinterpret_cast<const char*>(&cnt), 4);
    for (const auto& s : segs) {
        float row[6] = {s.cx, s.cy, s.length, s.thickness, s.angleDeg, s.curvature};
        f.write(reinterpret_cast<const char*>(row), sizeof(row));
    }
}

static void buildImportanceMapRaw(
    const uint8_t* rgba, int w, int h,
    const PreprocessParams& prep,
    int tileX0, int tileY0, int fullW, int fullH,
    std::vector<uint8_t>& outWeights
) {
    outWeights.assign((size_t)w * (size_t)h, 16);
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            if (rgba[i * 4 + 3] < 128) { outWeights[i] = 16; continue; }
            float gx = std::fabs(rgbLuma(rgba[(i + 1) * 4], rgba[(i + 1) * 4 + 1], rgba[(i + 1) * 4 + 2]) -
                                 rgbLuma(rgba[(i - 1) * 4], rgba[(i - 1) * 4 + 1], rgba[(i - 1) * 4 + 2]));
            float gy = std::fabs(rgbLuma(rgba[(i + (size_t)w) * 4], rgba[(i + (size_t)w) * 4 + 1], rgba[(i + (size_t)w) * 4 + 2]) -
                                 rgbLuma(rgba[(i - (size_t)w) * 4], rgba[(i - (size_t)w) * 4 + 1], rgba[(i - (size_t)w) * 4 + 2]));
            float g = std::sqrt(gx * gx + gy * gy);
            float wv = 1.0f + g * 8.0f;

            if (prep.useFocus) {
                float u = (float)(tileX0 + x) / (float)std::max(1, fullW);
                float v = (float)(tileY0 + y) / (float)std::max(1, fullH);
                if (u >= prep.focusRect[0] && u <= prep.focusRect[2] &&
                    v >= prep.focusRect[1] && v <= prep.focusRect[3]) {
                    wv *= prep.focusBoost;
                }
            }
            outWeights[i] = clampU8((int)std::round(wv * 16.0f));
        }
    }
}

// Convert Black-Hat contour segments into crisp Unicode typographic lineart layers (fontIndex = 0)
static void generateUnicodeLineartShapes(
    const std::vector<ContourSeg>& segs,
    int maxUnicodeShapes,
    std::vector<DecalShape>& outShapes
) {
    int added = 0;
    for (const auto& s : segs) {
        if (added >= maxUnicodeShapes) break;
        DecalShape ds;
        ds.isText = true;
        ds.sticker = 1;
        ds.fontIndex = 0; // LegacyRuntime.ttf (broad Unicode coverage in CPM2)
        ds.cx = s.cx;
        ds.cy = s.cy;
        ds.r = s.r; ds.g = s.g; ds.b = s.b; ds.a = 255;

        float absCurv = std::fabs(s.curvature);
        if (absCurv > 0.22f) {
            // Strong corner / tight curve -> box-drawing arc or katakana
            ds.text = (s.curvature > 0.0f) ? "\xE2\x95\xAE" : "\xE2\x95\xB0"; // "╮" or "╰"
            ds.w = s.length * 1.05f;
            ds.h = std::max(s.thickness * 2.5f, s.length * 0.55f);
            ds.angle = s.angleDeg;
        } else if (absCurv > 0.07f) {
            // Smooth curve -> parenthesis "(" or ")" rotated perpendicular to chord!
            ds.text = (s.curvature > 0.0f) ? "(" : ")";
            ds.w = std::max(s.thickness * 2.2f, s.length * 0.35f);
            ds.h = s.length * 1.15f;
            ds.angle = s.angleDeg + 90.0f;
        } else {
            // Straight crisp ink stroke -> "|" rotated along chord!
            ds.text = "|";
            ds.w = std::max(2.2f, s.thickness * 1.8f);
            ds.h = s.length * 1.12f;
            ds.angle = s.angleDeg + 90.0f;
        }
        outShapes.push_back(std::move(ds));
        added++;
    }
}

EvalMetrics evaluateDecalShapes(
    const uint8_t* targetRGBA, int w, int h,
    const std::vector<DecalShape>& shapes,
    const SpriteCatalog& catalog,
    const float bgPaintRGB[3],
    std::vector<uint8_t>* outRenderedRGBA,
    std::vector<uint8_t>* outErrorMapRGBA,
    std::vector<float>* outPerPixelError,
    const float* focusRectNorm
) {
    EvalMetrics m;
    if (!targetRGBA || w <= 0 || h <= 0) return m;
    const size_t n = (size_t)w * (size_t)h;
    std::vector<float> cr(n, 0.0f), cg(n, 0.0f), cb(n, 0.0f), ca(n, 0.0f);

    for (const auto& s : shapes) {
        const SpriteEntry* se = s.isText ? nullptr : catalog.find(s.sticker);
        if (!s.isText && (!se || se->mask256.size() != 256 * 256)) continue;
        float rad = s.angle * kDeg2Rad;
        float cosA = std::cos(rad), sinA = std::sin(rad);
        float hw = s.w * 0.5f, hh = s.h * 0.5f;
        float extX = std::fabs(cosA) * hw + std::fabs(sinA) * hh;
        float extY = std::fabs(sinA) * hw + std::fabs(cosA) * hh;
        int x0 = std::max(0, (int)std::floor(s.cx - extX));
        int x1 = std::min(w - 1, (int)std::ceil(s.cx + extX));
        int y0 = std::max(0, (int)std::floor(s.cy - extY));
        int y1 = std::min(h - 1, (int)std::ceil(s.cy + extY));
        float invW = 1.0f / std::max(0.5f, s.w);
        float invH = 1.0f / std::max(0.5f, s.h);
        float saBase = (float)s.a / 255.0f;
        float sr = (float)s.r, sg = (float)s.g, sb = (float)s.b;

        for (int y = y0; y <= y1; ++y) {
            float dy = (float)y + 0.5f - s.cy;
            for (int x = x0; x <= x1; ++x) {
                float dx = (float)x + 0.5f - s.cx;
                float u = (dx * cosA + dy * sinA) * invW + 0.5f;
                float v = (-dx * sinA + dy * cosA) * invH + 0.5f;
                if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f) continue;
                float a = 0.0f;
                if (s.isText) {
                    // Thin stroke approximation for Unicode lineart glyphs (|, (, ), ╰, ╮)
                    float strokeMask = (s.text == "(" || s.text == ")")
                        ? std::max(0.0f, 1.0f - std::fabs(u - (0.35f + 0.30f * (4.0f * (v - 0.5f) * (v - 0.5f)))) * 5.0f)
                        : std::max(0.0f, 1.0f - std::fabs(u - 0.5f) * 4.5f);
                    a = strokeMask * saBase;
                } else {
                    float uu = s.flipX ? (1.0f - u) : u;
                    int mx = std::clamp((int)(uu * 255.0f), 0, 255);
                    int my = std::clamp((int)(v * 255.0f), 0, 255);
                    a = ((float)se->mask256[(size_t)my * 256u + (size_t)mx] / 255.0f) * saBase;
                }
                if (a <= 0.002f) continue;
                size_t pi = (size_t)y * (size_t)w + (size_t)x;
                float invA = 1.0f - a;
                cr[pi] = sr * a + cr[pi] * invA;
                cg[pi] = sg * a + cg[pi] * invA;
                cb[pi] = sb * a + cb[pi] * invA;
                ca[pi] = a + ca[pi] * invA;
            }
        }
    }

    float pr = bgPaintRGB[0] * 255.0f, pg = bgPaintRGB[1] * 255.0f, pb = bgPaintRGB[2] * 255.0f;
    if (outRenderedRGBA) outRenderedRGBA->resize(n * 4u);
    if (outErrorMapRGBA) outErrorMapRGBA->resize(n * 4u);
    if (outPerPixelError) outPerPixelError->assign(n, 0.0f);

    std::vector<float> tgtLuma(n, 0.0f), renLuma(n, 0.0f);
    double sumAll = 0, sumFg = 0, sumEdge = 0, sumFlat = 0, sumSil = 0;
    int cntAll = 0, cntFg = 0, cntEdge = 0, cntFlat = 0, cntSil = 0;
    int spill = 0, holes = 0, interSil = 0, unionSil = 0;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            bool isFg = (targetRGBA[i * 4 + 3] >= 128);
            bool renCov = (ca[i] >= 0.35f);
            float tr = isFg ? (float)targetRGBA[i * 4] : pr;
            float tg = isFg ? (float)targetRGBA[i * 4 + 1] : pg;
            float tb = isFg ? (float)targetRGBA[i * 4 + 2] : pb;

            float rr = cr[i] + pr * (1.0f - ca[i]);
            float rg = cg[i] + pg * (1.0f - ca[i]);
            float rb = cb[i] + pb * (1.0f - ca[i]);
            float dist = std::sqrt((rr - tr) * (rr - tr) + (rg - tg) * (rg - tg) + (rb - tb) * (rb - tb)) / 1.73205f;

            tgtLuma[i] = 0.299f * tr + 0.587f * tg + 0.114f * tb;
            renLuma[i] = 0.299f * rr + 0.587f * rg + 0.114f * rb;

            if (outPerPixelError) {
                float normErr = std::clamp(dist / 65.0f, 0.0f, 1.0f);
                if (!isFg && renCov) normErr = std::max(normErr, 0.85f);
                (*outPerPixelError)[i] = normErr;
            }
            if (outRenderedRGBA) {
                (*outRenderedRGBA)[i * 4 + 0] = clampU8((int)std::round(rr));
                (*outRenderedRGBA)[i * 4 + 1] = clampU8((int)std::round(rg));
                (*outRenderedRGBA)[i * 4 + 2] = clampU8((int)std::round(rb));
                (*outRenderedRGBA)[i * 4 + 3] = 255;
            }
            if (outErrorMapRGBA) {
                float t = std::clamp(dist / 65.0f, 0.0f, 1.0f);
                uint8_t er, eg, eb;
                if (!isFg && renCov) {
                    er = 255; eg = 40; eb = 220; // Magenta = spill outside silhouette
                } else if (t < 0.5f) {
                    float u = t * 2.0f;
                    er = clampU8((int)(18 + u * 232));
                    eg = clampU8((int)(55 + u * 175));
                    eb = clampU8((int)(65 - u * 45));
                } else {
                    float u = (t - 0.5f) * 2.0f;
                    er = 250;
                    eg = clampU8((int)(230 - u * 200));
                    eb = 20;
                }
                (*outErrorMapRGBA)[i * 4 + 0] = er;
                (*outErrorMapRGBA)[i * 4 + 1] = eg;
                (*outErrorMapRGBA)[i * 4 + 2] = eb;
                (*outErrorMapRGBA)[i * 4 + 3] = 255;
            }

            if (isFg && renCov) interSil++;
            if (isFg || renCov) unionSil++;

            if (x == 0 || x == w - 1 || y == 0 || y == h - 1) continue;
            sumAll += dist; cntAll++;
            if (!isFg && renCov) spill++;
            if (isFg && !renCov) holes++;

            bool nearRim = isFg && (targetRGBA[(i - 1) * 4 + 3] < 128 || targetRGBA[(i + 1) * 4 + 3] < 128 ||
                                    targetRGBA[(i - (size_t)w) * 4 + 3] < 128 || targetRGBA[(i + (size_t)w) * 4 + 3] < 128);
            if (nearRim) { sumSil += dist; cntSil++; }
            if (isFg) {
                sumFg += dist; cntFg++;
                float gx = std::fabs(rgbLuma(targetRGBA[(i + 1) * 4], targetRGBA[(i + 1) * 4 + 1], targetRGBA[(i + 1) * 4 + 2]) -
                                     rgbLuma(targetRGBA[(i - 1) * 4], targetRGBA[(i - 1) * 4 + 1], targetRGBA[(i - 1) * 4 + 2]));
                float gy = std::fabs(rgbLuma(targetRGBA[(i + (size_t)w) * 4], targetRGBA[(i + (size_t)w) * 4 + 1], targetRGBA[(i + (size_t)w) * 4 + 2]) -
                                     rgbLuma(targetRGBA[(i - (size_t)w) * 4], targetRGBA[(i - (size_t)w) * 4 + 1], targetRGBA[(i - (size_t)w) * 4 + 2]));
                if (gx + gy > 0.14f) { sumEdge += dist; cntEdge++; }
                else { sumFlat += dist; cntFlat++; }
            }
        }
    }

    // 8x8 Windowed Structural Similarity (SSIM) strictly over blocks touching foreground!
    // Catches blurry/incoherent faces, eyes & edges that pixel-average RGB distance misses!
    const float C1 = (0.01f * 255.0f) * (0.01f * 255.0f);
    const float C2 = (0.03f * 255.0f) * (0.03f * 255.0f);
    double ssimSum = 0.0, focusSsimSum = 0.0;
    int ssimBlocks = 0, focusBlocks = 0;

    const float fx0 = focusRectNorm ? focusRectNorm[0] : 0.25f;
    const float fy0 = focusRectNorm ? focusRectNorm[1] : 0.20f;
    const float fx1 = focusRectNorm ? focusRectNorm[2] : 0.75f;
    const float fy1 = focusRectNorm ? focusRectNorm[3] : 0.75f;

    for (int by = 0; by + 8 <= h; by += 4) {
        for (int bx = 0; bx + 8 <= w; bx += 4) {
            int fgInBlock = 0;
            double muX = 0, muY = 0;
            for (int dy = 0; dy < 8; ++dy) {
                for (int dx = 0; dx < 8; ++dx) {
                    size_t idx = (size_t)(by + dy) * (size_t)w + (size_t)(bx + dx);
                    if (targetRGBA[idx * 4 + 3] >= 128) fgInBlock++;
                    muX += tgtLuma[idx];
                    muY += renLuma[idx];
                }
            }
            if (fgInBlock < 12) continue; // ignore empty background blocks!
            muX /= 64.0; muY /= 64.0;
            double varX = 0, varY = 0, covXY = 0;
            for (int dy = 0; dy < 8; ++dy) {
                for (int dx = 0; dx < 8; ++dx) {
                    size_t idx = (size_t)(by + dy) * (size_t)w + (size_t)(bx + dx);
                    double ex = tgtLuma[idx] - muX;
                    double ey = renLuma[idx] - muY;
                    varX += ex * ex; varY += ey * ey; covXY += ex * ey;
                }
            }
            varX /= 63.0; varY /= 63.0; covXY /= 63.0;
            double num = (2.0 * muX * muY + C1) * (2.0 * covXY + C2);
            double den = (muX * muX + muY * muY + C1) * (varX + varY + C2);
            double s = std::clamp(num / std::max(den, 1e-6), 0.0, 1.0);
            ssimSum += s; ssimBlocks++;

            float u = ((float)bx + 4.0f) / (float)w;
            float v = ((float)by + 4.0f) / (float)h;
            if (u >= fx0 && u <= fx1 && v >= fy0 && v <= fy1) {
                focusSsimSum += s; focusBlocks++;
            }
        }
    }

    m.valid = true;
    m.fgPx = std::max(1, cntFg);
    m.allErr = cntAll > 0 ? (float)(sumAll / cntAll) : 0.0f;
    m.fgErr = cntFg > 0 ? (float)(sumFg / cntFg) : m.allErr;
    m.edgeErr = cntEdge > 0 ? (float)(sumEdge / cntEdge) : m.fgErr;
    m.flatErr = cntFlat > 0 ? (float)(sumFlat / cntFlat) : m.fgErr;
    m.silErr = cntSil > 0 ? (float)(sumSil / cntSil) : m.fgErr;
    m.ssimPct = ssimBlocks > 0 ? (float)((ssimSum / ssimBlocks) * 100.0) : 0.0f;
    m.focusSSIMPct = focusBlocks > 0 ? (float)((focusSsimSum / focusBlocks) * 100.0) : m.ssimPct;
    m.silIoUPct = unionSil > 0 ? (float)((double)interSil / (double)unionSil * 100.0) : 0.0f;
    m.spillPx = spill;
    m.holesPx = holes;
    m.spillPct = (float)((double)spill / (double)m.fgPx * 100.0);
    m.holesPct = (float)((double)holes / (double)m.fgPx * 100.0);

    // Honest strict composite fidelity score (0..100%) — NEVER diluted by empty background!
    float s_ssim  = std::clamp(m.ssimPct, 0.0f, 100.0f);
    float s_focus = std::clamp(m.focusSSIMPct, 0.0f, 100.0f);
    float s_fg    = std::clamp(100.0f - m.fgErr * 2.15f, 0.0f, 100.0f);
    float s_edge  = std::clamp(100.0f - m.edgeErr * 1.25f, 0.0f, 100.0f);
    float s_sil   = std::clamp(m.silIoUPct, 0.0f, 100.0f);
    float s_clean = std::clamp(100.0f - (m.spillPct * 7.0f + m.holesPct * 5.0f), 0.0f, 100.0f);

    m.fidelityPct = 0.30f * s_ssim + 0.18f * s_focus + 0.22f * s_fg + 0.14f * s_edge + 0.08f * s_sil + 0.08f * s_clean;

    // Strict, honest verdict — refuses to say "Fidele" if structure, focus, or edges are poor!
    if (m.fidelityPct >= 88.0f && m.ssimPct >= 86.0f && m.fgErr <= 19.0f && m.spillPct <= 2.0f) {
        m.verdict = "Fidele";
        m.advice = "Tres bonne coherence visuelle et structurelle — pret pour injection.";
    } else if (m.fidelityPct >= 76.0f && m.ssimPct >= 74.0f) {
        m.verdict = "Correct";
        if (m.focusSSIMPct < 78.0f) m.advice = "Visage/Focus encore un peu flou : lancez Auto-Converge ou tracez un Focus plus serre.";
        else if (m.edgeErr > 48.0f) m.advice = "Contours perfectibles : utilisez Auto-Converge ou augmentez le Budget/Contraste.";
        else m.advice = "Bon rendu global, une passe Auto-Converge supplementaire affinera les micro-details.";
    } else if (m.fidelityPct >= 60.0f) {
        m.verdict = "Moyen";
        if (m.spillPct > 2.5f) m.advice = "Fort debordement hors silhouette : augmentez Penalite debordement (3.0+).";
        else if (m.holesPct > 4.0f) m.advice = "Zones non couvertes : activez Posterisation 10-12 ou augmentez le Budget.";
        else m.advice = "Correspondance insuffisante : activez Auto-Converge (3-5 passes) et Engine v5 Hybride.";
    } else {
        m.verdict = "Incoherent";
        if (m.silIoUPct < 70.0f) m.advice = "Silhouette tres eloignee : verifiez le detourage (onglet Damier) et le Budget de formes.";
        else m.advice = "Rendu trop eloigne de l'image originale : passez en Mode Auto-Converge HQ (240+ formes, Engine v5).";
    }
    return m;
}

void buildDiagnosticPreview(DecalItem& decal, const float paintColor[3]) {
    if (decal.origRGBA.empty()) return;
    std::vector<uint8_t> posterRGBA;
    if (decal.prepDirty || decal.prepRGBA.empty()) {
        preprocessDecalImage(decal, &posterRGBA);
    } else {
        posterRGBA = decal.prepRGBA;
        if (decal.prep.posterizeColors >= 2) {
            posterizeKMeans(posterRGBA, decal.prepW, decal.prepH, decal.prep.posterizeColors);
        }
    }
    const int W = decal.prepW, H = decal.prepH;
    if (W <= 0 || H <= 0) return;

    const auto& src = (decal.prep.posterizeColors >= 2 && posterRGBA.size() == decal.prepRGBA.size())
                      ? posterRGBA : decal.prepRGBA;
    decal.diagPreviewRGBA.resize((size_t)W * (size_t)H * 4u);

    uint8_t pr = clampU8((int)std::round(paintColor[0] * 255.0f));
    uint8_t pg = clampU8((int)std::round(paintColor[1] * 255.0f));
    uint8_t pb = clampU8((int)std::round(paintColor[2] * 255.0f));

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            size_t i = ((size_t)y * (size_t)W + (size_t)x) * 4u;
            uint8_t a = src[i + 3];
            if (a >= 128) {
                decal.diagPreviewRGBA[i + 0] = src[i + 0];
                decal.diagPreviewRGBA[i + 1] = src[i + 1];
                decal.diagPreviewRGBA[i + 2] = src[i + 2];
                decal.diagPreviewRGBA[i + 3] = 255;
            } else if (decal.previewMode == 1) {
                // Preview directly on current vehicle paint color!
                decal.diagPreviewRGBA[i + 0] = pr;
                decal.diagPreviewRGBA[i + 1] = pg;
                decal.diagPreviewRGBA[i + 2] = pb;
                decal.diagPreviewRGBA[i + 3] = 255;
            } else {
                // Crisp dark checkerboard showing exact transparent cutout!
                bool chk = (((x / 10) + (y / 10)) & 1) != 0;
                uint8_t c = chk ? 52 : 34;
                decal.diagPreviewRGBA[i + 0] = c;
                decal.diagPreviewRGBA[i + 1] = c + 2;
                decal.diagPreviewRGBA[i + 2] = c + 6;
                decal.diagPreviewRGBA[i + 3] = 255;
            }
        }
    }

    // Extract Black-Hat contours for live tile density estimation AND contour overlay mode (previewMode == 2)
    auto contours = extractBlackHatContours(decal.prepRGBA.data(), W, H, decal.conv.lineartThreshold);
    decal.lastContourCount = (int)contours.size();

    if (decal.previewMode == 2) {
        // Dim background slightly and paint detected Black-Hat contour strokes in bright cyan!
        for (size_t i = 0; i < (size_t)W * (size_t)H; ++i) {
            decal.diagPreviewRGBA[i * 4 + 0] = (uint8_t)(decal.diagPreviewRGBA[i * 4 + 0] * 0.45f);
            decal.diagPreviewRGBA[i * 4 + 1] = (uint8_t)(decal.diagPreviewRGBA[i * 4 + 1] * 0.45f);
            decal.diagPreviewRGBA[i * 4 + 2] = (uint8_t)(decal.diagPreviewRGBA[i * 4 + 2] * 0.45f);
        }
        for (const auto& s : contours) {
            float rad = s.angleDeg * kDeg2Rad;
            float dx = std::cos(rad) * (s.length * 0.5f);
            float dy = std::sin(rad) * (s.length * 0.5f);
            int steps = std::max(4, (int)std::ceil(s.length));
            for (int k = 0; k <= steps; ++k) {
                float t = (float)k / (float)steps - 0.5f;
                int px = (int)std::round(s.cx + dx * 2.0f * t);
                int py = (int)std::round(s.cy + dy * 2.0f * t);
                for (int oy = -1; oy <= 1; ++oy) {
                    for (int ox = -1; ox <= 1; ++ox) {
                        int nx = px + ox, ny = py + oy;
                        if (nx >= 0 && nx < W && ny >= 0 && ny < H) {
                            size_t pi = ((size_t)ny * (size_t)W + (size_t)nx) * 4u;
                            decal.diagPreviewRGBA[pi + 0] = 40;
                            decal.diagPreviewRGBA[pi + 1] = 245;
                            decal.diagPreviewRGBA[pi + 2] = 255;
                        }
                    }
                }
            }
        }
    }

    // Estimate per-tile budget live so the UI shows exact budgets inside the tile grid!
    const int cols = std::clamp(decal.tiling.cols, 1, 4);
    const int rows = std::clamp(decal.tiling.rows, 1, 4);
    float scores[16] = {};
    float totalSc = 0.0f;
    for (int ty = 0; ty < rows; ++ty) {
        for (int tx = 0; tx < cols; ++tx) {
            int idx = ty * cols + tx;
            int x0 = (tx * W) / cols, x1 = ((tx + 1) * W) / cols;
            int y0 = (ty * H) / rows, y1 = ((ty + 1) * H) / rows;
            int fgCnt = 0; float focHit = 0.0f;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    if (decal.prepRGBA[((size_t)y * (size_t)W + (size_t)x) * 4u + 3u] >= 128) {
                        fgCnt++;
                        if (decal.prep.useFocus) {
                            float u = (float)x / (float)W, v = (float)y / (float)H;
                            if (u >= decal.prep.focusRect[0] && u <= decal.prep.focusRect[2] &&
                                v >= decal.prep.focusRect[1] && v <= decal.prep.focusRect[3]) focHit += 1.0f;
                        }
                    }
                }
            }
            float cLen = 0.0f;
            for (const auto& c : contours) {
                if (c.cx >= x0 && c.cx < x1 && c.cy >= y0 && c.cy < y1) cLen += c.length;
            }
            if (fgCnt < 16) scores[idx] = 0.0f;
            else if (decal.tiling.autoBudgetByDensity) {
                float fgFrac = (float)fgCnt / (float)std::max(1, (x1 - x0) * (y1 - y0));
                float focMul = 1.0f + (focHit / (float)std::max(1, fgCnt)) * (decal.prep.focusBoost - 1.0f);
                scores[idx] = (0.35f * fgFrac + 0.65f * (cLen / 200.0f + 0.1f)) * focMul;
            } else {
                scores[idx] = std::max(0.05f, decal.tiling.manualWeights[idx]);
            }
            totalSc += scores[idx];
        }
    }
    int totB = std::clamp(decal.conv.totalShapes, 4, kQuotaBody);
    int assigned = 0, bestI = -1; float bestS = -1.0f;
    for (int i = 0; i < 16; ++i) decal.tiling.lastTileBudgets[i] = 0;
    for (int i = 0; i < cols * rows; ++i) {
        if (scores[i] > 0.0f && totalSc > 1e-6f) {
            decal.tiling.lastTileBudgets[i] = std::max(4, (int)std::floor((scores[i] / totalSc) * totB));
            assigned += decal.tiling.lastTileBudgets[i];
            if (scores[i] > bestS) { bestS = scores[i]; bestI = i; }
        }
    }
    if (bestI >= 0) decal.tiling.lastTileBudgets[bestI] = std::max(0, decal.tiling.lastTileBudgets[bestI] + (totB - assigned));
}

static bool runProcessHidden(const std::string& cmdLine) {
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::string mutableCmd = cmdLine;
    if (!CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode == 0;
}

ConversionWorker::~ConversionWorker() {
    cancel();
    if (m_thread.joinable()) m_thread.join();
}

void ConversionWorker::cancel() {
    m_cancel.store(true);
}

void ConversionWorker::requestStopKeepBest() {
    m_cancel.store(true);
    setStatus("Arret demande — conservation de la meilleure passe trouvee...");
}

float ConversionWorker::elapsedSeconds() const {
    if (!m_running.load()) return 0.0f;
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(now - m_startTime).count();
}

void ConversionWorker::setStatus(const std::string& s) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_status = s;
}

std::string ConversionWorker::statusText() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_status;
}

static void applySnapshotToDecal(
    DecalItem& dstDecal,
    const std::vector<DecalShape>& shapes,
    int w, int h,
    const EvalMetrics& metrics,
    const std::vector<uint8_t>& resRGBA,
    const std::vector<uint8_t>& errRGBA,
    const int tileBudgets[16],
    int mode,
    const std::string& passTag
) {
    dstDecal.shapes = shapes;
    dstDecal.srcW = w;
    dstDecal.srcH = h;
    dstDecal.metrics = metrics;
    dstDecal.resultRGBA = resRGBA;
    dstDecal.errorMapRGBA = errRGBA;
    for (int i = 0; i < 16; ++i) dstDecal.tiling.lastTileBudgets[i] = tileBudgets[i];

    ConversionIteration iter;
    const char* mTag = (mode == CONV_TRIANGLES_CURVES) ? "Tri+Courbes"
                     : (mode == CONV_TRIANGLES_UNICODE) ? "Tri+Unicode" : "Engine v5";
    char lbl[160];
    std::snprintf(lbl, sizeof(lbl), "#%d %s%s (%df | Fid %.0f%% | SSIM %.0f%%)",
                  (int)dstDecal.history.size() + 1, mTag, passTag.c_str(),
                  (int)shapes.size(), metrics.fidelityPct, metrics.ssimPct);
    iter.label = lbl;
    iter.mode = mode;
    iter.srcW = w; iter.srcH = h;
    iter.metrics = metrics;
    iter.shapes = shapes;
    iter.resultRGBA = resRGBA;
    iter.errorMapRGBA = errRGBA;
    if (dstDecal.history.size() >= 12) dstDecal.history.erase(dstDecal.history.begin());
    dstDecal.history.push_back(std::move(iter));
    dstDecal.activeHistoryIdx = (int)dstDecal.history.size() - 1;
    dstDecal.previewMode = 3;
}

bool ConversionWorker::pollIntermediate(DecalItem& dstDecal, int& outDecalIdx) {
    if (!m_hasIntermediate.load()) return false;
    std::lock_guard<std::mutex> lk(m_mtx);
    outDecalIdx = m_decalIdx;
    applySnapshotToDecal(dstDecal, m_resultShapes, m_resultW, m_resultH, m_resultMetrics,
                         m_resultRGBA, m_resultErrorRGBA, m_tileBudgets, m_resultMode, m_resultPassLabel);
    m_hasIntermediate.store(false);
    return true;
}

bool ConversionWorker::pollCompleted(DecalItem& dstDecal, int& outDecalIdx) {
    if (!m_completed.load()) return false;
    if (m_thread.joinable()) m_thread.join();
    std::lock_guard<std::mutex> lk(m_mtx);
    outDecalIdx = m_decalIdx;
    if (!m_resultShapes.empty()) {
        // Avoid duplicating the exact same final snapshot if pollIntermediate already pushed it
        bool alreadyLatest = (!dstDecal.history.empty() &&
                              dstDecal.history.back().shapes.size() == m_resultShapes.size() &&
                              std::fabs(dstDecal.history.back().metrics.fidelityPct - m_resultMetrics.fidelityPct) < 0.05f);
        if (!alreadyLatest) {
            applySnapshotToDecal(dstDecal, m_resultShapes, m_resultW, m_resultH, m_resultMetrics,
                                 m_resultRGBA, m_resultErrorRGBA, m_tileBudgets, m_resultMode, m_resultPassLabel);
        } else {
            dstDecal.shapes = m_resultShapes;
            dstDecal.srcW = m_resultW; dstDecal.srcH = m_resultH;
            dstDecal.metrics = m_resultMetrics;
            dstDecal.resultRGBA = m_resultRGBA;
            dstDecal.errorMapRGBA = m_resultErrorRGBA;
        }
    }
    m_hasIntermediate.store(false);
    m_completed.store(false);
    return true;
}

bool ConversionWorker::start(
    int decalIndex,
    const DecalItem& decalSnapshot,
    const SpriteCatalog& catalog,
    const std::string& projectRootDir,
    const float paintColor[3]
) {
    if (m_running.load()) return false;
    if (m_thread.joinable()) m_thread.join();

    const int maxPasses = decalSnapshot.conv.autoConverge ? std::clamp(decalSnapshot.conv.maxAutoPasses, 1, 6) : 1;
    m_decalIdx = decalIndex;
    m_cancel.store(false);
    m_completed.store(false);
    m_hasIntermediate.store(false);
    m_progress.store(0.01f);
    m_currentPass.store(1);
    m_totalPasses.store(maxPasses);
    m_bestFidelity.store(0.0f);
    m_startTime = std::chrono::steady_clock::now();
    m_running.store(true);
    setStatus("Pre-traitement & analyse des contours...");

    DecalItem snap = decalSnapshot;
    SpriteCatalog catCopy = catalog;
    float pCol[3] = {paintColor[0], paintColor[1], paintColor[2]};

    m_thread = std::thread([this, snap, catCopy, projectRootDir, pCol, maxPasses]() mutable {
        std::vector<uint8_t> posterRGBA;
        if (!preprocessDecalImage(snap, &posterRGBA)) {
            setStatus("Erreur : image source invalide.");
            m_running.store(false);
            return;
        }

        const int W = snap.prepW;
        const int H = snap.prepH;
        const size_t nPix = (size_t)W * (size_t)H;
        const int cols = std::clamp(snap.tiling.cols, 1, 4);
        const int rows = std::clamp(snap.tiling.rows, 1, 4);
        const int nTiles = cols * rows;
        const int bleed = std::clamp(snap.tiling.bleedPx, 0, 16);

        auto globalContours = extractBlackHatContours(snap.prepRGBA.data(), W, H, snap.conv.lineartThreshold);

        struct TileJob {
            int tx, ty, idx;
            int ownX0, ownY0, ownX1, ownY1;
            int x0, y0, x1, y1;
            int budget = 0;
            float score = 0.0f;
        };
        std::vector<TileJob> jobs;
        jobs.reserve((size_t)nTiles);
        float totalScore = 0.0f;

        for (int ty = 0; ty < rows; ++ty) {
            for (int tx = 0; tx < cols; ++tx) {
                TileJob tj;
                tj.tx = tx; tj.ty = ty; tj.idx = ty * cols + tx;
                tj.ownX0 = (tx * W) / cols; tj.ownX1 = ((tx + 1) * W) / cols;
                tj.ownY0 = (ty * H) / rows; tj.ownY1 = ((ty + 1) * H) / rows;
                tj.x0 = std::max(0, tj.ownX0 - bleed); tj.y0 = std::max(0, tj.ownY0 - bleed);
                tj.x1 = std::min(W, tj.ownX1 + bleed); tj.y1 = std::min(H, tj.ownY1 + bleed);

                int fgCnt = 0; float focusHit = 0.0f;
                for (int y = tj.ownY0; y < tj.ownY1; ++y) {
                    for (int x = tj.ownX0; x < tj.ownX1; ++x) {
                        if (snap.prepRGBA[((size_t)y * (size_t)W + (size_t)x) * 4u + 3u] >= 128) {
                            fgCnt++;
                            if (snap.prep.useFocus) {
                                float u = (float)x / (float)W, v = (float)y / (float)H;
                                if (u >= snap.prep.focusRect[0] && u <= snap.prep.focusRect[2] &&
                                    v >= snap.prep.focusRect[1] && v <= snap.prep.focusRect[3]) focusHit += 1.0f;
                            }
                        }
                    }
                }
                float contourLen = 0.0f;
                for (const auto& c : globalContours) {
                    if (c.cx >= tj.ownX0 && c.cx < tj.ownX1 && c.cy >= tj.ownY0 && c.cy < tj.ownY1) contourLen += c.length;
                }
                if (fgCnt < 16) tj.score = 0.0f;
                else if (snap.tiling.autoBudgetByDensity) {
                    float fgFrac = (float)fgCnt / (float)std::max(1, (tj.ownX1 - tj.ownX0) * (tj.ownY1 - tj.ownY0));
                    float focusMul = 1.0f + (focusHit / (float)std::max(1, fgCnt)) * (snap.prep.focusBoost - 1.0f);
                    tj.score = (0.35f * fgFrac + 0.65f * (contourLen / 200.0f + 0.1f)) * focusMul;
                } else {
                    tj.score = std::max(0.05f, snap.tiling.manualWeights[tj.idx]);
                }
                totalScore += tj.score;
                jobs.push_back(tj);
            }
        }

        int totalBudget = std::clamp(snap.conv.totalShapes, 4, kQuotaBody);
        int unicodeBudget = 0;
        int geoBudget = totalBudget;
        int poolMode = 0;
        if (snap.conv.mode == CONV_TRIANGLES_CURVES) poolMode = 2;
        else if (snap.conv.mode == CONV_TRIANGLES_UNICODE) {
            poolMode = 1;
            geoBudget = std::max(4, (int)std::round(totalBudget * snap.conv.flatPassRatio));
            unicodeBudget = std::max(0, totalBudget - geoBudget);
        }

        int assigned = 0, bestTile = -1; float bestSc = -1.0f;
        for (auto& tj : jobs) {
            if (tj.score <= 0.0f || totalScore <= 1e-6f) tj.budget = 0;
            else {
                tj.budget = std::max(4, (int)std::floor((tj.score / totalScore) * geoBudget));
                assigned += tj.budget;
                if (tj.score > bestSc) { bestSc = tj.score; bestTile = tj.idx; }
            }
        }
        if (bestTile >= 0) jobs[(size_t)bestTile].budget = std::max(0, jobs[(size_t)bestTile].budget + (geoBudget - assigned));

        fs::path tmpDir = fs::path(projectRootDir) / "work" / "studio_tmp";
        std::error_code ec;
        fs::create_directories(tmpDir, ec);
        std::string exePath = (fs::path(projectRootDir) / "tools" / "geometrize_cli" / "build" / "geometrize_cli.exe").string();
        if (!fs::exists(exePath, ec)) exePath = (fs::path(projectRootDir) / "bin" / "geometrize_cli.exe").string();
        std::string spritesDir = (fs::path(projectRootDir) / "tools" / "geometrize_cli" / "sprites_v4").string();
        if (!fs::exists(spritesDir, ec)) spritesDir = (fs::path(projectRootDir) / "sprites_v4").string();

        int hwThreads = std::max(1, (int)std::thread::hardware_concurrency());
        int activeTiles = 0;
        for (const auto& tj : jobs) if (tj.budget > 0) activeTiles++;
        int threadsPerTile = std::max(1, hwThreads / std::max(1, std::min(activeTiles, 4)));

        // Adaptive closed-loop error feedback map (starts uniform = 1.0, boosted where comparator finds error!)
        std::vector<float> feedbackBoost(nPix, 1.0f);
        float curBgPenalty = snap.conv.bgPenalty;
        int curCandidates = snap.conv.candidates;
        int curMutations = snap.conv.mutations;
        int curPolish = 3;

        float bestFid = -1.0f;
        std::vector<DecalShape> bestShapes;
        EvalMetrics bestMetrics;
        std::vector<uint8_t> bestRenRGBA, bestErrRGBA;
        int bestPassNum = 0;

        for (int pass = 1; pass <= maxPasses; ++pass) {
            if (m_cancel.load()) break;
            m_currentPass.store(pass);
            auto passStart = std::chrono::steady_clock::now();

            std::atomic<int> finishedTiles{0};
            {
                char sb[160];
                std::snprintf(sb, sizeof(sb), "Auto-Converge Passe %d/%d (cand=%d, mut=%d, polish=%d)...",
                              pass, maxPasses, curCandidates, curMutations, curPolish);
                setStatus(sb);
            }

            auto runTile = [&](const TileJob& tj) -> std::vector<DecalShape> {
                std::vector<DecalShape> tileShapes;
                if (tj.budget <= 0 || m_cancel.load()) return tileShapes;

                int tw = tj.x1 - tj.x0;
                int th = tj.y1 - tj.y0;
                std::vector<uint8_t> tRGBA((size_t)tw * (size_t)th * 4u);
                std::vector<uint8_t> tPoster((size_t)tw * (size_t)th * 4u);
                for (int y = 0; y < th; ++y) {
                    std::memcpy(&tRGBA[(size_t)y * (size_t)tw * 4u],
                                &snap.prepRGBA[((size_t)(tj.y0 + y) * (size_t)W + (size_t)tj.x0) * 4u],
                                (size_t)tw * 4u);
                    std::memcpy(&tPoster[(size_t)y * (size_t)tw * 4u],
                                &posterRGBA[((size_t)(tj.y0 + y) * (size_t)W + (size_t)tj.x0) * 4u],
                                (size_t)tw * 4u);
                }

                std::string tag = "p" + std::to_string(pass) + "_tile_" + std::to_string(tj.idx);
                std::string rawP = (tmpDir / (tag + ".rgba")).string();
                std::string colP = (tmpDir / (tag + "_col.rgba")).string();
                std::string wtsP = (tmpDir / (tag + "_wts.raw")).string();
                std::string cntP = (tmpDir / (tag + "_cnt.bin")).string();
                std::string outJ = (tmpDir / (tag + "_shapes.json")).string();

                const auto& mainTarget = (poolMode == 1 && snap.prep.posterizeColors >= 2) ? tPoster : tRGBA;
                { std::ofstream f(rawP, std::ios::binary); f.write(reinterpret_cast<const char*>(mainTarget.data()), (std::streamsize)mainTarget.size()); }
                { std::ofstream f(colP, std::ios::binary); f.write(reinterpret_cast<const char*>(tPoster.data()), (std::streamsize)tPoster.size()); }

                // Base importance map * closed-loop residual error feedback boost!
                std::vector<uint8_t> wts;
                buildImportanceMapRaw(tRGBA.data(), tw, th, snap.prep, tj.x0, tj.y0, W, H, wts);
                for (int y = 0; y < th; ++y) {
                    for (int x = 0; x < tw; ++x) {
                        size_t gi = (size_t)(tj.y0 + y) * (size_t)W + (size_t)(tj.x0 + x);
                        size_t ti = (size_t)y * (size_t)tw + (size_t)x;
                        wts[ti] = clampU8((int)std::round((float)wts[ti] * feedbackBoost[gi]));
                    }
                }
                { std::ofstream f(wtsP, std::ios::binary); f.write(reinterpret_cast<const char*>(wts.data()), (std::streamsize)wts.size()); }

                auto tContours = extractBlackHatContours(tRGBA.data(), tw, th, snap.conv.lineartThreshold);
                writeContoursBin(cntP, tContours);

                double bgPen = (snap.prep.bgMode == BG_NONE) ? 0.0 : (double)curBgPenalty;
                int passSeed = snap.conv.seed + (pass - 1) * 7919 + tj.idx * 73;
                std::ostringstream cmd;
                cmd << "\"" << exePath << "\" "
                    << "\"" << rawP << "\" " << tw << " " << th << " "
                    << "\"" << outJ << "\" "
                    << tj.budget << " 255 "
                    << curCandidates << " " << curMutations << " "
                    << threadsPerTile << " " << passSeed << " "
                    << bgPen << " none "
                    << "\"" << wtsP << "\" "
                    << (snap.conv.adaptiveAlpha ? 1 : 0) << " "
                    << "\"" << spritesDir << "\" "
                    << "\"" << colP << "\" "
                    << "\"" << cntP << "\" "
                    << poolMode << " "
                    << curPolish;

                if (runProcessHidden(cmd.str())) {
                    std::ifstream jf(outJ);
                    if (jf) {
                        json parsed;
                        try {
                            jf >> parsed;
                            if (parsed.contains("shapes") && parsed["shapes"].is_array()) {
                                for (const auto& s : parsed["shapes"]) {
                                    float gcx = s.value("cx", 0.0f) + (float)tj.x0;
                                    float gcy = s.value("cy", 0.0f) + (float)tj.y0;
                                    const float margin = (nTiles > 1) ? 1.5f : 100.0f;
                                    if (nTiles > 1 && (gcx < tj.ownX0 - margin || gcx > tj.ownX1 + margin ||
                                                       gcy < tj.ownY0 - margin || gcy > tj.ownY1 + margin)) {
                                        continue;
                                    }
                                    DecalShape ds;
                                    ds.sticker = s.value("sticker", 87);
                                    ds.flipX = s.value("flipX", false);
                                    ds.cx = gcx; ds.cy = gcy;
                                    ds.w = s.value("w", 10.0f);
                                    ds.h = s.value("h", 10.0f);
                                    ds.angle = s.value("angle", 0.0f);
                                    if (s.contains("color") && s["color"].size() >= 4) {
                                        ds.r = (uint8_t)s["color"][0].get<int>();
                                        ds.g = (uint8_t)s["color"][1].get<int>();
                                        ds.b = (uint8_t)s["color"][2].get<int>();
                                        ds.a = (uint8_t)s["color"][3].get<int>();
                                    }
                                    tileShapes.push_back(std::move(ds));
                                }
                            }
                        } catch (...) {}
                    }
                }

                int done = ++finishedTiles;
                float passBase = (float)(pass - 1) / (float)maxPasses;
                float passSpan = 1.0f / (float)maxPasses;
                m_progress.store(passBase + passSpan * (0.10f + 0.85f * ((float)done / (float)std::max(1, activeTiles))));
                return tileShapes;
            };

            std::vector<std::future<std::vector<DecalShape>>> futs;
            for (const auto& tj : jobs) {
                if (tj.budget > 0) futs.push_back(std::async(std::launch::async, runTile, tj));
            }
            std::vector<DecalShape> passShapes;
            for (auto& fut : futs) {
                auto part = fut.get();
                passShapes.insert(passShapes.end(), part.begin(), part.end());
            }
            if (unicodeBudget > 0 && !m_cancel.load()) {
                generateUnicodeLineartShapes(globalContours, unicodeBudget, passShapes);
            }
            if ((int)passShapes.size() > totalBudget) passShapes.resize((size_t)totalBudget);

            // Real measured calibration of CPU speed on pass 1!
            if (pass == 1) {
                float passSec = std::chrono::duration<float>(std::chrono::steady_clock::now() - passStart).count();
                float pxF = ((float)snap.prep.workSize / 350.0f); pxF *= pxF;
                float units = std::max(1.0f, (float)totalBudget * ((float)curCandidates / 300.0f) * ((float)curMutations / 180.0f) * pxF);
                m_lastSecPerUnit.store(passSec / units);
            }

            // Image-to-Image Perceptual Comparator!
            std::vector<uint8_t> renRGBA, errRGBA;
            std::vector<float> perPixErr;
            const float* fRect = snap.prep.useFocus ? snap.prep.focusRect : nullptr;
            EvalMetrics pm = evaluateDecalShapes(
                snap.prepRGBA.data(), W, H, passShapes, catCopy, pCol,
                &renRGBA, &errRGBA, &perPixErr, fRect
            );

            // Strict elitism: keep highest perceptual fidelity pass!
            if (pm.fidelityPct > bestFid) {
                bestFid = pm.fidelityPct;
                bestShapes = passShapes;
                bestMetrics = pm;
                bestRenRGBA = renRGBA;
                bestErrRGBA = errRGBA;
                bestPassNum = pass;
                m_bestFidelity.store(bestFid);

                // Publish live intermediate update so the user sees the 3D car improve pass-by-pass!
                std::lock_guard<std::mutex> lk(m_mtx);
                m_resultShapes = bestShapes;
                m_resultW = W; m_resultH = H;
                m_resultMetrics = bestMetrics;
                m_resultRGBA = bestRenRGBA;
                m_resultErrorRGBA = bestErrRGBA;
                m_resultMode = snap.conv.mode;
                m_resultPassLabel = (maxPasses > 1) ? (" P" + std::to_string(pass)) : "";
                for (int i = 0; i < 16; ++i) m_tileBudgets[i] = 0;
                for (const auto& tj : jobs) if (tj.idx < 16) m_tileBudgets[tj.idx] = tj.budget;
                m_hasIntermediate.store(true);
            }

            // Stop early if target perceptual fidelity achieved!
            if (pm.fidelityPct >= snap.conv.targetFidelityPct && pm.ssimPct >= snap.conv.targetFidelityPct - 3.0f) {
                char okb[160];
                std::snprintf(okb, sizeof(okb), "Objectif atteint Passe %d/%d : Fidelite %.1f%% (SSIM %.1f%%) !",
                              pass, maxPasses, bestFid, bestMetrics.ssimPct);
                setStatus(okb);
                break;
            }
            if (pass >= maxPasses || m_cancel.load()) break;

            // Closed-loop feedback for next pass:
            // 1) Dilate & accumulate residual error map -> boost weights right where the image diverges!
            for (int y = 1; y < H - 1; ++y) {
                for (int x = 1; x < W - 1; ++x) {
                    size_t i = (size_t)y * (size_t)W + (size_t)x;
                    float localMax = perPixErr[i];
                    localMax = std::max(localMax, perPixErr[i - 1]);
                    localMax = std::max(localMax, perPixErr[i + 1]);
                    localMax = std::max(localMax, perPixErr[i - (size_t)W]);
                    localMax = std::max(localMax, perPixErr[i + (size_t)W]);
                    float boost = 1.0f + 3.2f * std::pow(localMax, 1.4f);
                    feedbackBoost[i] = std::clamp(feedbackBoost[i] * 0.45f + boost * 0.55f, 1.0f, 5.5f);
                }
            }
            // 2) Self-tune optimizer parameters based on which exact criterion lagged!
            if (pm.spillPct > 1.8f) curBgPenalty = std::min(5.0f, curBgPenalty + 0.7f);
            curCandidates = std::min(720, curCandidates + 120);
            curMutations  = std::min(380, curMutations + 55);
            curPolish     = std::min(5, curPolish + 1);
            // If holes persist and posterization was aggressive, relax posterization slightly
            if (pm.holesPct > 3.5f && snap.prep.posterizeColors > 0 && snap.prep.posterizeColors < 14) {
                snap.prep.posterizeColors = std::min(16, snap.prep.posterizeColors + 4);
                posterRGBA = snap.prepRGBA;
                posterizeKMeans(posterRGBA, W, H, snap.prep.posterizeColors);
            }
        }

        {
            std::lock_guard<std::mutex> lk(m_mtx);
            float totalSec = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_startTime).count();
            char fb[180];
            std::snprintf(fb, sizeof(fb), "Termine en %.0fs (Meilleure: Passe %d) — Fidelite %.1f%% [%s] | SSIM %.1f%% | ErrFG %.1f",
                          totalSec, bestPassNum, bestMetrics.fidelityPct, bestMetrics.verdict.c_str(),
                          bestMetrics.ssimPct, bestMetrics.fgErr);
            m_status = fb;
        }
        m_progress.store(1.0f);
        m_completed.store(true);
        m_running.store(false);
    });

    return true;
}

} // namespace cpm
