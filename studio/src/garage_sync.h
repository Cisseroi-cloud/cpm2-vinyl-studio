#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cpm {

struct GarageCarInstance {
    int carId = -1;
    std::string instanceId;      // e.g. "car187_MM534892_AI684_84591" (100% unique even for duplicates!)
    std::string shortLabel;      // human-friendly suffix e.g. "AI684_84591"
    bool hasBodyVinyls = false;
    bool hasWindowVinyls = false;
    uint64_t bodyBytes = 0;
    uint64_t winBytes = 0;
    bool isActiveInGame = false; // true if currently mounted on screen in CPM 2
    bool hasVinylComp = false;
};

struct InjectionReport {
    bool hasReport = false;
    bool success = false;
    std::string mode;
    std::string carInstance;
    int carId = -1;
    int totalLayers = 0;
    int jobsCount = 0;
    int backupsCreated = 0;
    bool mountedLive = false;
    int exc = 0;
    std::string savePath;
    std::string summaryText;
    // Live GPU capture paths updated by the game right after injection
    bool hasLiveBodyCapture = false;
    bool hasLiveWinCapture = false;
    std::string liveBodyPngPath;
    std::string liveWinPngPath;
};

struct StudioConfig {
    std::string gameDir;          // auto-detected or user-chosen CPM2 folder
    float calibratedSecPerUnit = 0.0f; // real measured CPU speed factor (0 = uncalibrated)
};

// Auto-detect CPM2 installation path (env / studio_config.json / Steam registry + libraryfolders.vdf / sibling)
std::string autoDetectGameDir(const std::string& rootDir);
bool isValidGameDir(const std::string& dir);

// Load / Save studio_config.json in rootDir
void loadStudioConfig(const std::string& rootDir, StudioConfig& cfg);
void saveStudioConfig(const std::string& rootDir, const StudioConfig& cfg);

// Detect if "Car Parking 2.exe" is currently running; returns PID (0 if not running)
uint32_t findGameProcessId();

// Read-only scan of %USERPROFILE%\AppData\LocalLow\Olzhas\Car Parking 2\es3\
// Base64-decodes cfacars/<instanceId>/* to discover EVERY owned vehicle instance (including duplicates!)
std::vector<GarageCarInstance> scanOfflineES3Garage();

// Merge live scan result (work/garage_live.json produced by DLL 'scan_garage') into instances list
void mergeLiveGarageJson(const std::string& workDir, std::vector<GarageCarInstance>& instances, std::string& outActiveInstance, int& outActiveCarId);

// Direct native Win32 DLL injection into Car Parking 2.exe (copies DLL to unique work/tmp_inj_*.dll
// so LoadLibraryW always runs fresh and never locks the master DLL; returns detailed error message on failure).
bool injectDllIntoGame(const std::string& dllPath, const std::string& workDir, std::string& outErr);

// Clean up any stale work/tmp_inj_*.dll files from previous sessions
void cleanupStaleTempDlls(const std::string& workDir);

// Parse work/inject_done.txt after injection/scan
bool readInjectionReport(const std::string& workDir, InjectionReport& outReport);

} // namespace cpm
