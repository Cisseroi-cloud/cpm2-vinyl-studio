#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include "car_model.h"
#include "converter.h"
#include "garage_sync.h"
#include "math_types.h"
#include "projector.h"
#include "renderer_dx11.h"
#include "vinyl_types.h"

#include "../../third_party/stb/stb_image.h"

#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/backends/imgui_impl_win32.h"
#include "../../third_party/imgui/backends/imgui_impl_dx11.h"
#include "../../third_party/ImGuizmo/src/ImGuizmo.h"
#include "../../third_party/nlohmann/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace cpm;

// ================================ APP STATE ================================

struct StudioApp {
    std::string rootDir;
    StudioConfig config;
    RendererDX11 renderer;
    SpriteCatalog catalog;
    CarModel car;
    LiveryProject project;
    ProjectedLivery livery;
    ConversionWorker worker;

    std::vector<int> availableCarIds;
    std::vector<GarageCarInstance> garageInstances;
    std::string selectedInstanceId; // exact CarInstanceId (handles duplicate cars in garage!)
    uint32_t gamePid = 0;
    double lastPidCheckTime = -10.0;
    bool awaitingInjectorResult = false;
    double injectorStartTime = 0.0;
    InjectionReport lastInjReport;
    bool showLiveGameCaptureWindow = false;
    int liveBodyW = 0, liveBodyH = 0, liveWinW = 0, liveWinH = 0;

    OrbitCamera camera;
    ViewportOptions vpOpts;

    int viewLayout = 2; // 0 = 3D seul, 1 = UV seul, 2 = Split 3D + UV
    int selectedDecal = -1;
    int selectedText = -1;
    ImGuizmo::OPERATION gizmoOp = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE gizmoMode = ImGuizmo::LOCAL;

    bool liveryDirty = true;
    bool showGuideWindow = false;
    bool draggingFocus = false;
    ImVec2 focusDragStart{0, 0};
    std::string statusBanner = "Pret — CPM2 Vinyl Studio (DirectX 11 + Engine v5 Auto-Converge)";
    std::string activeProjectPath;

    // Viewport mouse orbit state
    bool orbiting = false;
    bool panning = false;
    ImVec2 lastMouse{0, 0};

    // Pending resize
    int pendingW = 0, pendingH = 0;

    std::string resolveInjectorDllPath() const {
        fs::path p1 = fs::path(rootDir) / "tools" / "vinyl_injector" / "build" / "CPM2_Vinyls_Injector.dll";
        if (fs::exists(p1)) return p1.string();
        return (fs::path(rootDir) / "bin" / "CPM2_Vinyls_Injector.dll").string();
    }

    std::string resolveSpritesDir() const {
        fs::path p1 = fs::path(rootDir) / "tools" / "geometrize_cli" / "sprites_v4";
        if (fs::exists(p1)) return p1.string();
        return (fs::path(rootDir) / "sprites_v4").string();
    }

    void refreshGarageOffline() {
        garageInstances = scanOfflineES3Garage();
        std::string activeInst; int activeCid = -1;
        mergeLiveGarageJson((fs::path(rootDir) / "work").string(), garageInstances, activeInst, activeCid);
        if (selectedInstanceId.empty() && !activeInst.empty()) selectedInstanceId = activeInst;
        if (selectedInstanceId.empty() && !garageInstances.empty()) {
            // Prefer first instance matching current carId, else first owned car
            for (const auto& g : garageInstances) {
                if (g.carId == project.carId) { selectedInstanceId = g.instanceId; break; }
            }
            if (selectedInstanceId.empty()) selectedInstanceId = garageInstances[0].instanceId;
        }
    }

    std::string buildExporterCommand(const std::string& args) const {
        fs::path exe = fs::path(rootDir) / "bin" / "export_car_3d.exe";
        if (fs::exists(exe)) {
            return "\"" + exe.string() + "\" " + args;
        }
        return "python \"" + (fs::path(rootDir) / "tools" / "export_car_3d.py").string() + "\" " + args;
    }

    void loadCatalogCarIds() {
        availableCarIds.clear();
        fs::path catP = fs::path(rootDir) / "cache" / "cars" / "catalog.json";
        if (!fs::exists(catP) && isValidGameDir(config.gameDir)) {
            std::string cmd = buildExporterCommand("--game-dir \"" + config.gameDir + "\" --list-json \"" + catP.string() + "\"");
            STARTUPINFOA si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, rootDir.c_str(), &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 30000);
                CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            }
        }
        if (fs::exists(catP)) {
            std::ifstream f(catP);
            json j;
            try {
                f >> j;
                if (j.contains("car_ids") && j["car_ids"].is_array()) {
                    availableCarIds = j["car_ids"].get<std::vector<int>>();
                }
            } catch (...) {}
        }
        if (availableCarIds.empty()) {
            availableCarIds = {187, 13, 1, 50};
        }
    }

    bool loadCarById(int carId) {
        fs::path carFile = fs::path(rootDir) / "cache" / "cars" / ("car" + std::to_string(carId) + ".cpmcar");
        if (!fs::exists(carFile)) {
            if (!isValidGameDir(config.gameDir)) {
                statusBanner = "Dossier CPM 2 non configure ! Allez dans Projet -> Dossier du jeu CPM 2.";
                return false;
            }
            statusBanner = "Extraction 3D du vehicule car" + std::to_string(carId) + " depuis les bundles du jeu...";
            std::string cmd = buildExporterCommand("--game-dir \"" + config.gameDir + "\" --out \"" + carFile.string() + "\" " + std::to_string(carId));
            STARTUPINFOA si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, rootDir.c_str(), &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 45000);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
        }
        if (!car.loadFromFile(carFile.string())) {
            statusBanner = "Echec chargement 3D pour car" + std::to_string(carId) + " (verifiez le dossier du jeu CPM 2)";
            return false;
        }
        project.carId = carId;
        project.kitVariants.assign(car.kitGroups.size(), 0);
        for (size_t i = 0; i < car.kitGroups.size(); ++i) project.kitVariants[i] = (int)car.kitGroups[i].defaultVariant;

        car.rebuildBVH(project.kitVariants);
        renderer.uploadCarModel(car);
        Vec3 center = car.bounds.center();
        camera.target = center;
        camera.distance = std::max(3.6f, car.bounds.extent().len() * 1.15f);
        liveryDirty = true;
        statusBanner = "Vehicule car" + std::to_string(carId) + " charge (" +
                       std::to_string(car.submeshes.size()) + " sous-meshes, " +
                       std::to_string(car.kitGroups.size()) + " groupes bodykit)";
        return true;
    }

    void refreshDecalPreviews(int idx) {
        if (idx < 0 || idx >= (int)project.decals.size()) return;
        auto& d = project.decals[(size_t)idx];
        if (!d.origRGBA.empty()) {
            renderer.updatePreviewTexture("orig_" + std::to_string(idx), d.origRGBA.data(), d.origW, d.origH);
            buildDiagnosticPreview(d, project.paintColor);
            if (!d.diagPreviewRGBA.empty()) {
                renderer.updatePreviewTexture("diag_" + std::to_string(idx), d.diagPreviewRGBA.data(), d.prepW, d.prepH);
            }
        }
        if (!d.resultRGBA.empty()) {
            renderer.updatePreviewTexture("res_" + std::to_string(idx), d.resultRGBA.data(), d.srcW, d.srcH);
        }
        if (!d.errorMapRGBA.empty()) {
            renderer.updatePreviewTexture("err_" + std::to_string(idx), d.errorMapRGBA.data(), d.srcW, d.srcH);
        }
    }

    void rebuildProjectionAndComposite() {
        projectLivery(car, catalog, project, livery);
        renderer.compositeVinyls(livery, catalog, project);
        liveryDirty = false;
    }

    void loadLiveCaptureTextures() {
        auto loadPng = [&](const std::string& path, const std::string& key, int& outW, int& outH) {
            int w = 0, h = 0, c = 0;
            unsigned char* px = stbi_load(path.c_str(), &w, &h, &c, 4);
            if (px && w > 0 && h > 0) {
                renderer.updatePreviewTexture(key, px, w, h);
                outW = w; outH = h;
                stbi_image_free(px);
            }
        };
        if (lastInjReport.hasLiveBodyCapture) loadPng(lastInjReport.liveBodyPngPath, "live_body_rt", liveBodyW, liveBodyH);
        if (lastInjReport.hasLiveWinCapture)  loadPng(lastInjReport.liveWinPngPath, "live_win_rt", liveWinW, liveWinH);
    }

    bool triggerLiveGarageScan() {
        fs::path outDir = fs::path(rootDir) / "work";
        fs::create_directories(outDir);
        {
            std::ofstream f(outDir / "cmd.txt");
            f << "scan_garage\ncapture=1\n";
        }
        std::error_code ec;
        fs::remove(outDir / "inject_done.txt", ec);
        std::string err;
        if (!injectDllIntoGame(resolveInjectorDllPath(), outDir.string(), err)) {
            statusBanner = "Sync Jeu : " + err;
            return false;
        }
        awaitingInjectorResult = true;
        injectorStartTime = ImGui::GetTime();
        statusBanner = "Synchronisation live avec Car Parking 2 en cours...";
        return true;
    }

    bool exportAndInject(bool injectInGame, bool captureAfter) {
        fs::path outDir = fs::path(rootDir) / "work";
        fs::create_directories(outDir);
        fs::path bodyPath = outDir / ("studio_car" + std::to_string(project.carId) + "_body_layers.txt");
        fs::path winPath  = outDir / ("studio_car" + std::to_string(project.carId) + "_windows_layers.txt");
        fs::path pltPath  = outDir / ("studio_car" + std::to_string(project.carId) + "_plate_layers.txt");

        bool hasBody = !livery.bodyLayers.empty();
        bool hasWin  = !livery.windowLayers.empty();
        bool hasPlt  = !livery.plateLayers.empty();

        if (!hasBody && !hasWin && !hasPlt) {
            statusBanner = "Rien a exporter : ajoutez et convertissez au moins un decal.";
            return false;
        }

        if (hasBody) saveLayersTxt(bodyPath.string(), livery.bodyLayers, kQuotaBody);
        if (hasWin)  saveLayersTxt(winPath.string(), livery.windowLayers, kQuotaWindows);
        if (hasPlt)  saveLayersTxt(pltPath.string(), livery.plateLayers, kQuotaPlate);

        if (!injectInGame) {
            statusBanner = "Exporte : Body=" + std::to_string(livery.bodyLayers.size()) +
                           " | Vitres=" + std::to_string(livery.windowLayers.size()) +
                           " | Plaques=" + std::to_string(livery.plateLayers.size()) + " -> work/studio_car*.txt";
            return true;
        }

        // Write atomic multi-surface batch command with EXACT CarInstanceId (handles duplicate cars in garage!)
        fs::path cmdPath = outDir / "cmd.txt";
        {
            std::ofstream f(cmdPath);
            f << "inject_batch\n";
            f << "car=" << project.carId << "\n";
            if (!selectedInstanceId.empty()) f << "instance=" << selectedInstanceId << "\n";
            if (hasBody) f << "body=" << bodyPath.string() << "\n";
            if (hasWin)  f << "windows=" << winPath.string() << "\n";
            if (hasPlt)  f << "plate_file=" << pltPath.string() << "\n";
            f << "plate=both\n";
            f << "backup=1\n";
            if (captureAfter) f << "capture=1\n";
        }
        std::error_code ec;
        fs::remove(outDir / "inject_done.txt", ec);
        fs::remove(outDir / "vinyl_injector.log", ec);

        std::string err;
        if (!injectDllIntoGame(resolveInjectorDllPath(), outDir.string(), err)) {
            lastInjReport = InjectionReport{};
            lastInjReport.hasReport = true;
            lastInjReport.success = false;
            lastInjReport.summaryText = err;
            statusBanner = "Injection impossible : " + err;
            return false;
        }
        awaitingInjectorResult = true;
        injectorStartTime = ImGui::GetTime();
        statusBanner = "Injection atomique envoyee a CPM 2 (car" + std::to_string(project.carId) +
                       (selectedInstanceId.empty() ? "" : (" [" + selectedInstanceId + "]")) + ") — attente confirmation...";
        return true;
    }
};

