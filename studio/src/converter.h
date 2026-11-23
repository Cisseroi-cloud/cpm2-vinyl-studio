#pragma once
#include "vinyl_types.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cpm {

struct ContourSeg {
    float cx = 0, cy = 0;
    float length = 0;
    float thickness = 1.5f;
    float angleDeg = 0;
    float curvature = 0;
    uint8_t r = 16, g = 16, b = 22;
};

// Load PNG/JPG/BMP/TGA image into DecalItem::origRGBA
bool loadDecalImage(const std::string& path, DecalItem& decal);

// Fast C++ preprocessor (cutout, hysteresis holes, strip white border, posterize 4..16, contrast, saturation)
// Updates decal.prepRGBA, decal.prepW, decal.prepH.
bool preprocessDecalImage(DecalItem& decal, std::vector<uint8_t>* outPosterizedRGBA = nullptr);

// Build visual diagnostic preview buffer for Inspector:
// mode 0 = checkerboard behind cutout, 1 = composited over active car paint, 2 = Black-Hat contour overlay
// Also updates decal.tiling.lastTileBudgets (live preview of budget per tile!) and decal.lastContourCount.
void buildDiagnosticPreview(DecalItem& decal, const float paintColor[3]);

// Extract Black-Hat dark ink valleys + NMS + contour polylines
std::vector<ContourSeg> extractBlackHatContours(
    const uint8_t* rgba, int w, int h, float minValleyStrength = 0.16f
);

// Evaluate honest multi-criteria perceptual metrics (Foreground-only MAE, windowed SSIM,
// Focus-zone SSIM, Silhouette IoU, Spill %, Holes %, strict Fidelity %) and optionally output
// rendered vectorized preview RGBA, error heatmap RGBA, and per-pixel normalized residual error [0..1].
EvalMetrics evaluateDecalShapes(
    const uint8_t* targetRGBA, int w, int h,
    const std::vector<DecalShape>& shapes,
    const SpriteCatalog& catalog,
    const float bgPaintRGB[3],
    std::vector<uint8_t>* outRenderedRGBA = nullptr,
    std::vector<uint8_t>* outErrorMapRGBA = nullptr,
    std::vector<float>* outPerPixelError = nullptr,
    const float* focusRectNorm = nullptr
);

// Asynchronous background conversion worker supporting Multi-Tile Tiling + Closed-Loop Auto-Converge!
class ConversionWorker {
public:
    ConversionWorker() = default;
    ~ConversionWorker();

    bool start(
        int decalIndex,
        const DecalItem& decalSnapshot,
        const SpriteCatalog& catalog,
        const std::string& projectRootDir,
        const float paintColor[3]
    );

    bool isRunning() const { return m_running.load(); }
    float progress() const { return m_progress.load(); }
    float elapsedSeconds() const;
    int currentPass() const { return m_currentPass.load(); }
    int totalPasses() const { return m_totalPasses.load(); }
    float bestFidelityPct() const { return m_bestFidelity.load(); }
    int targetDecalIndex() const { return m_decalIdx; }
    float lastRunSecPerUnit() const { return m_lastSecPerUnit.load(); }
    std::string statusText() const;

    // Poll intermediate pass snapshot so UI updates 3D car & history LIVE after every Auto-Converge pass!
    bool pollIntermediate(DecalItem& dstDecal, int& outDecalIdx);

    // Poll final completion; returns true once when finished and writes best result into dstDecal
    bool pollCompleted(DecalItem& dstDecal, int& outDecalIdx);

    // Gracefully stop after current pass (or immediately) while preserving the BEST pass found so far!
    void requestStopKeepBest();
    void cancel();

private:
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_completed{false};
    std::atomic<bool> m_hasIntermediate{false};
    std::atomic<float> m_progress{0.0f};
    std::atomic<int> m_currentPass{0};
    std::atomic<int> m_totalPasses{1};
    std::atomic<float> m_bestFidelity{0.0f};
    std::atomic<float> m_lastSecPerUnit{0.0f};
    std::chrono::steady_clock::time_point m_startTime;
    int m_decalIdx = -1;

    mutable std::mutex m_mtx;
    std::string m_status;
    std::vector<DecalShape> m_resultShapes;
    int m_resultW = 400, m_resultH = 400;
    int m_tileBudgets[16] = {};
    EvalMetrics m_resultMetrics;
    std::vector<uint8_t> m_resultRGBA;
    std::vector<uint8_t> m_resultErrorRGBA;
    int m_resultMode = CONV_HYBRID_V5;
    std::string m_resultPassLabel;

    void setStatus(const std::string& s);
};

} // namespace cpm
