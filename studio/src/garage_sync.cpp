#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include "garage_sync.h"
#include "../../third_party/nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace cpm {

static std::string base64Decode(const std::string& in) {
    static const int T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        int d = T[c];
        if (d == -1) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back((char)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

static std::string readRegStr(HKEY hive, const char* subkey, const char* value) {
    HKEY hk = nullptr;
    if (RegOpenKeyExA(hive, subkey, 0, KEY_READ, &hk) != ERROR_SUCCESS) return "";
    char buf[MAX_PATH] = {};
    DWORD sz = sizeof(buf), type = 0;
    std::string res;
    if (RegQueryValueExA(hk, value, nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        res = buf;
    }
    RegCloseKey(hk);
    return res;
}

bool isValidGameDir(const std::string& dir) {
    if (dir.empty()) return false;
    std::error_code ec;
    fs::path bundles = fs::path(dir) / "Car Parking 2_Data" / "StreamingAssets" / "aa" / "StandaloneWindows64";
    return fs::exists(bundles, ec) && fs::is_directory(bundles, ec);
}

std::string autoDetectGameDir(const std::string& rootDir) {
    std::vector<std::string> candidates;

    // 1. Env var
    char envBuf[MAX_PATH] = {};
    if (GetEnvironmentVariableA("CPM2_GAME_DIR", envBuf, MAX_PATH) > 0) candidates.push_back(envBuf);

    // 2. Sibling folder ../CPM2
    if (!rootDir.empty()) {
        candidates.push_back((fs::path(rootDir).parent_path() / "CPM2").string());
    }

    // 3. Steam registry + libraryfolders.vdf
    std::vector<std::string> steamRoots;
    auto s1 = readRegStr(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath");
    auto s2 = readRegStr(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath");
    auto s3 = readRegStr(HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam", "InstallPath");
    for (const auto& s : {s1, s2, s3}) if (!s.empty()) steamRoots.push_back(s);

    std::vector<std::string> libs = steamRoots;
    for (const auto& sr : steamRoots) {
        fs::path vdf = fs::path(sr) / "steamapps" / "libraryfolders.vdf";
        std::ifstream f(vdf);
        if (!f) continue;
        std::string txt((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::regex rx("\"path\"\\s+\"([^\"]+)\"");
        for (std::sregex_iterator it(txt.begin(), txt.end(), rx), end; it != end; ++it) {
            std::string p = (*it)[1].str();
            // unescape \\ -> /
            std::string clean;
            for (size_t i = 0; i < p.size(); ++i) {
                if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') { clean.push_back('\\'); ++i; }
                else clean.push_back(p[i]);
            }
            libs.push_back(clean);
        }
    }
    for (const auto& lib : libs) {
        candidates.push_back((fs::path(lib) / "steamapps" / "common" / "CPM2").string());
    }

    // 4. Common drives fallback
    for (char drv = 'C'; drv <= 'G'; ++drv) {
        std::string d(1, drv);
        candidates.push_back(d + ":\\Program Files (x86)\\Steam\\steamapps\\common\\CPM2");
        candidates.push_back(d + ":\\SteamLibrary\\steamapps\\common\\CPM2");
    }

    for (const auto& c : candidates) {
        if (isValidGameDir(c)) return fs::weakly_canonical(c).string();
    }
    return "";
}

void loadStudioConfig(const std::string& rootDir, StudioConfig& cfg) {
    fs::path p = fs::path(rootDir) / "studio_config.json";
    std::ifstream f(p);
    if (f) {
        try {
            json j; f >> j;
            cfg.gameDir = j.value("gameDir", "");
            cfg.calibratedSecPerUnit = j.value("calibratedSecPerUnit", 0.0f);
        } catch (...) {}
    }
    if (!isValidGameDir(cfg.gameDir)) {
        std::string det = autoDetectGameDir(rootDir);
        if (!det.empty()) {
            cfg.gameDir = det;
            saveStudioConfig(rootDir, cfg);
        }
    }
}

void saveStudioConfig(const std::string& rootDir, const StudioConfig& cfg) {
    fs::path p = fs::path(rootDir) / "studio_config.json";
    json j;
    j["gameDir"] = cfg.gameDir;
    j["calibratedSecPerUnit"] = cfg.calibratedSecPerUnit;
    std::ofstream f(p);
    if (f) f << j.dump(2);
}

uint32_t findGameProcessId() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    uint32_t pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"Car Parking 2.exe") == 0) {
                pid = (uint32_t)pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

std::vector<GarageCarInstance> scanOfflineES3Garage() {
    std::vector<GarageCarInstance> result;
    char userProf[MAX_PATH] = {};
    if (!GetEnvironmentVariableA("USERPROFILE", userProf, MAX_PATH)) return result;

    fs::path es3Dir = fs::path(userProf) / "AppData" / "LocalLow" / "Olzhas" / "Car Parking 2" / "es3";
    std::error_code ec;
    if (!fs::exists(es3Dir, ec) || !fs::is_directory(es3Dir, ec)) return result;

    std::map<std::string, GarageCarInstance> byInst;
    for (const auto& entry : fs::directory_iterator(es3Dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string dec = base64Decode(entry.path().filename().string());
        // Pattern: cfacars/<instanceId>/<subfile>
        if (dec.rfind("cfacars/", 0) != 0) continue;
        std::string rest = dec.substr(8);
        size_t sl = rest.find('/');
        if (sl == std::string::npos) continue;
        std::string instId = rest.substr(0, sl);
        std::string sub = rest.substr(sl + 1);

        // Parse carId from prefix "car<ID>_"
        int cid = -1;
        if (instId.rfind("car", 0) == 0) {
            size_t u = instId.find('_');
            std::string numStr = (u == std::string::npos) ? instId.substr(3) : instId.substr(3, u - 3);
            if (!numStr.empty() && std::all_of(numStr.begin(), numStr.end(), ::isdigit)) {
                cid = std::atoi(numStr.c_str());
            }
        }
        if (cid < 0) continue;

        auto& g = byInst[instId];
        g.carId = cid;
        g.instanceId = instId;
        // Short friendly tag: last 2 underscore segments (e.g. "AI684_84591")
        size_t p1 = instId.find('_');
        if (p1 != std::string::npos) {
            size_t p2 = instId.find('_', p1 + 1);
            g.shortLabel = (p2 != std::string::npos) ? instId.substr(p2 + 1) : instId.substr(p1 + 1);
        } else {
            g.shortLabel = instId;
        }
        uint64_t fsz = entry.file_size(ec);
        if (sub == "carVinyls") { g.hasBodyVinyls = true; g.bodyBytes = fsz; }
        else if (sub == "windowVinyls") { g.hasWindowVinyls = true; g.winBytes = fsz; }
    }

    for (auto& kv : byInst) result.push_back(std::move(kv.second));
    std::sort(result.begin(), result.end(), [](const GarageCarInstance& a, const GarageCarInstance& b) {
        if (a.carId != b.carId) return a.carId < b.carId;
        return a.instanceId < b.instanceId;
    });
    return result;
}

void mergeLiveGarageJson(
    const std::string& workDir,
    std::vector<GarageCarInstance>& instances,
    std::string& outActiveInstance,
    int& outActiveCarId
) {
    outActiveInstance.clear();
    outActiveCarId = -1;
    fs::path p = fs::path(workDir) / "garage_live.json";
    std::ifstream f(p);
    if (!f) return;
    json j;
    try { f >> j; } catch (...) { return; }

    outActiveInstance = j.value("active_instance", "");
    outActiveCarId = j.value("active_car_id", -1);

    for (auto& g : instances) {
        g.isActiveInGame = (!outActiveInstance.empty() && g.instanceId == outActiveInstance);
        g.hasVinylComp = false;
    }
    if (j.contains("live_vehicles") && j["live_vehicles"].is_array()) {
        for (const auto& lv : j["live_vehicles"]) {
            std::string id = lv.value("instance_id", "");
            int cid = lv.value("car_id", -1);
            bool hasVc = lv.value("has_vinyl_comp", false);
            if (id.empty()) continue;
            bool found = false;
            for (auto& g : instances) {
                if (g.instanceId == id) {
                    g.hasVinylComp = hasVc;
                    if (id == outActiveInstance) g.isActiveInGame = true;
                    found = true;
                    break;
                }
            }
            if (!found) {
                GarageCarInstance ng;
                ng.carId = cid;
                ng.instanceId = id;
                ng.shortLabel = id;
                ng.isActiveInGame = (id == outActiveInstance);
                ng.hasVinylComp = hasVc;
                instances.push_back(std::move(ng));
            }
        }
    }
}

void cleanupStaleTempDlls(const std::string& workDir) {
    std::error_code ec;
    fs::path wd(workDir);
    if (!fs::exists(wd, ec)) return;
    for (const auto& entry : fs::directory_iterator(wd, ec)) {
        std::string nm = entry.path().filename().string();
        if (nm.rfind("tmp_inj_", 0) == 0 && entry.path().extension() == ".dll") {
            fs::remove(entry.path(), ec); // silently skips any still locked by game
        }
    }
}

bool injectDllIntoGame(const std::string& dllPath, const std::string& workDir, std::string& outErr) {
    outErr.clear();
    uint32_t pid = findGameProcessId();
    if (pid == 0) {
        outErr = "Jeu 'Car Parking 2.exe' introuvable. Lancez CPM 2 d'abord.";
        return false;
    }
    std::error_code ec;
    if (!fs::exists(dllPath, ec)) {
        outErr = "DLL introuvable : " + dllPath;
        return false;
    }

    // Copy to unique temp DLL inside work/ so LoadLibraryW always triggers DllMain freshly
    // and never collides with a previously loaded module or locks the master DLL!
    fs::create_directories(workDir, ec);
    fs::path tmpDll = fs::path(workDir) / ("tmp_inj_" + std::to_string(GetTickCount64()) + ".dll");
    if (!fs::copy_file(dllPath, tmpDll, fs::copy_options::overwrite_existing, ec)) {
        outErr = "Impossible de copier la DLL temporaire : " + ec.message();
        return false;
    }

    std::wstring wPath = fs::absolute(tmpDll, ec).wstring();
    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) {
        DWORD err = GetLastError();
        outErr = "OpenProcess refuse (code " + std::to_string(err) + "). Lancez le Studio en Administrateur si le jeu tourne en mode admin.";
        return false;
    }

    SIZE_T byteLen = (wPath.size() + 1) * sizeof(wchar_t);
    LPVOID remoteMem = VirtualAllocEx(hProc, nullptr, byteLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        CloseHandle(hProc);
        outErr = "VirtualAllocEx a echoue (code " + std::to_string(GetLastError()) + ").";
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(hProc, remoteMem, wPath.c_str(), byteLen, &written)) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        outErr = "WriteProcessMemory a echoue (code " + std::to_string(GetLastError()) + ").";
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");
    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, loadLib, remoteMem, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        outErr = "CreateRemoteThread a echoue (code " + std::to_string(GetLastError()) + ").";
        return false;
    }

    WaitForSingleObject(hThread, 15000);
    DWORD remoteExit = 0;
    GetExitCodeThread(hThread, &remoteExit);
    CloseHandle(hThread);
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProc);

    if (remoteExit == 0) {
        outErr = "LoadLibraryW a retourne NULL dans le jeu (antivirus ou chemin bloque).";
        return false;
    }
    return true;
}

bool readInjectionReport(const std::string& workDir, InjectionReport& outReport) {
    fs::path doneP = fs::path(workDir) / "inject_done.txt";
    std::ifstream f(doneP);
    if (!f) return false;

    outReport = InjectionReport{};
    outReport.hasReport = true;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.rfind("mode=", 0) == 0) outReport.mode = line.substr(5);
        else if (line.rfind("car=", 0) == 0) outReport.carInstance = line.substr(4);
        else if (line.rfind("carId=", 0) == 0) outReport.carId = std::atoi(line.c_str() + 6);
        else if (line.rfind("layers=", 0) == 0) outReport.totalLayers = std::atoi(line.c_str() + 7);
        else if (line.rfind("jobs=", 0) == 0) outReport.jobsCount = std::atoi(line.c_str() + 5);
        else if (line.rfind("backups=", 0) == 0) outReport.backupsCreated = std::atoi(line.c_str() + 8);
        else if (line.rfind("mounted=", 0) == 0) outReport.mountedLive = (std::atoi(line.c_str() + 8) != 0);
        else if (line.rfind("path=", 0) == 0) outReport.savePath = line.substr(5);
        else if (line.rfind("exc=", 0) == 0) outReport.exc = std::atoi(line.c_str() + 4);
    }
    outReport.success = (outReport.exc == 0 && !outReport.carInstance.empty());

    std::error_code ec;
    fs::path liveB = fs::path(workDir) / "live_vinyl_rt.png";
    fs::path liveW = fs::path(workDir) / "live_window_rt.png";
    if (fs::exists(liveB, ec)) { outReport.hasLiveBodyCapture = true; outReport.liveBodyPngPath = liveB.string(); }
    if (fs::exists(liveW, ec)) { outReport.hasLiveWinCapture = true; outReport.liveWinPngPath = liveW.string(); }

    std::ostringstream oss;
    if (outReport.mode == "scan_garage") {
        oss << "Garage synchronise : actif = car" << outReport.carId << " (" << outReport.carInstance << ")";
    } else if (outReport.success) {
        oss << "SUCCES : " << outReport.totalLayers << " calques injectes sur car" << outReport.carId
            << " [" << outReport.carInstance << "]"
            << " | Backups ES3: " << outReport.backupsCreated
            << (outReport.mountedLive ? " | Rendu rafraichi en direct !" : " | Rechargez la voiture dans le garage.");
    } else {
        oss << "ECHEC injection (exc=" << outReport.exc << ") — consultez work/vinyl_injector.log";
    }
    outReport.summaryText = oss.str();
    return true;
}

} // namespace cpm