static StudioApp* g_app = nullptr;

// ================================ WIN32 DIALOGS ================================

static std::string openFileDialog(HWND owner, const char* filter, const char* initDir = nullptr) {
    char fileBuf[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = initDir;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) return std::string(fileBuf);
    return "";
}

static std::string saveFileDialog(HWND owner, const char* filter, const char* defExt, const char* initDir = nullptr) {
    char fileBuf[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = defExt;
    ofn.lpstrInitialDir = initDir;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn)) return std::string(fileBuf);
    return "";
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED && g_app) {
                g_app->pendingW = LOWORD(lParam);
                g_app->pendingH = HIWORD(lParam);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ================================ UI HELPERS ================================

static void HelpMarker(const char* desc) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static void applyPreset(DecalItem& d, int presetId) {
    switch (presetId) {
        case 0: // Test Rapide (5s)
            d.prep.posterizeColors = 10;
            d.prep.contrastBoost = 1.10f;
            d.prep.saturationBoost = 1.05f;
            d.prep.workSize = 240;
            d.tiling.cols = 1; d.tiling.rows = 1;
            d.conv.mode = CONV_HYBRID_V5;
            d.conv.totalShapes = 110;
            d.conv.candidates = 140;
            d.conv.mutations = 90;
            d.conv.bgPenalty = 2.4f;
            d.conv.adaptiveAlpha = true;
            d.conv.autoConverge = false;
            d.conv.maxAutoPasses = 1;
            break;
        case 1: // Anime Portrait HD (Recommande)
            d.prep.bgMode = BG_FLOOD;
            d.prep.bgTol = 45.0f;
            d.prep.removeHoles = true;
            d.prep.erodePx = 1;
            d.prep.stripWhiteBorder = false;
            d.prep.posterizeColors = 12;
            d.prep.contrastBoost = 1.12f;
            d.prep.saturationBoost = 1.08f;
            d.prep.workSize = 420;
            d.tiling.cols = 1; d.tiling.rows = 1;
            d.conv.mode = CONV_HYBRID_V5;
            d.conv.totalShapes = 280;
            d.conv.candidates = 420;
            d.conv.mutations = 240;
            d.conv.bgPenalty = 2.6f;
            d.conv.adaptiveAlpha = true;
            d.conv.autoConverge = true;
            d.conv.targetFidelityPct = 90.0f;
            d.conv.maxAutoPasses = 4;
            break;
        case 2: // Sticker PNG / Bord Blanc
            d.prep.bgMode = BG_STICKER_WHITE;
            d.prep.bgTol = 38.0f;
            d.prep.removeHoles = true;
            d.prep.erodePx = 1;
            d.prep.stripWhiteBorder = true;
            d.prep.posterizeColors = 10;
            d.prep.contrastBoost = 1.15f;
            d.prep.saturationBoost = 1.10f;
            d.prep.workSize = 400;
            d.tiling.cols = 1; d.tiling.rows = 1;
            d.conv.mode = CONV_HYBRID_V5;
            d.conv.totalShapes = 220;
            d.conv.candidates = 360;
            d.conv.mutations = 210;
            d.conv.bgPenalty = 2.8f;
            d.conv.adaptiveAlpha = true;
            d.conv.autoConverge = true;
            d.conv.targetFidelityPct = 90.0f;
            d.conv.maxAutoPasses = 3;
            break;
        case 3: // Cel-Shading Net (Triangles + Courbes)
            d.prep.posterizeColors = 6;
            d.prep.contrastBoost = 1.25f;
            d.prep.saturationBoost = 1.15f;
            d.prep.workSize = 380;
            d.tiling.cols = 1; d.tiling.rows = 1;
            d.conv.mode = CONV_TRIANGLES_CURVES;
            d.conv.totalShapes = 240;
            d.conv.candidates = 360;
            d.conv.mutations = 200;
            d.conv.bgPenalty = 2.6f;
            d.conv.adaptiveAlpha = false;
            d.conv.autoConverge = true;
            d.conv.targetFidelityPct = 88.0f;
            d.conv.maxAutoPasses = 3;
            break;
        case 4: // Lineart Manga B&W (Triangles + Unicode)
            d.prep.bgMode = BG_KEY;
            d.prep.bgTol = 42.0f;
            d.prep.posterizeColors = 4;
            d.prep.contrastBoost = 1.45f;
            d.prep.saturationBoost = 0.25f;
            d.prep.workSize = 380;
            d.tiling.cols = 1; d.tiling.rows = 1;
            d.conv.mode = CONV_TRIANGLES_UNICODE;
            d.conv.totalShapes = 150;
            d.conv.flatPassRatio = 0.50f;
            d.conv.lineartThreshold = 0.15f;
            d.conv.candidates = 260;
            d.conv.mutations = 160;
            d.conv.bgPenalty = 3.0f;
            d.conv.adaptiveAlpha = false;
            d.conv.autoConverge = true;
            d.conv.targetFidelityPct = 86.0f;
            d.conv.maxAutoPasses = 3;
            break;
        case 5: // Ultra-Detail Visage (Multi-Tuiles 2x2 + Focus)
            d.prep.posterizeColors = 12;
            d.prep.contrastBoost = 1.12f;
            d.prep.saturationBoost = 1.06f;
            d.prep.workSize = 340;
            d.prep.useFocus = true;
            d.prep.focusBoost = 4.5f;
            d.tiling.cols = 2; d.tiling.rows = 2;
            d.tiling.bleedPx = 4;
            d.tiling.autoBudgetByDensity = true;
            d.conv.mode = CONV_HYBRID_V5;
            d.conv.totalShapes = 290;
            d.conv.candidates = 380;
            d.conv.mutations = 210;
            d.conv.bgPenalty = 2.6f;
            d.conv.adaptiveAlpha = true;
            d.conv.autoConverge = true;
            d.conv.targetFidelityPct = 92.0f;
            d.conv.maxAutoPasses = 4;
            break;
    }
    d.prepDirty = true;
}

static float estimateConversionSeconds(const DecalItem& d, float calibratedSecPerUnit) {
    if (calibratedSecPerUnit <= 0.0f) return -1.0f; // Honest: don't guess until calibrated on this CPU!
    float pxFactor = ((float)d.prep.workSize / 350.0f);
    pxFactor *= pxFactor;
    float workUnits = (float)d.conv.totalShapes * ((float)d.conv.candidates / 300.0f) * ((float)d.conv.mutations / 180.0f) * pxFactor;
    int passes = d.conv.autoConverge ? std::clamp(d.conv.maxAutoPasses, 1, 6) : 1;
    // Later auto-converge passes increase candidates/mutations slightly (~1.35x avg per extra pass)
    float passMul = (passes > 1) ? (1.0f + (passes - 1) * 1.35f) : 1.0f;
    return std::max(2.0f, workUnits * calibratedSecPerUnit * passMul);
}

static void drawGuideWindow(bool& open) {
    if (!open) return;
    ImGui::SetNextWindowSize(ImVec2(760, 620), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Guide d'utilisation — CPM2 Vinyl Studio", &open)) {
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f), "WORKFLOW RECOMMANDE EN 5 ETAPES (RAPIDE & VISUEL)");
        ImGui::Separator();
        ImGui::BulletText("1. Choisissez votre voiture (224 modeles) et votre bodykit dans le panneau de gauche.");
        ImGui::BulletText("2. Cliquez '+ Image (PNG/JPG)' pour importer votre visuel.");
        ImGui::BulletText("3. Reglez le detourage visuellement dans l'apercu (onglets Damier / Sur Peinture / Contours) :");
        ImGui::Indent();
        ImGui::TextWrapped("- Si du fond reste autour du personnage : augmentez 'Tolerance Fond' (45-65) ou essayez 'Sticker Bord Blanc' + 'Peler bord blanc'.\n"
                           "- Pour donner la priorite aux yeux/visage : cochez 'Zone Focus' puis GLISSEZ directement un rectangle a la souris sur l'image !");
        ImGui::Unindent();
        ImGui::BulletText("4. Iterez avec les Presets 1-clic : commencez par 'Test Rapide (5s)' pour verifier le rendu en 3D et ajuster la taille/position avec ImGuizmo (W/E/R, Shift+Clic pour coller sur la carrosserie), puis lancez 'Anime Portrait HD' ou 'Ultra Visage 2x2' !");
        ImGui::BulletText("5. Comparez vos essais dans 'Historique A/B' puis cliquez 'Injecter en Jeu (Atomique)' (sauvegarde ES3 automatique avant ecriture).");

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f), "A QUOI SERVENT LES CURSEURS DE CONVERSION ?");
        ImGui::Separator();
        if (ImGui::BeginTable("SliderGuide", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Curseur", ImGuiTableColumnFlags_WidthFixed, 170.0f);
            ImGui::TableSetupColumn("Role concret", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Reglage conseille", ImGuiTableColumnFlags_WidthFixed, 165.0f);
            ImGui::TableHeadersRow();
            auto row = [](const char* a, const char* b, const char* c) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(a);
                ImGui::TableNextColumn(); ImGui::TextWrapped("%s", b);
                ImGui::TableNextColumn(); ImGui::TextWrapped("%s", c);
            };
            row("Posterisation (teintes)", "Reduit l'image a N couleurs franches (style cel-shading anime). Evite les degrades flous et economise des calques.", "8-12 (Anime HD)\n4-6 (Cel-shading net)");
            row("Contraste / Saturation", "Renforce l'encre noire et fait ressortir les couleurs vives sur une peinture sombre.", "1.10-1.25 / 1.05-1.15");
            row("Resolution / tuile", "Taille de calcul en pixels. Plus haut = traits plus fins mais calcul plus long.", "240 (Test 5s)\n380-440 (Final HQ)");
            row("Budget Formes", "Nombre max de stickers utilises par ce decal. Surveillez les jauges en haut (300 Carrosserie, 150 Vitres).", "110 (Test)\n240-298 (Capot HQ)\n120-148 (Vitre)");
            row("Penalite debordement", "Interdit aux formes de depasser hors de la silhouette sur la peinture. Trop bas = halo parasite ; trop haut = bords ronges.", "2.2 - 3.0 (Standard)\n0.0 si image sans fond");
            row("Candidats / Mutations", "Effort de recherche du moteur par forme. Plus c'est haut, plus chaque forme epouse parfaitement les courbes, mais plus c'est lent.", "140/90 (Rapide)\n400/230 (Final HQ)");
            row("Grille Multi-Tuiles", "Decoupe l'image en M x N carres convertis en parallele. La repartition auto donne plus de formes aux tuiles riches (visage/yeux).", "1x1 (Standard)\n2x2 (Grand portrait HD)");
            row("Seuil Black-Hat", "Sensibilite de detection des traits sombres (visible en direct dans l'onglet 'Contours' en cyan !).", "0.14 - 0.22");
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f), "LIRE LES SCORES EVALSHAPES & LA CARTE D'ERREUR");
        ImGui::Separator();
        ImGui::BulletText("Globale (all) : < 14 = Excellent | 14-22 = Bon | > 22 = Augmentez le budget ou utilisez Engine v5.");
        ImGui::BulletText("Contours (edge) : < 52 = Tres net | > 65 = Augmentez le contraste ou activez le Focus sur le visage.");
        ImGui::BulletText("Debordements (spill) : pixels qui bavent hors silhouette (en MAGENTA sur la Carte d'Erreur) -> augmentez 'Penalite debordement'.");
        ImGui::BulletText("Trous (holes) : pixels non couverts (en ROUGE sur la Carte d'Erreur) -> activez Posterisation 10-12 ou augmentez le Budget.");
    }
    ImGui::End();
}

