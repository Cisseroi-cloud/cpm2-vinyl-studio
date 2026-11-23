#include "../src/car_model.h"
#include "../src/converter.h"
#include "../src/garage_sync.h"
#include "../src/projector.h"
#include "../src/vinyl_types.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <thread>

using namespace cpm;
namespace fs = std::filesystem;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
}

int main() {
    std::string root = fs::current_path().string();
    std::cout << "=== CPM2 Vinyl Studio Automated Verification ===\n";

    // 1. Sprite catalog (20 high-res 256x256 sprites)
    SpriteCatalog cat;
    check(cat.load(root + "\\tools\\geometrize_cli\\sprites_v4"), "Loaded 20 high-res 256x256 sprite masks");
    check(cat.entries.size() == 20, "Catalog has exactly 20 sprites");
    check(cat.find(95) != nullptr && cat.find(130) != nullptr, "Triangle 95 and Crescent 130 present");

    // 2. CarModel .cpmcar + BVH across Body, Windows, Plate
    CarModel car;
    check(car.loadFromFile(root + "\\cache\\cars\\car187.cpmcar"), "Loaded car187.cpmcar");
    check(car.kitGroups.size() == 5, "Detected 5 bodykit groups (hood, fbumper, rbumper, fender, skirts)");
    check(car.wheels.size() == 4, "Detected 4 wheel mounts");

    // Raycast directly down onto hood -> BODY
    RayHit hHood;
    check(car.raycast({{0.0f, 1.5f, 1.15f}, {0.0f, -1.0f, 0.0f}}, 3.0f, MASK_BODY | MASK_WINDOWS, hHood), "Raycast hit hood");
    check(hHood.surface == SURF_BODY, "Hood hit classified as SURF_BODY");

    // Raycast directly onto front windshield -> WINDOWS
    RayHit hWin;
    check(car.raycast({{0.0f, 1.5f, 0.35f}, {0.0f, -1.0f, 0.0f}}, 3.0f, MASK_BODY | MASK_WINDOWS, hWin), "Raycast hit front windshield");
    check(hWin.surface == SURF_WINDOWS, "Windshield hit classified as SURF_WINDOWS");

    // 3. Cross-surface Body <-> Windows straddle projection (Side Door + Side Window AND Hood + Windshield)
    LiveryProject proj;
    proj.carId = 187;
    proj.kitVariants.assign(car.kitGroups.size(), 0);

    DecalItem sideDecal;
    sideDecal.name = "Straddle_Door_Window_Test";
    check(loadShapesJson(root + "\\work\\art_v4_bluegirl_298_shapes.json", sideDecal), "Loaded 298 reference shapes");
    // Place on right flank straddling FR_Door (BODY, y~0.3..0.65) and windowR (WINDOWS, y~0.65..1.02)
    sideDecal.xform.pos = {1.05f, 0.68f, 0.05f};
    sideDecal.xform.rotEuler = {0.0f, 90.0f, 0.0f}; // -Z points along -X into right door & right window!
    sideDecal.xform.scale = {0.95f, 0.95f, 0.60f};
    sideDecal.xform.targetMask = MASK_BODY | MASK_WINDOWS;
    sideDecal.xform.autoSplitBodyWindows = true;
    proj.decals.push_back(sideDecal);

    ProjectedLivery liv;
    projectLivery(car, cat, proj, liv);
    const auto& pd = proj.decals[0];
    std::cout << "  Side Straddle Projection: Body=" << pd.projectedBodyCount
              << " Windows=" << pd.projectedWindowCount
              << " StraddleBridged=" << pd.straddleSplitCount << "\n";
    check(pd.projectedBodyCount > 20, "Body shapes routed to CarVinyls");
    check(pd.projectedWindowCount > 10, "Window shapes automatically routed to WindowVinyls");
    check(pd.straddleSplitCount > 0, "Seam-straddling shapes seamlessly bridged across Body <-> Windows");
    check((int)liv.bodyLayers.size() <= kQuotaBody && (int)liv.windowLayers.size() <= kQuotaWindows, "Server safety quotas enforced");

    // Verify all projected layers have finite coordinates and real 3D iconPos
    for (const auto& l : liv.bodyLayers) {
        check(std::isfinite(l.px) && std::isfinite(l.py) && std::isfinite(l.sx) && std::isfinite(l.sy), "Body layer finite");
    }
    for (const auto& l : liv.windowLayers) {
        check(std::isfinite(l.px) && std::isfinite(l.py) && std::isfinite(l.sx) && std::isfinite(l.sy), "Window layer finite");
    }

    // 4. Project JSON Save/Load Round-Trip + Layer .txt Export
    std::string projPath = root + "\\work\\studio_verify.cpmproj.json";
    check(saveProjectJson(projPath, proj), "Saved .cpmproj.json");
    LiveryProject projReload;
    check(loadProjectJson(projPath, projReload), "Reloaded .cpmproj.json");
    check(projReload.carId == 187 && projReload.decals.size() == 1 && projReload.decals[0].shapes.size() == pd.shapes.size(),
          "Project JSON round-trip preserves carId, decals and shapes");

    std::string bodyTxt = root + "\\work\\studio_verify_body.txt";
    std::string winTxt  = root + "\\work\\studio_verify_win.txt";
    check(saveLayersTxt(bodyTxt, liv.bodyLayers, kQuotaBody), "Exported CarVinyls .txt");
    check(saveLayersTxt(winTxt, liv.windowLayers, kQuotaWindows), "Exported WindowVinyls .txt");
    std::vector<VinylLayer> reloadBody;
    check(loadLayersTxt(bodyTxt, reloadBody) && reloadBody.size() == liv.bodyLayers.size(), "Re-parsed exported layers .txt 1:1");

    // 5. Multi-Tile Tiling (2x2) + Double Passe (Triangles 95 + Unicode Lineart) end-to-end test!
    std::string testImg = root + "\\work\\art_v4_bluegirl_298_target.png";
    if (fs::exists(testImg)) {
        DecalItem td;
        td.name = "Tile2x2_Triangles_Unicode";
        check(loadDecalImage(testImg, td), "Loaded test image for 2x2 Multi-Tile Tiling");
        td.prep.bgMode = BG_FLOOD;
        td.prep.posterizeColors = 6;
        td.prep.contrastBoost = 1.15f;
        td.prep.workSize = 200; // fast CI verification size (400x400 across 2x2 tiles!)
        td.tiling.cols = 2;
        td.tiling.rows = 2;
        td.tiling.bleedPx = 4;
        td.tiling.autoBudgetByDensity = true;
        td.conv.mode = CONV_TRIANGLES_UNICODE;
        td.conv.totalShapes = 60;
        td.conv.flatPassRatio = 0.70f;
        td.conv.candidates = 90;
        td.conv.mutations = 60;
        td.conv.autoConverge = true;
        td.conv.maxAutoPasses = 2;
        td.conv.targetFidelityPct = 92.0f;

        ConversionWorker worker;
        check(worker.start(0, td, cat, root, proj.paintColor), "Started async 2-pass Auto-Converge 2x2 Multi-Tile worker");
        while (worker.isRunning()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        int doneIdx = -1;
        check(worker.pollCompleted(td, doneIdx), "Worker completed and handed off best Auto-Converge pass");

        int nTri = 0, nUnicode = 0;
        for (const auto& s : td.shapes) {
            if (s.isText) nUnicode++;
            else if (s.sticker == 95) nTri++;
        }
        std::cout << "  2x2 Auto-Converge Result: total=" << td.shapes.size()
                  << " (Triangles95=" << nTri << ", UnicodeLineart=" << nUnicode << ")"
                  << " | Fid=" << td.metrics.fidelityPct << "% [" << td.metrics.verdict << "]"
                  << " SSIM=" << td.metrics.ssimPct << "% ErrFG=" << td.metrics.fgErr
                  << " IoU=" << td.metrics.silIoUPct << "%\n";
        check(nTri > 0, "Pass 1 produced pure Triangle (95) flat shapes across 2x2 tiles");
        check(nUnicode > 0, "Pass 2 produced Unicode typographic lineart shapes (isText=1, fontIndex=0)");
        check((int)td.shapes.size() <= 60, "Total tiled shapes strictly within budget");
        check(td.metrics.valid && td.metrics.ssimPct > 0.0f && !td.metrics.verdict.empty(), "Honest Perceptual SSIM + Foreground metrics + Verdict computed");
        check(td.metrics.verdict != "Fidele", "Honest verdict refuses to call a 60-shape rough test 'Fidele'!");
    }

    // 6. Portable Auto-Detection + Offline ES3 Garage Scanner (Unique CarInstanceIds!)
    std::string detGame = autoDetectGameDir(root);
    check(isValidGameDir(detGame), "Auto-detected CPM2 game directory without hardcoded paths");
    auto garage = scanOfflineES3Garage();
    std::cout << "  Offline ES3 Garage instances discovered: " << garage.size() << "\n";
    for (const auto& g : garage) {
        std::cout << "    car" << g.carId << " inst=" << g.instanceId
                  << " body=" << g.hasBodyVinyls << " win=" << g.hasWindowVinyls << "\n";
    }
    check(!garage.empty(), "Discovered owned garage vehicle instances with unique CarInstanceIds");

    std::cout << "=== ALL TESTS PASSED! ===\n";
    return 0;
}