static void drawQuotaGauge(const char* label, int rawCount, int hardLimit, float barW = 130.0f) {
    float frac = (float)rawCount / (float)std::max(1, hardLimit);
    ImVec4 col = (rawCount > hardLimit) ? ImVec4(0.95f, 0.20f, 0.20f, 1.0f)
               : (frac >= 0.85f)        ? ImVec4(0.96f, 0.65f, 0.14f, 1.0f)
                                        : ImVec4(0.20f, 0.78f, 0.42f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    char overlay[64];
    if (rawCount > hardLimit)
        std::snprintf(overlay, sizeof(overlay), "%s %d/%d (!)", label, rawCount, hardLimit);
    else
        std::snprintf(overlay, sizeof(overlay), "%s %d/%d", label, rawCount, hardLimit);
    ImGui::ProgressBar(std::min(1.0f, frac), ImVec2(barW, 19.0f), overlay);
    ImGui::PopStyleColor();
}

static void applyStudioTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 5.0f;
    s.FrameRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.WindowPadding = ImVec2(8, 8);
    s.FramePadding = ImVec2(6, 4);
    s.ItemSpacing = ImVec2(6, 5);

    auto& c = s.Colors;
    c[ImGuiCol_WindowBg]         = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    c[ImGuiCol_ChildBg]          = ImVec4(0.11f, 0.12f, 0.15f, 1.00f);
    c[ImGuiCol_PopupBg]          = ImVec4(0.11f, 0.12f, 0.15f, 0.96f);
    c[ImGuiCol_Border]           = ImVec4(0.22f, 0.25f, 0.30f, 0.70f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.22f, 0.25f, 0.31f, 1.00f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.26f, 0.31f, 0.38f, 1.00f);
    c[ImGuiCol_TitleBg]          = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    c[ImGuiCol_TitleBgActive]    = ImVec4(0.12f, 0.15f, 0.20f, 1.00f);
    c[ImGuiCol_Header]           = ImVec4(0.18f, 0.34f, 0.56f, 0.65f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.22f, 0.42f, 0.68f, 0.85f);
    c[ImGuiCol_HeaderActive]     = ImVec4(0.26f, 0.48f, 0.78f, 1.00f);
    c[ImGuiCol_Button]           = ImVec4(0.18f, 0.22f, 0.28f, 1.00f);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.24f, 0.36f, 0.54f, 1.00f);
    c[ImGuiCol_ButtonActive]     = ImVec4(0.20f, 0.46f, 0.76f, 1.00f);
    c[ImGuiCol_Tab]              = ImVec4(0.12f, 0.14f, 0.18f, 1.00f);
    c[ImGuiCol_TabHovered]       = ImVec4(0.24f, 0.42f, 0.68f, 0.90f);
    c[ImGuiCol_TabSelected]      = ImVec4(0.20f, 0.38f, 0.62f, 1.00f);
    c[ImGuiCol_SliderGrab]       = ImVec4(0.26f, 0.56f, 0.90f, 1.00f);
    c[ImGuiCol_CheckMark]        = ImVec4(0.32f, 0.72f, 1.00f, 1.00f);
}

static std::string detectProjectRoot() {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    fs::path exeDir = fs::path(exePath).parent_path();
    fs::path p = exeDir;
    for (int i = 0; i < 5; ++i) {
        // Supports BOTH portable dist layout (sprites_v4 + tools) and dev layout (studio/build/ -> root)
        if ((fs::exists(p / "sprites_v4") && fs::exists(p / "bin")) ||
            (fs::exists(p / "tools" / "export_car_3d.py"))) {
            return p.string();
        }
        if (p == p.parent_path()) break;
        p = p.parent_path();
    }
    return exeDir.string();
}

// ================================ MAIN ENTRY ================================

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int) {
    SetProcessDPIAware();
    std::string cmdLineStr = lpCmdLine ? lpCmdLine : "";
    std::string autoShotPath;
    size_t shotPos = cmdLineStr.find("--screenshot=");
    if (shotPos != std::string::npos) {
        autoShotPath = cmdLineStr.substr(shotPos + 13);
        while (!autoShotPath.empty() && (autoShotPath.back() == ' ' || autoShotPath.back() == '"')) autoShotPath.pop_back();
        if (!autoShotPath.empty() && autoShotPath.front() == '"') autoShotPath.erase(0, 1);
    }

    WNDCLASSEXW wc{sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0, 0, hInst, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"CPM2VinylStudioClass", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"CPM2 Vinyl Studio — Editeur 3D GPU & Engine v5",
                              WS_OVERLAPPEDWINDOW, 60, 40, 1600, 960, nullptr, nullptr, hInst, nullptr);

    StudioApp app;
    g_app = &app;
    app.rootDir = detectProjectRoot();

    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (!app.renderer.init(hwnd, rc.right - rc.left, rc.bottom - rc.top)) {
        MessageBoxA(hwnd, "Echec initialisation DirectX 11.", "CPM2 Vinyl Studio", MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.FontGlobalScale = 1.0f;

    // Load clean Segoe UI font with French accents + Unicode symbols
    static const ImWchar glyphRanges[] = {0x0020, 0x00FF, 0x2010, 0x203A, 0x2500, 0x257F, 0};
    if (fs::exists("C:\\Windows\\Fonts\\segoeui.ttf")) {
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f, nullptr, glyphRanges);
    }
    applyStudioTheme();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(app.renderer.device(), app.renderer.context());

    // Load portable config (auto-detects CPM2 game folder on any PC/drive via Steam registry!)
    loadStudioConfig(app.rootDir, app.config);
    cleanupStaleTempDlls((fs::path(app.rootDir) / "work").string());

    // Load 20 high-res 256x256 sprites into GPU Texture2DArray
    app.catalog.load(app.resolveSpritesDir());
    app.renderer.uploadSpriteCatalog(app.catalog);
    app.loadCatalogCarIds();
    app.refreshGarageOffline();

    // Clean startup (zero personal demo data!): load vehicle matching selectedInstanceId, else first owned garage car, else 187
    int startCarId = 187;
    for (const auto& g : app.garageInstances) {
        if (g.instanceId == app.selectedInstanceId) { startCarId = g.carId; break; }
    }
    if (startCarId == 187 && !app.garageInstances.empty() && app.selectedInstanceId.empty()) {
        startCarId = app.garageInstances[0].carId;
    }
    if (!app.loadCarById(startCarId) && !app.availableCarIds.empty()) {
        app.loadCarById(app.availableCarIds[0]);
    }

    bool done = false;
    int frameCount = 0;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (app.pendingW > 0 && app.pendingH > 0) {
            app.renderer.resizeSwapChain(app.pendingW, app.pendingH);
            app.pendingW = app.pendingH = 0;
        }

        // Poll game process status every 1.5s (lightweight)
        double nowT = ImGui::GetTime();
        if (nowT - app.lastPidCheckTime > 1.5) {
            app.gamePid = findGameProcessId();
            app.lastPidCheckTime = nowT;
        }

        // Poll live post-injection / scan_garage confirmation from CPM2_Vinyls_Injector.dll
        if (app.awaitingInjectorResult) {
            std::string wd = (fs::path(app.rootDir) / "work").string();
            InjectionReport rep;
            if (readInjectionReport(wd, rep)) {
                app.lastInjReport = rep;
                app.awaitingInjectorResult = false;
                app.statusBanner = rep.summaryText;
                app.refreshGarageOffline();
                app.loadLiveCaptureTextures();
                if (rep.mode == "scan_garage" && rep.carId >= 0 && rep.carId != app.project.carId) {
                    app.selectedInstanceId = rep.carInstance;
                    app.loadCarById(rep.carId);
                }
            } else if (nowT - app.injectorStartTime > 20.0) {
                app.awaitingInjectorResult = false;
                app.lastInjReport = InjectionReport{};
                app.lastInjReport.hasReport = true;
                app.lastInjReport.success = false;
                app.lastInjReport.summaryText = "Timeout (20s) sans reponse du jeu. Verifiez que CPM 2 n'est pas fige ou sur un ecran de chargement.";
                app.statusBanner = app.lastInjReport.summaryText;
            }
        }

        // Poll async Auto-Converge worker (live intermediate pass updates + final completion + CPU speed calibration!)
        if (app.worker.isRunning()) {
            app.statusBanner = app.worker.statusText();
        }
        int doneIdx = -1;
        if (app.worker.targetDecalIndex() >= 0 && app.worker.targetDecalIndex() < (int)app.project.decals.size()) {
            auto& targetD = app.project.decals[(size_t)app.worker.targetDecalIndex()];
            if (app.worker.pollIntermediate(targetD, doneIdx)) {
                app.refreshDecalPreviews(doneIdx);
                app.liveryDirty = true;
            }
            if (app.worker.pollCompleted(targetD, doneIdx)) {
                app.refreshDecalPreviews(doneIdx);
                app.liveryDirty = true;
                app.statusBanner = app.worker.statusText();
                float spu = app.worker.lastRunSecPerUnit();
                if (spu > 0.0001f) {
                    app.config.calibratedSecPerUnit = (app.config.calibratedSecPerUnit > 0.0f)
                        ? (app.config.calibratedSecPerUnit * 0.5f + spu * 0.5f) : spu;
                    saveStudioConfig(app.rootDir, app.config);
                }
            }
        }

        if (app.liveryDirty) {
            app.rebuildProjectionAndComposite();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();

        // ==================== FULLSCREEN DOCKSPACE + TOP TOOLBAR ====================
        ImGuiViewport* mainVp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(mainVp->WorkPos);
        ImGui::SetNextWindowSize(mainVp->WorkSize);
        ImGui::SetNextWindowViewport(mainVp->ID);
        ImGuiWindowFlags hostFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
        ImGui::Begin("CPM2StudioHost", nullptr, hostFlags);
        ImGui::PopStyleVar(3);

        // ---- Menu Bar ----
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("Projet")) {
                if (ImGui::MenuItem("Ouvrir projet (.cpmproj.json)...")) {
                    auto p = openFileDialog(hwnd, "CPM2 Project (*.json)\0*.json\0All (*.*)\0*.*\0", (fs::path(app.rootDir) / "work").string().c_str());
                    if (!p.empty() && loadProjectJson(p, app.project)) {
                        app.activeProjectPath = p;
                        auto savedKits = app.project.kitVariants;
                        app.loadCarById(app.project.carId);
                        if (savedKits.size() == app.project.kitVariants.size()) {
                            app.project.kitVariants = savedKits;
                            app.car.rebuildBVH(app.project.kitVariants);
                        }
                        for (int i = 0; i < (int)app.project.decals.size(); ++i) {
                            if (!app.project.decals[(size_t)i].imagePath.empty()) {
                                loadDecalImage(app.project.decals[(size_t)i].imagePath, app.project.decals[(size_t)i]);
                                app.refreshDecalPreviews(i);
                            }
                        }
                        app.selectedDecal = app.project.decals.empty() ? -1 : 0;
                        app.liveryDirty = true;
                        app.statusBanner = "Projet charge : " + p;
                    }
                }
                if (ImGui::MenuItem("Sauvegarder projet (.cpmproj.json)...")) {
                    auto p = saveFileDialog(hwnd, "CPM2 Project (*.cpmproj.json)\0*.cpmproj.json\0", "cpmproj.json", (fs::path(app.rootDir) / "work").string().c_str());
                    if (!p.empty() && saveProjectJson(p, app.project)) {
                        app.activeProjectPath = p;
                        app.statusBanner = "Projet sauvegarde : " + p;
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Importer calques existants (Carrosserie *_layers.txt)...")) {
                    auto p = openFileDialog(hwnd, "Layers TXT (*.txt)\0*.txt\0", (fs::path(app.rootDir) / "work").string().c_str());
                    if (!p.empty() && loadLayersTxt(p, app.project.importedBody)) {
                        app.liveryDirty = true;
                        app.statusBanner = "Importe " + std::to_string(app.project.importedBody.size()) + " calques carrosserie";
                    }
                }
                if (ImGui::MenuItem("Importer calques existants (Vitres *_layers.txt)...")) {
                    auto p = openFileDialog(hwnd, "Layers TXT (*.txt)\0*.txt\0", (fs::path(app.rootDir) / "work").string().c_str());
                    if (!p.empty() && loadLayersTxt(p, app.project.importedWindows)) {
                        app.liveryDirty = true;
                        app.statusBanner = "Importe " + std::to_string(app.project.importedWindows.size()) + " calques vitres";
                    }
                }
                if (ImGui::MenuItem("Vider calques importes")) {
                    app.project.importedBody.clear();
                    app.project.importedWindows.clear();
                    app.project.importedPlate.clear();
                    app.liveryDirty = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Choisir dossier du jeu CPM 2 (Car Parking 2.exe)...")) {
                    auto p = openFileDialog(hwnd, "Car Parking 2.exe\0Car Parking 2.exe\0Executable (*.exe)\0*.exe\0");
                    if (!p.empty()) {
                        std::string dir = fs::path(p).parent_path().string();
                        if (isValidGameDir(dir)) {
                            app.config.gameDir = dir;
                            saveStudioConfig(app.rootDir, app.config);
                            app.loadCatalogCarIds();
                            app.statusBanner = "Dossier du jeu CPM 2 configure : " + dir;
                        } else {
                            app.statusBanner = "Dossier invalide (StreamingAssets\\aa\\StandaloneWindows64 introuvable).";
                        }
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Quitter")) done = true;
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Affichage")) {
                ImGui::RadioButton("Vue 3D uniquement", &app.viewLayout, 0);
                ImGui::RadioButton("Vue UV / Planaire uniquement", &app.viewLayout, 1);
                ImGui::RadioButton("Split 3D + Vue UV", &app.viewLayout, 2);
                ImGui::Separator();
                ImGui::MenuItem("Heatmap Deformation UV", nullptr, &app.vpOpts.showUVHeatmap);
                ImGui::MenuItem("Wireframe 3D", nullptr, &app.vpOpts.showWireframe);
                ImGui::MenuItem("Afficher Vitres", nullptr, &app.vpOpts.showWindows);
                ImGui::MenuItem("Afficher Habitacle / Chassis", nullptr, &app.vpOpts.showInteriorStatic);
                ImGui::MenuItem("Afficher Roues", nullptr, &app.vpOpts.showWheels);
                ImGui::MenuItem("Boites Projecteurs & Grille Tuiles", nullptr, &app.vpOpts.showDecalBoxes);
                ImGui::EndMenu();
            }

            // Right-aligned live server quota safety gauges right inside the top bar!
            ImGui::Separator();
            drawQuotaGauge("Carrosserie", app.livery.rawBodyCount, kQuotaBody, 165.0f);
            ImGui::SameLine();
            drawQuotaGauge("Vitres", app.livery.rawWindowCount, kQuotaWindows, 130.0f);
            ImGui::SameLine();
            drawQuotaGauge("Plaques", app.livery.rawPlateCount, kQuotaPlate, 125.0f);
            ImGui::EndMenuBar();
        }

        // ---- Quick Action Toolbar ----
        {
            ImGui::BeginChild("TopToolbar", ImVec2(0, 38), true, ImGuiWindowFlags_NoScrollbar);
            // Car Selector (all 224 vehicles!)
            ImGui::SetNextItemWidth(115);
            std::string curCarLabel = "car" + std::to_string(app.project.carId);
            if (ImGui::BeginCombo("##CarSel", curCarLabel.c_str())) {
                for (int cid : app.availableCarIds) {
                    std::string lbl = "car" + std::to_string(cid);
                    bool sel = (cid == app.project.carId);
                    if (ImGui::Selectable(lbl.c_str(), sel)) {
                        app.loadCarById(cid);
                        app.selectedInstanceId.clear();
                        for (const auto& g : app.garageInstances) {
                            if (g.carId == cid) { app.selectedInstanceId = g.instanceId; break; }
                        }
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();

            // Gizmo tool buttons
            if (ImGui::RadioButton("Depl. (W)", app.gizmoOp == ImGuizmo::TRANSLATE)) app.gizmoOp = ImGuizmo::TRANSLATE;
            ImGui::SameLine();
            if (ImGui::RadioButton("Rot. (E)", app.gizmoOp == ImGuizmo::ROTATE)) app.gizmoOp = ImGuizmo::ROTATE;
            ImGui::SameLine();
            if (ImGui::RadioButton("Ech. (R)", app.gizmoOp == ImGuizmo::SCALE)) app.gizmoOp = ImGuizmo::SCALE;
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();

            // View layout quick toggle
            if (ImGui::RadioButton("3D", app.viewLayout == 0)) app.viewLayout = 0;
            ImGui::SameLine();
            if (ImGui::RadioButton("UV", app.viewLayout == 1)) app.viewLayout = 1;
            ImGui::SameLine();
            if (ImGui::RadioButton("Split 3D+UV", app.viewLayout == 2)) app.viewLayout = 2;
            ImGui::SameLine();
            ImGui::Checkbox("Heatmap UV", &app.vpOpts.showUVHeatmap);
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();

            // Export & Inject buttons
            if (ImGui::Button("Exporter Calques (.txt)")) {
                app.exportAndInject(false, false);
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.52f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.65f, 0.38f, 1.0f));
            if (ImGui::Button("Injecter en Jeu (Atomique)")) {
                app.exportAndInject(true, true);
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.38f, 0.62f, 1.0f));
            if (ImGui::Button("Guide (?)")) {
                app.showGuideWindow = !app.showGuideWindow;
            }
            ImGui::PopStyleColor();
            ImGui::EndChild();
        }

        // ==================== MAIN 3-COLUMN WORKSPACE ====================
        float availH = ImGui::GetContentRegionAvail().y - 26.0f;
        float leftW = 285.0f;
        float rightW = 420.0f;
        float centerW = std::max(320.0f, ImGui::GetContentRegionAvail().x - leftW - rightW - 14.0f);

        // ---------------- LEFT PANEL: Live Garage Sync, Vehicle, Bodykits, Decals & Texts ----------------
        ImGui::BeginChild("LeftPanel", ImVec2(leftW, availH), true);

        if (ImGui::CollapsingHeader("Garage & Connexion Jeu (Temps Reel)", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!isValidGameDir(app.config.gameDir)) {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.25f, 1.0f), "Jeu CPM 2 non trouve !");
                ImGui::TextWrapped("Menu Projet -> Choisir dossier du jeu CPM 2.");
            }
            if (app.gamePid != 0) {
                ImGui::TextColored(ImVec4(0.28f, 0.90f, 0.45f, 1.0f), "Jeu EN LIGNE (PID %u)", app.gamePid);
            } else {
                ImGui::TextDisabled("Jeu hors ligne (lancez CPM 2 pour injecter)");
            }

            if (ImGui::Button("Actu. Garage", ImVec2(125, 0))) {
                app.refreshGarageOffline();
                app.statusBanner = "Garage ES3 scanne : " + std::to_string(app.garageInstances.size()) + " vehicule(s) possede(s).";
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(app.gamePid == 0 || app.awaitingInjectorResult);
            if (ImGui::Button("Sync Jeu Live", ImVec2(-1, 0))) {
                app.triggerLiveGarageScan();
            }
            ImGui::EndDisabled();

            // Unique CarInstanceId selector (handles duplicate models in the same garage!)
            if (!app.garageInstances.empty()) {
                ImGui::TextDisabled("Instance cible (gere les doublons) :");
                std::string curInstLbl = app.selectedInstanceId.empty() ? "(Auto: actif en jeu)" : app.selectedInstanceId;
                for (const auto& g : app.garageInstances) {
                    if (g.instanceId == app.selectedInstanceId) {
                        curInstLbl = "car" + std::to_string(g.carId) + " [" + g.shortLabel + "]" + (g.isActiveInGame ? " *ACTIF*" : "");
                        break;
                    }
                }
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##GarageInst", curInstLbl.c_str())) {
                    if (ImGui::Selectable("(Auto : vehicule actif en jeu)", app.selectedInstanceId.empty())) {
                        app.selectedInstanceId.clear();
                    }
                    for (const auto& g : app.garageInstances) {
                        std::string itemLbl = "car" + std::to_string(g.carId) + " [" + g.shortLabel + "]";
                        if (g.isActiveInGame) itemLbl += " *ACTIF*";
                        if (g.hasBodyVinyls || g.hasWindowVinyls) {
                            itemLbl += " (";
                            if (g.hasBodyVinyls) itemLbl += "B";
                            if (g.hasWindowVinyls) itemLbl += "W";
                            itemLbl += ")";
                        }
                        bool sel = (g.instanceId == app.selectedInstanceId);
                        if (ImGui::Selectable(itemLbl.c_str(), sel)) {
                            app.selectedInstanceId = g.instanceId;
                            if (g.carId != app.project.carId) app.loadCarById(g.carId);
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            if (app.awaitingInjectorResult) {
                ImGui::TextColored(ImVec4(0.98f, 0.80f, 0.25f, 1.0f), "Attente reponse du jeu...");
            } else if (app.lastInjReport.hasReport) {
                ImVec4 rc = app.lastInjReport.success ? ImVec4(0.28f, 0.90f, 0.45f, 1.0f) : ImVec4(0.96f, 0.30f, 0.28f, 1.0f);
                ImGui::TextColored(rc, "%s", app.lastInjReport.success ? "Derniere injection : OK" : "Derniere injection : ECHEC");
                ImGui::TextWrapped("%s", app.lastInjReport.summaryText.c_str());
                if (app.lastInjReport.hasLiveBodyCapture || app.lastInjReport.hasLiveWinCapture) {
                    if (ImGui::Button("Voir Rendu Jeu Live (GPU)", ImVec2(-1, 0))) {
                        app.showLiveGameCaptureWindow = true;
                    }
                }
            }
        }

        if (ImGui::CollapsingHeader("Vehicule & Bodykit", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::ColorEdit3("Peinture", app.project.paintColor)) {
                if (app.selectedDecal >= 0) app.refreshDecalPreviews(app.selectedDecal);
            }
            ImGui::SliderFloat("Metallique", &app.project.paintMetallic, 0.0f, 1.0f);
            ImGui::SliderFloat("Rugosite", &app.project.paintRoughness, 0.04f, 0.85f);
            ImGui::ColorEdit4("Teinte Vitres", app.project.windowTint);

            if (!app.car.kitGroups.empty()) {
                ImGui::Separator();
                ImGui::Text("Variantes Bodykit (car%d) :", app.project.carId);
                for (size_t g = 0; g < app.car.kitGroups.size(); ++g) {
                    int v = (g < app.project.kitVariants.size()) ? app.project.kitVariants[g] : 0;
                    int maxV = std::max(0, (int)app.car.kitGroups[g].variantCount - 1);
                    if (ImGui::SliderInt(app.car.kitGroups[g].name.c_str(), &v, 0, maxV)) {
                        if (g < app.project.kitVariants.size()) {
                            app.project.kitVariants[g] = v;
                            app.car.rebuildBVH(app.project.kitVariants);
                            app.liveryDirty = true;
                        }
                    }
                }
            }
        }

        if (ImGui::CollapsingHeader("Decals 3D (Images & Calques)", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Button("+ Image (PNG/JPG)", ImVec2(-1, 0))) {
                auto p = openFileDialog(hwnd, "Images (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0All (*.*)\0*.*\0");
                if (!p.empty()) {
                    DecalItem d;
                    d.name = fs::path(p).stem().string();
                    if (loadDecalImage(p, d)) {
                        app.project.decals.push_back(std::move(d));
                        app.selectedDecal = (int)app.project.decals.size() - 1;
                        app.refreshDecalPreviews(app.selectedDecal);
                        app.statusBanner = "Image chargee : " + p + " — cliquez 'Convertir' dans l'inspecteur.";
                    }
                }
            }
            if (ImGui::Button("+ Formes JSON existantes", ImVec2(-1, 0))) {
                auto p = openFileDialog(hwnd, "Shapes JSON (*.json)\0*.json\0", (fs::path(app.rootDir) / "work").string().c_str());
                if (!p.empty()) {
                    DecalItem d;
                    d.name = fs::path(p).stem().string();
                    if (loadShapesJson(p, d)) {
                        app.project.decals.push_back(std::move(d));
                        app.selectedDecal = (int)app.project.decals.size() - 1;
                        app.liveryDirty = true;
                    }
                }
            }

            ImGui::Separator();
            for (int i = 0; i < (int)app.project.decals.size(); ++i) {
                auto& d = app.project.decals[(size_t)i];
                ImGui::PushID(i);
                if (ImGui::Checkbox("##en", &d.enabled)) app.liveryDirty = true;
                ImGui::SameLine();
                char lbl[128];
                std::snprintf(lbl, sizeof(lbl), "%s [%dB/%dW]", d.name.c_str(), d.projectedBodyCount, d.projectedWindowCount);
                if (ImGui::Selectable(lbl, app.selectedDecal == i)) {
                    app.selectedDecal = i;
                    app.refreshDecalPreviews(i);
                }
                ImGui::PopID();
            }

            if (app.selectedDecal >= 0 && app.selectedDecal < (int)app.project.decals.size()) {
                if (ImGui::Button("Dupliquer")) {
                    DecalItem copy = app.project.decals[(size_t)app.selectedDecal];
                    copy.name += "_copy";
                    copy.xform.pos.x += 0.15f;
                    app.project.decals.push_back(std::move(copy));
                    app.selectedDecal = (int)app.project.decals.size() - 1;
                    app.liveryDirty = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Supprimer")) {
                    app.project.decals.erase(app.project.decals.begin() + app.selectedDecal);
                    app.selectedDecal = std::min(app.selectedDecal, (int)app.project.decals.size() - 1);
                    app.liveryDirty = true;
                }
            }
        }

        if (ImGui::CollapsingHeader("Calques Texte Directs (Unicode / JDM)")) {
            if (ImGui::Button("+ Ajouter Texte", ImVec2(-1, 0))) {
                DirectTextItem t;
                t.name = "Texte_" + std::to_string(app.project.texts.size() + 1);
                app.project.texts.push_back(t);
                app.selectedText = (int)app.project.texts.size() - 1;
                app.liveryDirty = true;
            }
            for (int i = 0; i < (int)app.project.texts.size(); ++i) {
                auto& t = app.project.texts[(size_t)i];
                ImGui::PushID(1000 + i);
                if (ImGui::Checkbox("##ten", &t.enabled)) app.liveryDirty = true;
                ImGui::SameLine();
                if (ImGui::Selectable((t.name + " : " + t.text).c_str(), app.selectedText == i)) {
                    app.selectedText = i;
                }
                ImGui::PopID();
            }
            if (app.selectedText >= 0 && app.selectedText < (int)app.project.texts.size()) {
                auto& t = app.project.texts[(size_t)app.selectedText];
                char buf[128] = {};
                std::strncpy(buf, t.text.c_str(), sizeof(buf) - 1);
                if (ImGui::InputText("Contenu", buf, sizeof(buf))) { t.text = buf; app.liveryDirty = true; }
                const char* surfs[] = {"Carrosserie", "Vitres", "Plaque"};
                if (ImGui::Combo("Surface", &t.surface, surfs, 3)) app.liveryDirty = true;
                if (ImGui::SliderInt("Police (0=Unicode)", &t.fontIndex, 0, 18)) app.liveryDirty = true;
                if (ImGui::DragFloat2("UV Centre", t.uvCenter, 0.002f, 0.0f, 1.0f)) app.liveryDirty = true;
                if (ImGui::DragFloat2("Echelle", t.scale, 0.05f, 0.1f, 20.0f)) app.liveryDirty = true;
                if (ImGui::SliderFloat("Rotation", &t.rotation, 0.0f, 360.0f)) app.liveryDirty = true;
                if (ImGui::ColorEdit4("Couleur", t.color)) app.liveryDirty = true;
                if (ImGui::Checkbox("Gras", &t.bold)) app.liveryDirty = true;
                ImGui::SameLine();
                if (ImGui::Checkbox("Miroir X", &t.flipX)) app.liveryDirty = true;
                if (ImGui::Button("Supprimer Texte")) {
                    app.project.texts.erase(app.project.texts.begin() + app.selectedText);
                    app.selectedText = -1;
                    app.liveryDirty = true;
                }
            }
        }
        ImGui::EndChild();

        // ---------------- CENTER PANEL: 3D GPU Viewport + 2D UV Viewport ----------------
        ImGui::SameLine();
        ImGui::BeginChild("CenterPanel", ImVec2(centerW, availH), false);

        // Quick camera presets bar
        {
            if (ImGui::SmallButton("3/4"))     { app.camera.yaw = 0.65f; app.camera.pitch = 0.30f; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Capot"))   { app.camera.yaw = 0.0f;  app.camera.pitch = 1.25f; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Avant"))   { app.camera.yaw = 0.0f;  app.camera.pitch = 0.18f; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Arriere")) { app.camera.yaw = kPi;   app.camera.pitch = 0.18f; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Gauche"))  { app.camera.yaw = -kPi * 0.5f; app.camera.pitch = 0.14f; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Droite"))  { app.camera.yaw = +kPi * 0.5f; app.camera.pitch = 0.14f; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Toit"))    { app.camera.yaw = 0.0f;  app.camera.pitch = 1.50f; }
            ImGui::SameLine();
            ImGui::TextDisabled(" | Clic-gauche: Orbite | Molette: Zoom | Shift+Clic: Snap Decal sur 3D");
        }

        ImVec2 vpAvail = ImGui::GetContentRegionAvail();
        bool show3D = (app.viewLayout == 0 || app.viewLayout == 2);
        bool showUV = (app.viewLayout == 1 || app.viewLayout == 2);
        float w3D = (app.viewLayout == 2) ? (vpAvail.x * 0.62f - 4.0f) : vpAvail.x;
        float wUV = (app.viewLayout == 2) ? (vpAvail.x * 0.38f - 4.0f) : vpAvail.x;

        // ---- 3D GPU Viewport ----
        if (show3D) {
            ImGui::BeginChild("VP3D", ImVec2(w3D, vpAvail.y), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImVec2 sz = ImGui::GetContentRegionAvail();
            int rw = std::max(64, (int)sz.x);
            int rh = std::max(64, (int)sz.y);

            app.renderer.render3DViewport(rw, rh, app.camera, app.car, app.project, app.selectedDecal, app.vpOpts);

            ImVec2 imgPos = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)app.renderer.viewport3DSRV(), ImVec2((float)rw, (float)rh));
            bool hovered = ImGui::IsItemHovered();

            // ImGuizmo 3D manipulation on selected decal!
            bool gizmoUsing = false;
            if (app.selectedDecal >= 0 && app.selectedDecal < (int)app.project.decals.size()) {
                auto& d = app.project.decals[(size_t)app.selectedDecal];
                if (d.enabled && d.xform.projectionMode == 0) {
                    ImGuizmo::SetOrthographic(false);
                    ImGuizmo::SetDrawlist();
                    ImGuizmo::SetRect(imgPos.x, imgPos.y, (float)rw, (float)rh);

                    float aspect = (float)rw / (float)rh;
                    Mat4 viewM = app.camera.viewMatrix();
                    Mat4 projM = app.camera.projMatrix(aspect);
                    Mat4 modelM = d.xform.matrix();

                    if (ImGuizmo::Manipulate(viewM.m, projM.m, app.gizmoOp, app.gizmoMode, modelM.m)) {
                        float tr[3], rt[3], sc[3];
                        ImGuizmo::DecomposeMatrixToComponents(modelM.m, tr, rt, sc);
                        d.xform.pos = {tr[0], tr[1], tr[2]};
                        d.xform.rotEuler = {rt[0], rt[1], rt[2]};
                        if (d.xform.lockAspect && d.srcH > 0) {
                            float ratio = (float)d.srcW / (float)d.srcH;
                            if (std::fabs(sc[0] - d.xform.scale.x) > std::fabs(sc[1] - d.xform.scale.y)) {
                                d.xform.scale.x = std::max(0.05f, sc[0]);
                                d.xform.scale.y = d.xform.scale.x / ratio;
                            } else {
                                d.xform.scale.y = std::max(0.05f, sc[1]);
                                d.xform.scale.x = d.xform.scale.y * ratio;
                            }
                        } else {
                            d.xform.scale.x = std::max(0.05f, sc[0]);
                            d.xform.scale.y = std::max(0.05f, sc[1]);
                        }
                        d.xform.scale.z = std::max(0.08f, sc[2]);
                        app.liveryDirty = true;
                    }
                    gizmoUsing = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
                }
            }

            // Camera orbit/pan/zoom + Shift+Click Snap-to-Surface 3D!
            if (hovered && !gizmoUsing) {
                if (io.MouseWheel != 0.0f) {
                    app.camera.distance = std::clamp(app.camera.distance * (1.0f - io.MouseWheel * 0.12f), 1.0f, 14.0f);
                }
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyShift && app.selectedDecal >= 0) {
                    float nx = ((io.MousePos.x - imgPos.x) / (float)rw) * 2.0f - 1.0f;
                    float ny = 1.0f - ((io.MousePos.y - imgPos.y) / (float)rh) * 2.0f;
                    Ray r = app.camera.screenPointToRay(nx, ny, (float)rw / (float)rh);
                    RayHit hit;
                    if (app.car.raycast(r, 30.0f, MASK_BODY | MASK_WINDOWS | MASK_PLATE, hit)) {
                        snapDecalToHit(app.project.decals[(size_t)app.selectedDecal], hit);
                        app.liveryDirty = true;
                        app.statusBanner = std::string("Decal aligne sur surface 3D (") +
                                           (hit.surface == SURF_WINDOWS ? "Vitre" : hit.surface == SURF_PLATE ? "Plaque" : "Carrosserie") + ")";
                    }
                } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    app.orbiting = true;
                    app.lastMouse = io.MousePos;
                }
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    app.panning = true;
                    app.lastMouse = io.MousePos;
                }
            }
            if (app.orbiting) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    float dx = io.MousePos.x - app.lastMouse.x;
                    float dy = io.MousePos.y - app.lastMouse.y;
                    app.lastMouse = io.MousePos;
                    app.camera.yaw += dx * 0.007f;
                    app.camera.pitch = std::clamp(app.camera.pitch + dy * 0.007f, -0.35f, 1.52f);
                } else app.orbiting = false;
            }
            if (app.panning) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                    float dx = io.MousePos.x - app.lastMouse.x;
                    float dy = io.MousePos.y - app.lastMouse.y;
                    app.lastMouse = io.MousePos;
                    Mat4 vm = app.camera.viewMatrix();
                    Vec3 right(vm(0, 0), vm(0, 1), vm(0, 2));
                    Vec3 up(vm(1, 0), vm(1, 1), vm(1, 2));
                    float spd = app.camera.distance * 0.0018f;
                    app.camera.target += right * (-dx * spd) + up * (dy * spd);
                } else app.panning = false;
            }
            // Keyboard gizmo shortcuts when not typing
            if (!io.WantTextInput) {
                if (ImGui::IsKeyPressed(ImGuiKey_W)) app.gizmoOp = ImGuizmo::TRANSLATE;
                if (ImGui::IsKeyPressed(ImGuiKey_E)) app.gizmoOp = ImGuizmo::ROTATE;
                if (ImGui::IsKeyPressed(ImGuiKey_R)) app.gizmoOp = ImGuizmo::SCALE;
            }
            ImGui::EndChild();
        }

        // ---- 2D UV / Planar Projection Viewport ----
        if (showUV) {
            if (show3D) ImGui::SameLine();
            ImGui::BeginChild("VPUV", ImVec2(wUV, vpAvail.y), true, ImGuiWindowFlags_NoScrollbar);
            const char* uvSurfs[] = {"Atlas Carrosserie (3000²)", "Atlas Vitres (3000²)", "Atlas Plaque (2:1)"};
            ImGui::SetNextItemWidth(195);
            ImGui::Combo("##UVSurf", &app.vpOpts.uvViewSurface, uvSurfs, 3);
            ImGui::SameLine();
            ImGui::Checkbox("Wireframe UV", &app.vpOpts.uvShowWireframe);

            ImVec2 sz = ImGui::GetContentRegionAvail();
            int side = std::max(64, (int)std::min(sz.x, sz.y));
            int uvw = (app.vpOpts.uvViewSurface == SURF_PLATE) ? std::max(64, (int)sz.x) : side;
            int uvh = (app.vpOpts.uvViewSurface == SURF_PLATE) ? std::max(64, uvw / 2) : side;
            app.renderer.renderUVViewport(uvw, uvh, app.car, app.project, app.vpOpts);
            ImGui::Image((ImTextureID)app.renderer.viewportUVSRV(), ImVec2((float)uvw, (float)uvh));
            ImGui::EndChild();
        }
        ImGui::EndChild();

        // ---------------- RIGHT PANEL: Decal Inspector, Tiling & Conversion Engine ----------------
        ImGui::SameLine();
        ImGui::BeginChild("RightPanel", ImVec2(rightW, availH), true);
        ImGui::PushItemWidth(195.0f);

        if (app.selectedDecal >= 0 && app.selectedDecal < (int)app.project.decals.size()) {
            auto& d = app.project.decals[(size_t)app.selectedDecal];
            char nbuf[96] = {};
            std::strncpy(nbuf, d.name.c_str(), sizeof(nbuf) - 1);
            if (ImGui::InputText("Nom", nbuf, sizeof(nbuf))) d.name = nbuf;

            // --- 0. Presets 1-clic & Apercu Visuel Interactif ---
            if (ImGui::CollapsingHeader("Presets Rapides (1 clic) & Apercu Visuel", ImGuiTreeNodeFlags_DefaultOpen)) {
                float bw = (ImGui::GetContentRegionAvail().x - 8.0f) * 0.5f;
                auto presetBtn = [&](int id, const char* lbl, const char* tip, ImVec4 col) {
                    ImGui::PushStyleColor(ImGuiCol_Button, col);
                    if (ImGui::Button(lbl, ImVec2(bw, 26))) {
                        applyPreset(d, id);
                        app.refreshDecalPreviews(app.selectedDecal);
                        app.statusBanner = std::string("Preset applique : ") + lbl;
                    }
                    ImGui::PopStyleColor();
                    if (ImGui::BeginItemTooltip()) {
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
                        ImGui::TextUnformatted(tip);
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                };
                presetBtn(0, "Test Rapide (5s)", "240px, 110 formes, 140 cand. Ideal pour tester votre placement 3D et votre detourage en quelques secondes.", ImVec4(0.20f, 0.44f, 0.36f, 1.0f));
                ImGui::SameLine();
                presetBtn(1, "Anime Portrait HD", "Recommande : Engine v5 Hybride, 280 formes, Posterisation 12 teintes, detourage Flood+Hysteresie. Meilleure fidelite globale.", ImVec4(0.18f, 0.40f, 0.68f, 1.0f));
                presetBtn(2, "Sticker Bord Blanc", "Pour stickers anime die-cut a contour blanc : pele la bordure blanche jusqu'au trait d'encre noir + Engine v5 (220 formes).", ImVec4(0.26f, 0.34f, 0.52f, 1.0f));
                ImGui::SameLine();
                presetBtn(3, "Cel-Shading (Triangles)", "Aplats tres nets a 6 teintes : Double Passe Triangles (95) + Lineart Courbes natives (240 formes).", ImVec4(0.46f, 0.34f, 0.18f, 1.0f));
                presetBtn(4, "Lineart Manga Unicode", "Pour vitres ou mangas N&B : Chromakey + Contraste fort + Triangles (95) + Contours en caracteres Unicode (150 formes).", ImVec4(0.40f, 0.24f, 0.48f, 1.0f));
                ImGui::SameLine();
                presetBtn(5, "Ultra Visage (2x2)", "Multi-Tuiles 2x2 avec repartition auto par densite + Zone Focus Visage/Yeux (Boost 4.5x), 290 formes.", ImVec4(0.54f, 0.24f, 0.24f, 1.0f));

                // Interactive Visual Diagnostic Canvas (Damier / Sur Peinture / Contours / Resultat / Erreurs)
                if (!d.origRGBA.empty()) {
                    ImGui::Separator();
                    bool modeChanged = false;
                    auto modeTab = [&](int m, const char* lbl, const char* tip) {
                        bool sel = (d.previewMode == m);
                        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.50f, 0.82f, 1.0f));
                        if (ImGui::SmallButton(lbl)) { d.previewMode = m; modeChanged = true; }
                        if (sel) ImGui::PopStyleColor();
                        if (ImGui::BeginItemTooltip()) { ImGui::TextUnformatted(tip); ImGui::EndTooltip(); }
                    };
                    modeTab(0, "Damier", "Verifiez le detourage transparent (le damier gris = zone invisible sur la voiture)");
                    ImGui::SameLine();
                    modeTab(1, "Peinture", "Apercu du visuel detoure directement sur la couleur de carrosserie active");
                    ImGui::SameLine();
                    modeTab(2, "Contours", "Affiche en cyan fluo les traits Black-Hat detectes (reglez 'Seuil Black-Hat' en direct !)");
                    ImGui::SameLine();
                    modeTab(3, "Resultat", "Rendu exact des formes vectorisees sur la peinture (apres conversion)");
                    ImGui::SameLine();
                    modeTab(4, "Erreurs", "Carte thermique : Bleu/Vert=Parfait | Jaune=Moyen | Rouge=Details manquants | Magenta=Debordement");
                    if (modeChanged && d.previewMode <= 2) {
                        app.refreshDecalPreviews(app.selectedDecal);
                    }

                    ID3D11ShaderResourceView* activeSRV = nullptr;
                    int pw = d.prepW, ph = d.prepH;
                    if (d.previewMode == 3) {
                        activeSRV = app.renderer.getPreviewSRV("res_" + std::to_string(app.selectedDecal));
                        pw = d.srcW; ph = d.srcH;
                    } else if (d.previewMode == 4) {
                        activeSRV = app.renderer.getPreviewSRV("err_" + std::to_string(app.selectedDecal));
                        pw = d.srcW; ph = d.srcH;
                    } else {
                        activeSRV = app.renderer.getPreviewSRV("diag_" + std::to_string(app.selectedDecal));
                    }
                    if (!activeSRV) activeSRV = app.renderer.getPreviewSRV("diag_" + std::to_string(app.selectedDecal));

                    if (activeSRV && pw > 0 && ph > 0) {
                        float maxW = ImGui::GetContentRegionAvail().x - 4.0f;
                        float dispW = std::min(maxW, 340.0f);
                        float dispH = dispW * ((float)ph / (float)pw);
                        if (dispH > 260.0f) { dispH = 260.0f; dispW = dispH * ((float)pw / (float)ph); }

                        ImVec2 p0 = ImGui::GetCursorScreenPos();
                        ImGui::Image((ImTextureID)activeSRV, ImVec2(dispW, dispH));
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        ImVec2 p1(p0.x + dispW, p0.y + dispH);

                        // Interactive Click-and-Drag to draw Focus Box directly on the preview!
                        if (ImGui::IsItemHovered() && d.prep.useFocus) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                app.draggingFocus = true;
                                app.focusDragStart = io.MousePos;
                            }
                        }
                        if (app.draggingFocus) {
                            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                                float u0 = std::clamp((app.focusDragStart.x - p0.x) / dispW, 0.0f, 1.0f);
                                float v0 = std::clamp((app.focusDragStart.y - p0.y) / dispH, 0.0f, 1.0f);
                                float u1 = std::clamp((io.MousePos.x - p0.x) / dispW, 0.0f, 1.0f);
                                float v1 = std::clamp((io.MousePos.y - p0.y) / dispH, 0.0f, 1.0f);
                                d.prep.focusRect[0] = std::min(u0, u1); d.prep.focusRect[1] = std::min(v0, v1);
                                d.prep.focusRect[2] = std::max(u0, u1); d.prep.focusRect[3] = std::max(v0, v1);
                            } else {
                                app.draggingFocus = false;
                                app.refreshDecalPreviews(app.selectedDecal);
                            }
                        }

                        // Draw Multi-Tile Grid & live estimated shape budget per cell!
                        if (d.tiling.cols > 1 || d.tiling.rows > 1) {
                            for (int c = 1; c < d.tiling.cols; ++c) {
                                float x = p0.x + dispW * ((float)c / (float)d.tiling.cols);
                                dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(255, 165, 35, 220), 2.0f);
                            }
                            for (int r = 1; r < d.tiling.rows; ++r) {
                                float y = p0.y + dispH * ((float)r / (float)d.tiling.rows);
                                dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(255, 165, 35, 220), 2.0f);
                            }
                            for (int r = 0; r < d.tiling.rows; ++r) {
                                for (int c = 0; c < d.tiling.cols; ++c) {
                                    int ti = r * d.tiling.cols + c;
                                    float tx = p0.x + dispW * ((float)c / (float)d.tiling.cols) + 4.0f;
                                    float ty = p0.y + dispH * ((float)r / (float)d.tiling.rows) + 3.0f;
                                    char tb[32];
                                    std::snprintf(tb, sizeof(tb), "T%d: %df", ti + 1, d.tiling.lastTileBudgets[ti]);
                                    dl->AddRectFilled(ImVec2(tx - 2, ty - 1), ImVec2(tx + 54, ty + 15), IM_COL32(10, 12, 16, 185), 3.0f);
                                    dl->AddText(ImVec2(tx, ty), IM_COL32(255, 195, 70, 255), tb);
                                }
                            }
                        }

                        // Draw Focus Box overlay (Visage / Yeux)
                        if (d.prep.useFocus) {
                            ImVec2 f0(p0.x + d.prep.focusRect[0] * dispW, p0.y + d.prep.focusRect[1] * dispH);
                            ImVec2 f1(p0.x + d.prep.focusRect[2] * dispW, p0.y + d.prep.focusRect[3] * dispH);
                            dl->AddRectFilled(f0, f1, IM_COL32(40, 235, 180, 32));
                            dl->AddRect(f0, f1, IM_COL32(40, 245, 195, 235), 2.0f, 0, 2.0f);
                            dl->AddText(ImVec2(f0.x + 3, f0.y + 2), IM_COL32(60, 255, 205, 255), "FOCUS");
                        }

                        if (d.previewMode == 2) {
                            ImGui::TextColored(ImVec4(0.30f, 0.90f, 1.0f, 1.0f), "%d traits Black-Hat detectes (seuil=%.2f)", d.lastContourCount, d.conv.lineartThreshold);
                        } else if (d.prep.useFocus) {
                            ImGui::TextDisabled("Astuce : cliquez-glissez sur l'image pour tracer la zone FOCUS !");
                        }
                    }
                }
            }

            // --- 1. Placement 3D & Multi-Surfaces ---
            if (ImGui::CollapsingHeader("1. Placement 3D & Routage Carrosserie/Vitres")) {
                const char* projModes[] = {"Projection 3D Multi-Surfaces (Recommande)", "UV Planaire Direct (Legacy)"};
                if (ImGui::Combo("Mode", &d.xform.projectionMode, projModes, 2)) app.liveryDirty = true;
                HelpMarker("3D Multi-Surfaces : projette librement sur toute la voiture et separe automatiquement Carrosserie/Vitres.\nUV Planaire : placement direct sur l'atlas 2D (compatible configs legacy).");

                if (d.xform.projectionMode == 0) {
                    if (ImGui::DragFloat3("Position X/Y/Z", &d.xform.pos.x, 0.01f)) app.liveryDirty = true;
                    if (ImGui::DragFloat3("Rotation (deg)", &d.xform.rotEuler.x, 0.5f)) app.liveryDirty = true;
                    if (ImGui::DragFloat("Largeur (m)", &d.xform.scale.x, 0.01f, 0.05f, 5.0f)) {
                        if (d.xform.lockAspect && d.srcH > 0) d.xform.scale.y = d.xform.scale.x * ((float)d.srcH / (float)d.srcW);
                        app.liveryDirty = true;
                    }
                    if (ImGui::DragFloat("Hauteur (m)", &d.xform.scale.y, 0.01f, 0.05f, 5.0f)) {
                        if (d.xform.lockAspect && d.srcH > 0) d.xform.scale.x = d.xform.scale.y * ((float)d.srcW / (float)d.srcH);
                        app.liveryDirty = true;
                    }
                    if (ImGui::DragFloat("Profondeur proj.", &d.xform.scale.z, 0.01f, 0.05f, 2.5f)) app.liveryDirty = true;
                    HelpMarker("Portee de la boite de projection (en metres). Augmentez si une aile tres bombee n'est pas atteinte.");
                    ImGui::Checkbox("Verrouiller Ratio", &d.xform.lockAspect);
                    ImGui::SameLine();
                    if (ImGui::Checkbox("Miroir H", &d.xform.flipH)) app.liveryDirty = true;
                    ImGui::SameLine();
                    if (ImGui::Checkbox("Miroir V", &d.xform.flipV)) app.liveryDirty = true;

                    bool tBody = (d.xform.targetMask & MASK_BODY) != 0;
                    bool tWin  = (d.xform.targetMask & MASK_WINDOWS) != 0;
                    bool tPlt  = (d.xform.targetMask & MASK_PLATE) != 0;
                    if (ImGui::Checkbox("Carrosserie", &tBody)) { d.xform.targetMask = (d.xform.targetMask & ~MASK_BODY) | (tBody ? MASK_BODY : 0); app.liveryDirty = true; }
                    ImGui::SameLine();
                    if (ImGui::Checkbox("Vitres", &tWin)) { d.xform.targetMask = (d.xform.targetMask & ~MASK_WINDOWS) | (tWin ? MASK_WINDOWS : 0); app.liveryDirty = true; }
                    ImGui::SameLine();
                    if (ImGui::Checkbox("Plaques", &tPlt)) { d.xform.targetMask = (d.xform.targetMask & ~MASK_PLATE) | (tPlt ? MASK_PLATE : 0); app.liveryDirty = true; }
                    if (ImGui::Checkbox("Routage auto Carrosserie <-> Vitres", &d.xform.autoSplitBodyWindows)) app.liveryDirty = true;
                    HelpMarker("Si le decal deborde entre la carrosserie et une vitre, les formes a cheval sont routees sur les 2 calques tout en gardant un alignement continu.");

                    if (ImGui::SmallButton("Capot"))       { d.xform.pos = {0.0f, 0.80f, 1.15f}; d.xform.rotEuler = {-90.0f, 0.0f, 0.0f}; app.liveryDirty = true; }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Pare-brise"))  { d.xform.pos = {0.0f, 0.98f, 0.42f}; d.xform.rotEuler = {-52.0f, 0.0f, 0.0f}; app.liveryDirty = true; }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Flanc G"))     { d.xform.pos = {-1.02f, 0.62f, 0.05f}; d.xform.rotEuler = {0.0f, -90.0f, 0.0f}; app.liveryDirty = true; }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Flanc D"))     { d.xform.pos = {+1.02f, 0.62f, 0.05f}; d.xform.rotEuler = {0.0f, +90.0f, 0.0f}; app.liveryDirty = true; }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Toit"))        { d.xform.pos = {0.0f, 1.25f, -0.35f}; d.xform.rotEuler = {-90.0f, 0.0f, 0.0f}; app.liveryDirty = true; }
                } else {
                    const char* surfs[] = {"Carrosserie", "Vitres", "Plaque"};
                    if (ImGui::Combo("Surface UV", &d.xform.uvSurface, surfs, 3)) app.liveryDirty = true;
                    if (ImGui::DragFloat2("Centre UV", d.xform.uvCenter, 0.002f, 0.0f, 1.0f)) app.liveryDirty = true;
                    if (ImGui::DragFloat("Hauteur UV", &d.xform.uvHeight, 0.003f, 0.02f, 1.0f)) app.liveryDirty = true;
                    if (ImGui::SliderFloat("Rotation UV", &d.xform.uvRotation, -180.0f, 180.0f)) app.liveryDirty = true;
                    if (ImGui::Checkbox("Miroir H", &d.xform.flipH)) app.liveryDirty = true;
                    ImGui::SameLine();
                    if (ImGui::Checkbox("Miroir V", &d.xform.flipV)) app.liveryDirty = true;
                }

                ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f),
                    "Routees : %d Carrosserie | %d Vitres | %d Plaques (%d a cheval)",
                    d.projectedBodyCount, d.projectedWindowCount, d.projectedPlateCount, d.straddleSplitCount);
            }

            // --- 2. Pre-traitement integre ---
            if (ImGui::CollapsingHeader("2. Pre-traitement (Detourage, Couleurs, Focus)", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool changed = false;
                const char* bgModes[] = {"Auto", "Flood + Hysteresie", "Chromakey", "Sticker Bord Blanc", "Alpha PNG", "Aucun"};
                changed |= ImGui::Combo("Detourage Fond", &d.prep.bgMode, bgModes, 6);
                HelpMarker("Methode pour enlever le fond :\n- Flood+Hysteresie : fond uni (photo/JPG)\n- Sticker Bord Blanc : sticker a contour blanc sur fond complexe\n- Chromakey : manga noir & blanc sur fond blanc\n- Alpha PNG : PNG deja transparent\n- Aucun : garde le rectangle entier.");

                changed |= ImGui::SliderFloat("Tolerance Fond", &d.prep.bgTol, 5.0f, 110.0f);
                HelpMarker("Distance couleur par rapport au bord pour supprimer le fond. Augmentez si un halo reste autour du personnage ; baissez si le personnage est grignote.");

                changed |= ImGui::Checkbox("Boucles internes", &d.prep.removeHoles);
                HelpMarker("Ouvre les poches de fond emprisonnees entre les meches de cheveux sans percer les reflets.");
                ImGui::SameLine();
                changed |= ImGui::Checkbox("Peler bord blanc", &d.prep.stripWhiteBorder);
                HelpMarker("Retire la bordure blanche externe des stickers die-cut jusqu'au trait noir (economise ~50 calques !).");

                changed |= ImGui::SliderInt("Erosion bord (px)", &d.prep.erodePx, 0, 4);
                HelpMarker("Rogne la silhouette de 1-2 px pour eliminer tout lisere colore du fond d'origine (recommande : 1 px).");

                changed |= ImGui::SliderInt("Posterisation", &d.prep.posterizeColors, 0, 16, d.prep.posterizeColors < 2 ? "Desactivee" : "%d teintes");
                HelpMarker("Reduit les aplats a N couleurs franches (style anime cel-shading).\n- 4-6 teintes : aplats tres tranches (ideal mode Triangles)\n- 8-12 teintes : equilibre anime HD optimal\n- 0 : couleurs originales.");

                changed |= ImGui::SliderFloat("Contraste", &d.prep.contrastBoost, 0.70f, 1.80f);
                HelpMarker("Renforce la difference entre l'encre noire et les aplats clairs. Recommande : 1.10-1.25 (1.40 pour manga N&B).");

                changed |= ImGui::SliderFloat("Saturation", &d.prep.saturationBoost, 0.00f, 1.80f);
                HelpMarker("Ravive les couleurs (>1.0) pour qu'elles ressortent sur la carrosserie, ou desature (<0.3) pour du lineart N&B.");

                changed |= ImGui::SliderInt("Resolution / tuile", &d.prep.workSize, 200, 512, "%d px");
                HelpMarker("Resolution de calcul par tuile. Plus bas (220-260) = tres rapide pour iterer ; plus haut (380-440) = traits fins pour le rendu final.");

                changed |= ImGui::Checkbox("Zone Focus (Visage / Yeux)", &d.prep.useFocus);
                HelpMarker("Active un rectangle prioritaire (en cyan sur l'apercu). Cliquez-glissez directement sur l'image en haut pour encadrer les yeux/visage !");
                if (d.prep.useFocus) {
                    changed |= ImGui::SliderFloat("Boost Focus", &d.prep.focusBoost, 1.0f, 8.0f, "%.1fx");
                    HelpMarker("Multiplicateur de priorite dans la zone cyan (recommande : 3.5x a 5.0x pour des yeux ultra-nets).");
                }
                if (changed) {
                    d.prepDirty = true;
                    app.refreshDecalPreviews(app.selectedDecal);
                }
            }

            // --- 3. Multi-Tile Tiling ---
            if (ImGui::CollapsingHeader("3. Decoupage Multi-Tuiles (Multi-Tile Tiling)")) {
                bool tChanged = false;
                tChanged |= ImGui::SliderInt("Colonnes (M)", &d.tiling.cols, 1, 4);
                HelpMarker("Nombre de decoupes horizontales. Les limites orange et le budget par tuile s'affichent en direct sur l'apercu et en 3D !");
                tChanged |= ImGui::SliderInt("Lignes (N)", &d.tiling.rows, 1, 4);
                ImGui::SliderInt("Marge Bleed (px)", &d.tiling.bleedPx, 0, 10);
                HelpMarker("Chevauchement en pixels entre tuiles adjacentes pour garantir un assemblage sans aucune couture visible (recommande : 4 px).");
                tChanged |= ImGui::Checkbox("Budget auto par densite", &d.tiling.autoBudgetByDensity);
                HelpMarker("Alloue automatiquement plus de formes aux tuiles riches en contours (visage/yeux) et moins aux zones vides.");
                if (!d.tiling.autoBudgetByDensity && (d.tiling.cols > 1 || d.tiling.rows > 1)) {
                    int nT = d.tiling.cols * d.tiling.rows;
                    for (int t = 0; t < nT && t < 16; ++t) {
                        char tl[32];
                        std::snprintf(tl, sizeof(tl), "Poids Tuile %d", t + 1);
                        tChanged |= ImGui::SliderFloat(tl, &d.tiling.manualWeights[t], 0.1f, 5.0f);
                    }
                }
                if (tChanged) {
                    d.prepDirty = true;
                    app.refreshDecalPreviews(app.selectedDecal);
                }
            }

            // --- 4. Moteur de Conversion & Historique A/B ---
            if (ImGui::CollapsingHeader("4. Conversion & Historique d'Iterations", ImGuiTreeNodeFlags_DefaultOpen)) {
                const char* convModes[] = {
                    "Engine v5 Hybride (Recommande)",
                    "Double Passe: Triangles (95) + Courbes",
                    "Double Passe: Triangles (95) + Unicode"
                };
                ImGui::Combo("Profil", &d.conv.mode, convModes, 3);
                HelpMarker("- Engine v5 Hybride : 10 solides dont triangle 95 + aerographes + courbes 256x256. Meilleur score mesure sur CPM 2.\n- Triangles + Courbes : aplats forces en triangles 95 + contours courbes.\n- Triangles + Unicode : aplats triangles 95 + contours en glyphes texte ( ) | / \\.");

                bool bChanged = ImGui::SliderInt("Budget Formes", &d.conv.totalShapes, 16, kQuotaBody);
                HelpMarker("Nombre total de calques generes pour ce decal.\nLimites CPM 2 : 300 max Carrosserie, 150 max Vitres, 100 max Plaque.");
                if (bChanged) app.refreshDecalPreviews(app.selectedDecal);

                if (d.conv.mode == CONV_TRIANGLES_UNICODE) {
                    ImGui::SliderFloat("Ratio Aplats / Lineart", &d.conv.flatPassRatio, 0.35f, 0.90f);
                    HelpMarker("Part du budget dediee aux triangles d'aplats (ex. 0.65 = 65% triangles, 35% lineart Unicode).");
                }
                if (ImGui::SliderFloat("Seuil Black-Hat", &d.conv.lineartThreshold, 0.08f, 0.40f)) {
                    if (d.previewMode == 2) app.refreshDecalPreviews(app.selectedDecal);
                }
                HelpMarker("Sensibilite de detection des traits d'encre. Cliquez sur l'onglet 'Contours' en haut pour voir les traits detectes en cyan en temps reel !");

                ImGui::SliderFloat("Penalite debordement", &d.conv.bgPenalty, 0.0f, 5.0f);
                HelpMarker("Empeche les formes de depasser hors de la silhouette sur la carrosserie.\n- 0.0 : desactive\n- 2.2 a 3.0 : recommande (zero halo)\n- > 3.5 : coupe tres stricte.");

                ImGui::SliderInt("Candidats / forme", &d.conv.candidates, 100, 600);
                HelpMarker("Nombre de formes aleatoires testees avant d'en choisir une. 140 = rapide (test) | 400+ = haute precision.");

                ImGui::SliderInt("Mutations / forme", &d.conv.mutations, 80, 350);
                HelpMarker("Nombre d'ajustements fins par forme (position/angle/taille). 90 = rapide | 220+ = contours tres propres.");

                ImGui::Checkbox("Alpha adaptatif", &d.conv.adaptiveAlpha);
                HelpMarker("Autorise des formes semi-transparentes pour les ombres douces et reflets. Decochez pour des aplats 100% opaques.");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(72.0f);
                ImGui::InputInt("Seed", &d.conv.seed, 0, 0);
                ImGui::SameLine();
                if (ImGui::SmallButton("Alea")) { d.conv.seed = (d.conv.seed * 1664525 + 1013904223) & 0x7FFF; }
                HelpMarker("Graine aleatoire : cliquez 'Alea' pour generer une variante differente avec les memes reglages.");

                ImGui::Separator();
                ImGui::Checkbox("Auto-Converge (Boucle comparative)", &d.conv.autoConverge);
                HelpMarker("Le logiciel compare automatiquement son rendu a l'image originale apres chaque passe, amplifie les zones d'erreur (visage/contours/trous), ajuste les parametres et garde strictement la meilleure version.");
                if (d.conv.autoConverge) {
                    ImGui::SliderFloat("Objectif Fidelite", &d.conv.targetFidelityPct, 75.0f, 96.0f, "%.0f %%");
                    HelpMarker("Arrete la boucle automatiquement des que la fidelite perceptuelle (SSIM + Premier Plan + Contours) atteint cet objectif.");
                    ImGui::SliderInt("Passes max", &d.conv.maxAutoPasses, 2, 6);
                    HelpMarker("Nombre maximal d'iterations automatiques (3 a 5 recommande pour une correspondance quasi parfaite ; plus lent mais beaucoup plus fiable).");
                }

                float estSec = estimateConversionSeconds(d, app.config.calibratedSecPerUnit);
                if (estSec > 0.0f) {
                    int m = (int)(estSec / 60.0f), s = (int)estSec % 60;
                    if (m > 0) ImGui::TextColored(ImVec4(0.75f, 0.82f, 0.92f, 1.0f), "Estimation calibree (votre CPU) : ~%d min %02d s (max)", m, s);
                    else       ImGui::TextColored(ImVec4(0.75f, 0.82f, 0.92f, 1.0f), "Estimation calibree (votre CPU) : ~%d s (max)", s);
                } else {
                    ImGui::TextDisabled("Duree : sera calibree sur votre CPU apres la 1ere passe.");
                }

                bool canConvert = !d.origRGBA.empty() && !app.worker.isRunning();
                ImGui::BeginDisabled(!canConvert);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.48f, 0.84f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.58f, 0.96f, 1.0f));
                const char* btnLbl = d.conv.autoConverge ? "Lancer Auto-Converge HQ (Async)" : "Convertir (1 passe Async)";
                if (ImGui::Button(btnLbl, ImVec2(-1, 34))) {
                    app.worker.start(app.selectedDecal, d, app.catalog, app.rootDir, app.project.paintColor);
                }
                ImGui::PopStyleColor(2);
                ImGui::EndDisabled();

                if (app.worker.isRunning()) {
                    float el = app.worker.elapsedSeconds();
                    char progTxt[96];
                    std::snprintf(progTxt, sizeof(progTxt), "Passe %d/%d | Ecoule: %.0fs | Best: %.1f%%",
                                  app.worker.currentPass(), app.worker.totalPasses(), el, app.worker.bestFidelityPct());
                    ImGui::ProgressBar(app.worker.progress(), ImVec2(-1, 20), progTxt);
                    ImGui::TextWrapped("%s", app.worker.statusText().c_str());
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.22f, 0.22f, 1.0f));
                    if (ImGui::Button("Arreter & Garder le Meilleur", ImVec2(-1, 24))) {
                        app.worker.requestStopKeepBest();
                    }
                    ImGui::PopStyleColor();
                }

                // Honest Multi-Criteria Perceptual Diagnostic (SSIM + Foreground-only MAE + Focus + Silhouette IoU!)
                if (d.metrics.valid) {
                    ImGui::Separator();
                    ImVec4 vCol = (d.metrics.verdict == "Fidele")  ? ImVec4(0.25f, 0.90f, 0.45f, 1.0f)
                                : (d.metrics.verdict == "Correct") ? ImVec4(0.95f, 0.82f, 0.22f, 1.0f)
                                : (d.metrics.verdict == "Moyen")   ? ImVec4(0.98f, 0.55f, 0.18f, 1.0f)
                                                                   : ImVec4(0.96f, 0.26f, 0.26f, 1.0f);
                    ImGui::TextColored(vCol, "Fidelite reelle : %.1f%% [%s]", d.metrics.fidelityPct, d.metrics.verdict.c_str());
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, vCol);
                    ImGui::ProgressBar(std::clamp(d.metrics.fidelityPct / 100.0f, 0.0f, 1.0f), ImVec2(-1, 14), "");
                    ImGui::PopStyleColor();

                    auto scoreHi = [](float v, float good, float ok) {
                        return (v >= good) ? ImVec4(0.25f, 0.88f, 0.45f, 1.0f)
                             : (v >= ok)   ? ImVec4(0.98f, 0.72f, 0.20f, 1.0f)
                                           : ImVec4(0.96f, 0.30f, 0.28f, 1.0f);
                    };
                    auto scoreLo = [](float v, float good, float ok) {
                        return (v <= good) ? ImVec4(0.25f, 0.88f, 0.45f, 1.0f)
                             : (v <= ok)   ? ImVec4(0.98f, 0.72f, 0.20f, 1.0f)
                                           : ImVec4(0.96f, 0.30f, 0.28f, 1.0f);
                    };

                    ImGui::TextColored(scoreHi(d.metrics.ssimPct, 86.0f, 74.0f), "SSIM: %.1f%%", d.metrics.ssimPct);
                    ImGui::SameLine();
                    ImGui::TextColored(scoreHi(d.metrics.focusSSIMPct, 84.0f, 72.0f), "| Focus: %.1f%%", d.metrics.focusSSIMPct);
                    ImGui::SameLine();
                    ImGui::TextColored(scoreHi(d.metrics.silIoUPct, 93.0f, 84.0f), "| IoU: %.1f%%", d.metrics.silIoUPct);

                    ImGui::TextColored(scoreLo(d.metrics.fgErr, 18.0f, 28.0f), "Err.Objet: %.1f", d.metrics.fgErr);
                    ImGui::SameLine();
                    ImGui::TextColored(scoreLo(d.metrics.edgeErr, 42.0f, 58.0f), "| Contours: %.1f", d.metrics.edgeErr);
                    ImGui::TextColored(scoreLo(d.metrics.spillPct, 1.5f, 3.5f), "Debord: %.1f%%", d.metrics.spillPct);
                    ImGui::SameLine();
                    ImGui::TextColored(scoreLo(d.metrics.holesPct, 2.5f, 5.5f), "| Trous: %.1f%%", d.metrics.holesPct);

                    if (!d.metrics.advice.empty()) {
                        ImGui::TextColored(vCol, "%s", d.metrics.advice.c_str());
                    }
                }

                // Clickable A/B Iteration History!
                if (!d.history.empty()) {
                    ImGui::Separator();
                    ImGui::Text("Historique A/B (cliquez pour comparer en 3D) :");
                    for (int h = (int)d.history.size() - 1; h >= 0; --h) {
                        bool isCur = (h == d.activeHistoryIdx);
                        if (ImGui::Selectable(d.history[(size_t)h].label.c_str(), isCur)) {
                            const auto& it = d.history[(size_t)h];
                            d.activeHistoryIdx = h;
                            d.shapes = it.shapes;
                            d.srcW = it.srcW; d.srcH = it.srcH;
                            d.metrics = it.metrics;
                            d.resultRGBA = it.resultRGBA;
                            d.errorMapRGBA = it.errorMapRGBA;
                            app.refreshDecalPreviews(app.selectedDecal);
                            app.liveryDirty = true;
                        }
                    }
                }
            }
        } else {
            ImGui::TextWrapped("Selectionnez ou ajoutez un Decal 3D dans le panneau de gauche pour configurer son placement, son decoupage multi-tuiles et sa conversion.");
        }
        ImGui::PopItemWidth();
        ImGui::EndChild();

        // ---------------- BOTTOM STATUS BAR ----------------
        ImGui::Separator();
        ImGui::Text("%s", app.statusBanner.c_str());
        ImGui::End(); // CPM2StudioHost

        drawGuideWindow(app.showGuideWindow);

        // Live In-Game GPU RenderTexture Verification Popup (reloaded right after injection!)
        if (app.showLiveGameCaptureWindow) {
            ImGui::SetNextWindowSize(ImVec2(820, 480), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Rendu Jeu Live (Capture GPU directe CPM 2)", &app.showLiveGameCaptureWindow)) {
                ImGui::TextWrapped("%s", app.lastInjReport.summaryText.c_str());
                ImGui::Separator();
                auto* bSRV = app.renderer.getPreviewSRV("live_body_rt");
                auto* wSRV = app.renderer.getPreviewSRV("live_win_rt");
                if (bSRV) {
                    ImGui::BeginGroup();
                    ImGui::Text("CarVinyls RenderTexture (%dx%d)", app.liveBodyW, app.liveBodyH);
                    ImGui::Image((ImTextureID)bSRV, ImVec2(370, 370));
                    ImGui::EndGroup();
                }
                if (bSRV && wSRV) ImGui::SameLine();
                if (wSRV) {
                    ImGui::BeginGroup();
                    ImGui::Text("WindowVinyls RenderTexture (%dx%d)", app.liveWinW, app.liveWinH);
                    ImGui::Image((ImTextureID)wSRV, ImVec2(370, 370));
                    ImGui::EndGroup();
                }
                if (!bSRV && !wSRV) {
                    ImGui::TextDisabled("Aucune capture GPU live disponible. Cliquez 'Injecter en Jeu' ou 'Sync Jeu Live'.");
                }
            }
            ImGui::End();
        }

        // ==================== RENDER DX11 FRAME ====================
        ImGui::Render();
        const float clearCol[4] = {0.08f, 0.09f, 0.11f, 1.0f};
        app.renderer.beginFrame(clearCol);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (!autoShotPath.empty() && frameCount == 4) {
            app.renderer.saveBackbufferPNG(autoShotPath);
            done = true;
        }
        app.renderer.endFrame(true);
        frameCount++;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    app.renderer.shutdown();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}
