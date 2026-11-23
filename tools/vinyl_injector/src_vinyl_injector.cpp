// CPM2 vinyl injector: reads a text layer file and writes a VinylData list
// through VinylsDataUtility.TrySaveCarVinyls on the Unity main thread.
// Supports CarVinyls (type=1), PlateVinyls (type=2), WindowVinyls (type=4),
// RGBA RenderTexture capture, and runtime probing.
//
// Layer file line: px py order sx sy rot color stickerIndex xAngle yAngle

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>

typedef void* Il2CppDomain; typedef void* Il2CppAssembly; typedef void* Il2CppImage;
typedef void* Il2CppClass; typedef void* Il2CppObject; typedef void* MethodInfo;
typedef void* FieldInfo; typedef void* Il2CppType; typedef void* Il2CppReflectionType;
typedef void* Il2CppArray; typedef void* Il2CppString;

typedef Il2CppDomain(*t_domain_get)();
typedef void* (*t_thread_attach)(Il2CppDomain);
typedef const Il2CppAssembly** (*t_domain_get_assemblies)(Il2CppDomain, size_t*);
typedef Il2CppImage* (*t_assembly_get_image)(const Il2CppAssembly*);
typedef const char* (*t_image_get_name)(const Il2CppImage*);
typedef size_t (*t_image_get_class_count)(const Il2CppImage*);
typedef Il2CppClass* (*t_image_get_class)(const Il2CppImage*, size_t);
typedef Il2CppClass* (*t_class_from_name)(const Il2CppImage*, const char*, const char*);
typedef const char* (*t_class_get_name)(Il2CppClass*);
typedef const char* (*t_class_get_namespace)(Il2CppClass*);
typedef Il2CppClass* (*t_class_get_parent)(Il2CppClass*);
typedef FieldInfo* (*t_class_get_field_from_name)(Il2CppClass*, const char*);
typedef const MethodInfo* (*t_class_get_methods)(Il2CppClass*, void**);
typedef const char* (*t_method_get_name)(const MethodInfo*);
typedef uint32_t (*t_method_get_param_count)(const MethodInfo*);
typedef size_t (*t_field_get_offset)(FieldInfo*);
typedef const FieldInfo* (*t_class_get_fields)(Il2CppClass*, void**);
typedef const char* (*t_field_get_name)(FieldInfo*);
typedef void* (*t_class_get_static_field_data)(Il2CppClass*);
typedef const Il2CppType* (*t_class_get_type)(Il2CppClass*);
typedef Il2CppReflectionType* (*t_type_get_object)(const Il2CppType*);
typedef Il2CppObject* (*t_runtime_invoke)(const MethodInfo*, void*, void**, Il2CppObject**);
typedef int32_t (*t_array_length)(Il2CppArray*);
typedef Il2CppObject* (*t_object_new)(Il2CppClass*);
typedef Il2CppString* (*t_string_new)(const char*);
typedef void (*t_runtime_class_init)(Il2CppClass*);
typedef uint32_t (*t_gchandle_new)(Il2CppObject*, bool);
typedef void (*t_gchandle_free)(uint32_t);

static t_domain_get p_domain_get; static t_thread_attach p_thread_attach;
static t_domain_get_assemblies p_domain_get_assemblies; static t_assembly_get_image p_assembly_get_image;
static t_image_get_name p_image_get_name; static t_image_get_class_count p_image_get_class_count;
static t_image_get_class p_image_get_class; static t_class_from_name p_class_from_name;
static t_class_get_name p_class_get_name; static t_class_get_namespace p_class_get_namespace;
static t_class_get_parent p_class_get_parent; static t_class_get_field_from_name p_class_get_field_from_name;
static t_class_get_methods p_class_get_methods; static t_method_get_name p_method_get_name;
static t_method_get_param_count p_method_get_param_count; static t_field_get_offset p_field_get_offset;
static t_class_get_fields p_class_get_fields; static t_field_get_name p_field_get_name;
static t_class_get_static_field_data p_class_get_static_field_data;
static t_class_get_type p_class_get_type; static t_type_get_object p_type_get_object;
static t_runtime_invoke p_runtime_invoke; static t_array_length p_array_length;
static t_object_new p_object_new; static t_string_new p_string_new;
static t_runtime_class_init p_runtime_class_init;
static t_gchandle_new p_gchandle_new; static t_gchandle_free p_gchandle_free;

static Il2CppDomain g_domain; static Il2CppImage* g_img; static Il2CppImage* g_core;
static bool g_attached = false;
static const size_t kStringChars = 0x14;
static const size_t kArrayData = 0x20;
static std::string g_outDir;
static std::string g_logPath;
static CRITICAL_SECTION g_logLock;

static std::string ResolveWorkDirFromDll(HMODULE hMod) {
    char buf[MAX_PATH] = {};
    if (GetModuleFileNameA(hMod, buf, MAX_PATH) == 0) return ".\\work";
    std::string p(buf);
    // Walk up up to 4 levels looking for a folder containing "work" or "tools" or "cpm2_studio.exe"
    std::string cur = p;
    for (int i = 0; i < 5; ++i) {
        size_t sl = cur.find_last_of("\\/");
        if (sl == std::string::npos) break;
        cur = cur.substr(0, sl);
        std::string w1 = cur + "\\work";
        std::string m1 = cur + "\\tools";
        std::string m2 = cur + "\\cpm2_studio.exe";
        if (GetFileAttributesA(w1.c_str()) != INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesA(m1.c_str()) != INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesA(m2.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return w1;
        }
    }
    size_t sl = p.find_last_of("\\/");
    return ((sl == std::string::npos) ? std::string(".") : p.substr(0, sl)) + "\\work";
}

static std::string Base64Encode(const std::string& in) {
    static const char* kTbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(kTbl[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(kTbl[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

static void Log(const std::string& s) {
    EnterCriticalSection(&g_logLock);
    FILE* f = fopen(g_logPath.c_str(), "a");
    if (f) { fprintf(f, "%s\n", s.c_str()); fclose(f); }
    LeaveCriticalSection(&g_logLock);
    OutputDebugStringA(("[CPM2Vinyl] " + s + "\n").c_str());
}
static bool SafeCopy(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template <typename T> static bool SafeRead(const void* src, T& out) { return SafeCopy(&out, src, sizeof(T)); }
static bool Readable(const void* p) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    return mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}
static Il2CppClass* SafeClassOf(void* obj) {
    if (!obj || !Readable(obj)) return nullptr;
    void* k = nullptr;
    if (!SafeRead(obj, k) || !k || !Readable(k)) return nullptr;
    return (Il2CppClass*)k;
}
static void EnsureThread() {
    if (!g_attached && g_domain && p_thread_attach) { p_thread_attach(g_domain); g_attached = true; }
}
static void EnsureInit(Il2CppClass* c) { if (c && p_runtime_class_init) p_runtime_class_init(c); }
static std::string ReadString(Il2CppString* p) {
    if (!p) return "";
    struct Hdr { void* k; void* m; int len; } h = {};
    if (!SafeCopy(&h, p, sizeof(h)) || h.len <= 0 || h.len > 4096) return "";
    std::wstring w((size_t)h.len, L'\0');
    if (!SafeCopy(&w[0], (char*)p + kStringChars, (size_t)h.len * 2)) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), h.len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), h.len, &s[0], n, nullptr, nullptr);
    return s;
}
static Il2CppImage* FindImage(const char* name) {
    size_t n = 0; auto asms = p_domain_get_assemblies(p_domain_get(), &n);
    for (size_t i = 0; i < n; ++i) {
        auto im = p_assembly_get_image(asms[i]);
        const char* s = im ? p_image_get_name(im) : nullptr;
        if (s && strcmp(s, name) == 0) return im;
    }
    return nullptr;
}
static Il2CppClass* FindClass(Il2CppImage* img, const char* name) {
    if (!img) return nullptr;
    size_t n = p_image_get_class_count(img);
    for (size_t i = 0; i < n; ++i) {
        auto k = p_image_get_class(img, i);
        const char* cn = k ? p_class_get_name(k) : nullptr;
        if (cn && strcmp(cn, name) == 0) return k;
    }
    return nullptr;
}
static Il2CppClass* FindClassSimple(const char* name) {
    if (g_img) { auto c = FindClass(g_img, name); if (c) return c; }
    if (g_core) { auto c = FindClass(g_core, name); if (c) return c; }
    return nullptr;
}
static MethodInfo* FindMethod(Il2CppClass* c, const char* name, int pc) {
    if (!c) return nullptr;
    void* it = nullptr; const MethodInfo* m;
    while ((m = p_class_get_methods(c, &it))) {
        const char* n = p_method_get_name(m);
        if (n && strcmp(n, name) == 0 && (int)p_method_get_param_count(m) == pc)
            return const_cast<MethodInfo*>(m);
    }
    return nullptr;
}
static MethodInfo* FindMethodInHierarchy(Il2CppClass* c, const char* name, int pc) {
    for (auto k = c; k; k = (Il2CppClass*)p_class_get_parent(k)) {
        auto m = FindMethod(k, name, pc);
        if (m) return m;
    }
    return nullptr;
}
static FieldInfo* FindFieldInHierarchy(Il2CppClass* c, const char* name) {
    for (auto k = c; k; k = (Il2CppClass*)p_class_get_parent(k)) {
        auto f = p_class_get_field_from_name(k, name);
        if (f) return f;
    }
    return nullptr;
}
static int OffsetOf(void* obj, const char* name) {
    auto f = FindFieldInHierarchy(SafeClassOf(obj), name);
    return f ? (int)p_field_get_offset(f) : -1;
}
static bool ReadPtr(void* obj, const char* name, void*& p) {
    int o = OffsetOf(obj, name); return o >= 0x10 && SafeRead((char*)obj + o, p);
}
static bool ReadInt(void* obj, const char* name, int& v) {
    int o = OffsetOf(obj, name); return o >= 0x10 && SafeRead((char*)obj + o, v);
}
static bool ReadStr(void* obj, const char* name, std::string& s) {
    void* p = nullptr; if (!ReadPtr(obj, name, p) || !p) return false;
    s = ReadString((Il2CppString*)p); return true;
}
static bool WritePtr(void* obj, const char* name, void* v) {
    int o = OffsetOf(obj, name); return o >= 0x10 && SafeCopy((char*)obj + o, &v, sizeof(void*));
}
static bool WriteVec3(void* obj, const char* name, float x, float y, float z) {
    int o = OffsetOf(obj, name); if (o < 0x10) return false;
    float v[3] = { x,y,z }; return SafeCopy((char*)obj + o, v, 12);
}
static bool WriteUInt(void* obj, const char* name, uint32_t v) {
    int o = OffsetOf(obj, name); return o >= 0x10 && SafeCopy((char*)obj + o, &v, 4);
}
static bool WriteLong(void* obj, const char* name, int64_t v) {
    int o = OffsetOf(obj, name); return o >= 0x10 && SafeCopy((char*)obj + o, &v, 8);
}
static bool ReadList(void* list, std::vector<void*>& out) {
    out.clear(); if (!list) return false;
    auto cls = SafeClassOf(list);
    auto fi = FindFieldInHierarchy(cls, "_items");
    auto fs = FindFieldInHierarchy(cls, "_size");
    if (!fi || !fs) return false;
    void* arr = nullptr; int size = 0;
    if (!SafeRead((char*)list + p_field_get_offset(fi), arr) || !arr) return false;
    if (!SafeRead((char*)list + p_field_get_offset(fs), size) || size < 0) return false;
    int alen = p_array_length((Il2CppArray*)arr);
    if (alen < 0) return false;
    if (size > alen) size = alen;
    if (size > 20000) size = 20000;
    for (int i = 0; i < size; ++i) {
        void* item = nullptr;
        if (SafeRead((char*)arr + kArrayData + (size_t)i * 8, item) && item) out.push_back(item);
    }
    return true;
}
static std::vector<void*> FindAllObjects(Il2CppClass* cls) {
    std::vector<void*> out;
    if (!cls || !g_core) return out;
    auto res = FindClass(g_core, "Resources");
    auto m = FindMethod(res, "FindObjectsOfTypeAll", 1);
    if (!m) return out;
    auto ty = p_type_get_object(p_class_get_type(cls));
    void* args[1] = { ty };
    Il2CppObject* exc = nullptr;
    auto arr = p_runtime_invoke(m, nullptr, args, &exc);
    if (exc || !arr) return out;
    int n = p_array_length((Il2CppArray*)arr);
    if (n <= 0 || n > 5000) return out;
    for (int i = 0; i < n; ++i) {
        void* item = nullptr;
        if (SafeRead((char*)arr + kArrayData + (size_t)i * 8, item) && item) out.push_back(item);
    }
    return out;
}
struct Guard {
    uint32_t h = 0;
    Guard() = default;
    Guard(const Guard&) = delete;
    ~Guard() { if (h && p_gchandle_free) p_gchandle_free(h); }
    void root(void* o) { if (o && p_gchandle_new && !h) h = p_gchandle_new((Il2CppObject*)o, false); }
};
struct Layer {
    float px, py; int order; float sx, sy, rot; uint32_t color;
    int sticker, xAngle, yAngle;
    float ix, iy, iz;
    int isText = 0, italic = 0, bold = 0, flipX = 0, fontIndex = 0;
    std::string text;
};
static bool LoadLayers(const std::string& path, std::vector<Layer>& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        Layer l{};
        l.ix = 0.0f; l.iy = 0.72f; l.iz = 1.10f;
        std::string mainPart = line;
        size_t bar1 = line.find('|');
        if (bar1 != std::string::npos) {
            mainPart = line.substr(0, bar1);
            size_t bar2 = line.find('|', bar1 + 1);
            if (bar2 != std::string::npos) {
                std::string meta = line.substr(bar1 + 1, bar2 - bar1 - 1);
                sscanf_s(meta.c_str(), "%d %d %d %d %d", &l.isText, &l.italic, &l.bold, &l.flipX, &l.fontIndex);
                l.text = line.substr(bar2 + 1);
            }
        }
        int n = sscanf_s(mainPart.c_str(), "%f %f %d %f %f %f %u %d %d %d %f %f %f",
            &l.px, &l.py, &l.order, &l.sx, &l.sy, &l.rot, &l.color,
            &l.sticker, &l.xAngle, &l.yAngle, &l.ix, &l.iy, &l.iz);
        if (n >= 10) out.push_back(l);
    }
    return !out.empty();
}

static std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

static volatile LONG g_done = 0;
static HWND g_hwnd = nullptr;
static WNDPROC g_old = nullptr;
static const UINT WM_RUN = WM_APP + 0x516;

// Read RenderTexture with true RGBA32 (preserving alpha!), falling back to CommonExtensions.ToTexture2D
static bool SaveRTToPng(void* rt, const std::string& outPath) {
    if (!rt) return false;
    auto imgConv = FindImage("UnityEngine.ImageConversionModule.dll");
    auto icCls = imgConv ? FindClass(imgConv, "ImageConversion") : nullptr;
    auto objCls = FindClass(g_core, "Object");
    auto rtCls = FindClass(g_core, "RenderTexture");
    auto texCls = FindClass(g_core, "Texture2D");
    if (!icCls || !objCls) { Log("SaveRT missing classes"); return false; }
    auto encPng = FindMethod(icCls, "EncodeToPNG", 1);
    auto destroyM = FindMethod(objCls, "Destroy", 1);
    if (!encPng) { Log("SaveRT missing EncodeToPNG"); return false; }

    void* tex = nullptr;
    Guard texG;

    // Try direct RGBA32 readback first so alpha channel is preserved!
    if (rtCls && texCls) {
        auto getW = FindMethodInHierarchy(SafeClassOf(rt), "get_width", 0);
        auto getH = FindMethodInHierarchy(SafeClassOf(rt), "get_height", 0);
        auto getAct = FindMethod(rtCls, "get_active", 0);
        auto setAct = FindMethod(rtCls, "set_active", 1);
        auto ctor2 = FindMethod(texCls, ".ctor", 2); // Texture2D(int w, int h) -> unambiguous RGBA32!
        auto readPx = FindMethod(texCls, "ReadPixels", 3); // (Rect source, int destX, int destY)
        auto applyM = FindMethod(texCls, "Apply", 0);
        if (getW && getH && getAct && setAct && ctor2 && readPx && applyM) {
            Il2CppObject* exc = nullptr;
            auto rw = p_runtime_invoke(getW, rt, nullptr, &exc);
            auto rh = p_runtime_invoke(getH, rt, nullptr, &exc);
            int w = 0, h = 0;
            if (rw && rh) { SafeRead((char*)rw + 0x10, w); SafeRead((char*)rh + 0x10, h); }
            Log("RT dims=" + std::to_string(w) + "x" + std::to_string(h));
            if (w > 0 && h > 0 && w <= 8192 && h <= 8192) {
                auto newTex = p_object_new(texCls);
                if (newTex) {
                    void* ca[2] = { &w, &h };
                    exc = nullptr;
                    p_runtime_invoke(ctor2, newTex, ca, &exc);
                    if (!exc) {
                        texG.root(newTex);
                        exc = nullptr;
                        auto prevAct = p_runtime_invoke(getAct, nullptr, nullptr, &exc);
                        void* sa1[1] = { rt }; exc = nullptr;
                        p_runtime_invoke(setAct, nullptr, sa1, &exc);

                        float rect[4] = { 0.0f, 0.0f, (float)w, (float)h };
                        int dx = 0, dy = 0;
                        void* ra[3] = { rect, &dx, &dy };
                        exc = nullptr;
                        p_runtime_invoke(readPx, newTex, ra, &exc);
                        if (!exc) {
                            exc = nullptr;
                            p_runtime_invoke(applyM, newTex, nullptr, &exc);
                        }
                        void* sa0[1] = { prevAct }; Il2CppObject* e2 = nullptr;
                        p_runtime_invoke(setAct, nullptr, sa0, &e2);

                        if (!exc) {
                            tex = newTex;
                        } else {
                            Log("RGBA32 ReadPixels exc, falling back");
                            if (destroyM) { void* ad[1] = { newTex }; Il2CppObject* ed = nullptr; p_runtime_invoke(destroyM, nullptr, ad, &ed); }
                        }
                    } else {
                        Log("Texture2D(w,h) ctor exc, falling back");
                    }
                }
            }
        }
    }

    if (!tex) {
        auto ceCls = FindClassSimple("CommonExtensions");
        auto toTex = ceCls ? FindMethod(ceCls, "ToTexture2D", 1) : nullptr;
        if (!toTex) { Log("ToTexture2D missing"); return false; }
        Il2CppObject* exc = nullptr;
        void* a1[1] = { rt };
        tex = p_runtime_invoke(toTex, nullptr, a1, &exc);
        if (exc || !tex) { Log("ToTexture2D failed"); return false; }
        texG.root(tex);
    }

    void* a2[1] = { tex };
    Il2CppObject* exc = nullptr;
    auto bytes = p_runtime_invoke(encPng, nullptr, a2, &exc);
    bool ok = false;
    if (!exc && bytes) {
        int len = p_array_length((Il2CppArray*)bytes);
        if (len > 0 && len < 64 * 1024 * 1024) {
            std::ofstream f(outPath, std::ios::binary);
            if (f) {
                f.write((const char*)bytes + kArrayData, len);
                ok = true;
                Log("saved " + outPath + " bytes=" + std::to_string(len));
            }
        }
    } else {
        Log("EncodeToPNG failed");
    }
    if (destroyM) {
        void* ad[1] = { tex };
        exc = nullptr;
        p_runtime_invoke(destroyM, nullptr, ad, &exc);
    }
    return ok;
}

static void* GetComp(void* vehicle, const char* getter) {
    if (!vehicle) return nullptr;
    auto m = FindMethod(SafeClassOf(vehicle), getter, 0);
    if (!m) return nullptr;
    Il2CppObject* e = nullptr;
    auto r = p_runtime_invoke(m, vehicle, nullptr, &e);
    return e ? nullptr : r;
}

static void CaptureAllRTs(void* vehicle, void* vinylComp) {
    // 1. Body vinyl RT
    void* bodyRT = nullptr;
    if (vinylComp) {
        auto getRT = FindMethod(SafeClassOf(vinylComp), "get_RenderTexture", 0);
        if (getRT) {
            Il2CppObject* exc = nullptr;
            bodyRT = p_runtime_invoke(getRT, vinylComp, nullptr, &exc);
            if (exc) bodyRT = nullptr;
        }
    }
    Log(std::string("bodyRT=") + (bodyRT ? "ok" : "null"));
    if (bodyRT) SaveRTToPng(bodyRT, g_outDir + "\\live_vinyl_rt.png");

    // 2. Window vinyl RT
    void* winComp = GetComp(vehicle, "get_WindowComposition");
    void* winRT = nullptr;
    if (winComp) ReadPtr(winComp, "WindowRT", winRT);
    Log(std::string("windowRT=") + (winRT ? "ok" : "null"));
    if (winRT) SaveRTToPng(winRT, g_outDir + "\\live_window_rt.png");

    // 3. Plate RTs (Front & Rear)
    void* platesComp = GetComp(vehicle, "get_PlatesComposition");
    void* frontPlateRT = nullptr;
    void* rearPlateRT = nullptr;
    if (platesComp) {
        ReadPtr(platesComp, "FrontPlateRT", frontPlateRT);
        ReadPtr(platesComp, "RearPlateRT", rearPlateRT);
    }
    Log(std::string("frontPlateRT=") + (frontPlateRT ? "ok" : "null") +
        " rearPlateRT=" + (rearPlateRT ? "ok" : "null"));
    if (frontPlateRT) SaveRTToPng(frontPlateRT, g_outDir + "\\live_front_plate_rt.png");
    if (rearPlateRT) SaveRTToPng(rearPlateRT, g_outDir + "\\live_rear_plate_rt.png");
}

static void ProbeVehicle(void* vehicle, void* vinylComp, const std::string& instanceId) {
    auto dirCls = FindClassSimple("DirectoryHelper");
    EnsureInit(dirCls);
    auto callDir = [&](const char* mname, const std::string& arg) -> std::string {
        auto m = FindMethod(dirCls, mname, 1);
        if (!m) return "<missing>";
        auto s = p_string_new(arg.c_str());
        void* a[1] = { s }; Il2CppObject* e = nullptr;
        auto r = p_runtime_invoke(m, nullptr, a, &e);
        return (!e && r) ? ReadString((Il2CppString*)r) : "<exc>";
    };
    Log("probe GetCarVinylsPath=" + callDir("GetCarVinylsPath", instanceId));
    Log("probe GetWindowVinylsPath=" + callDir("GetWindowVinylsPath", instanceId));
    Log("probe GetPlateVinylsPath('1')=" + callDir("GetPlateVinylsPath", "1"));
    Log("probe GetPlateVinylsPath('Plate1')=" + callDir("GetPlateVinylsPath", "Plate1"));

    auto pdu = FindClassSimple("PlateDataUtility");
    if (pdu) {
        EnsureInit(pdu);
        auto tryGet = FindMethod(pdu, "TryGetPlateIdForCar", 2);
        if (tryGet) {
            for (int f = 0; f <= 1; ++f) {
                bool isFront = (f == 1);
                auto s = p_string_new(instanceId.c_str());
                void* a[2] = { s, &isFront }; Il2CppObject* e = nullptr;
                auto r = p_runtime_invoke(tryGet, nullptr, a, &e);
                int pid = -999;
                if (!e && r) SafeRead((char*)r + 0x10, pid);
                Log(std::string("probe TryGetPlateIdForCar isFront=") + (isFront ? "true" : "false") + " -> " + std::to_string(pid));
            }
        }
    }

    // Check CarMaterial paint color (_Color1 / _BaseColor / _Color)
    if (vehicle) {
        auto getMat = FindMethodInHierarchy(SafeClassOf(vehicle), "get_CarMaterial", 0);
        if (getMat) {
            Il2CppObject* e = nullptr;
            auto mat = p_runtime_invoke(getMat, vehicle, nullptr, &e);
            if (!e && mat) {
                auto getColStr = FindMethod(SafeClassOf(mat), "GetColor", 1);
                if (getColStr) {
                    const char* props[] = { "_Color1", "_BaseColor", "_Color", "_PaintColor" };
                    for (auto pn : props) {
                        auto ps = p_string_new(pn);
                        void* a[1] = { ps }; e = nullptr;
                        auto cr = p_runtime_invoke(getColStr, mat, a, &e);
                        if (!e && cr) {
                            float c[4] = {};
                            SafeCopy(c, (char*)cr + 0x10, 16);
                            char buf[128];
                            snprintf(buf, sizeof(buf), "probe CarMat %s = (%.3f, %.3f, %.3f, %.3f)", pn, c[0], c[1], c[2], c[3]);
                            Log(buf);
                        }
                    }
                }
            }
        }
    }

    // Also check VinylsComposition.GetVinylsCount for body & windows
    if (vinylComp) {
        auto getCnt = FindMethod(SafeClassOf(vinylComp), "GetVinylsCount", 2);
        if (getCnt) {
            for (uint8_t vt : { (uint8_t)1, (uint8_t)4, (uint8_t)2 }) {
                bool isF = (vt == 2);
                void* ca[2] = { &vt, &isF }; Il2CppObject* e = nullptr;
                auto r = p_runtime_invoke(getCnt, vinylComp, ca, &e);
                int cnt = -1;
                if (!e && r) SafeRead((char*)r + 0x10, cnt);
                Log("probe GetVinylsCount type=" + std::to_string((int)vt) + " -> " + std::to_string(cnt));
            }
        }
    }

    // Dump V.Fonts & sticker atlases (STICKERS_NEW_2023 + main car sticker) + plate mesh bounds!
    {
        auto vCls = FindClassSimple("V");
        auto objCls = FindClass(g_core, "Object");
        auto getName = objCls ? FindMethod(objCls, "get_name", 0) : nullptr;
        if (vCls && getName) {
            EnsureInit(vCls);
            void* sdata = p_class_get_static_field_data(vCls);
            auto fFonts = FindFieldInHierarchy(vCls, "<Fonts>k__BackingField");
            if (sdata && fFonts) {
                void* arr = nullptr;
                SafeRead((char*)sdata + p_field_get_offset(fFonts), arr);
                if (arr) {
                    int n = p_array_length((Il2CppArray*)arr);
                    Log("probe V.Fonts count=" + std::to_string(n));
                    for (int i = 0; i < n && i < 64; ++i) {
                        void* fobj = nullptr;
                        SafeRead((char*)arr + kArrayData + (size_t)i * 8, fobj);
                        if (!fobj) continue;
                        Il2CppObject* e = nullptr;
                        auto nm = p_runtime_invoke(getName, fobj, nullptr, &e);
                        Log("  font[" + std::to_string(i) + "]=" + ((!e && nm) ? ReadString((Il2CppString*)nm) : "?"));
                    }
                }
            }
            // Dump STICKERS_NEW_2023 & main car sticker textures via Graphics.Blit -> tmpRT -> PNG!
            auto fAll = FindFieldInHierarchy(vCls, "AllVinyls");
            auto gfxCls = FindClass(g_core, "Graphics");
            auto rtCls = FindClass(g_core, "RenderTexture");
            auto blitM = gfxCls ? FindMethod(gfxCls, "Blit", 2) : nullptr;
            auto getTemp = rtCls ? FindMethod(rtCls, "GetTemporary", 3) : nullptr;
            auto relTemp = rtCls ? FindMethod(rtCls, "ReleaseTemporary", 1) : nullptr;
            if (sdata && fAll && blitM && getTemp && relTemp) {
                void* arr = nullptr;
                SafeRead((char*)sdata + p_field_get_offset(fAll), arr);
                if (arr) {
                    int n = p_array_length((Il2CppArray*)arr);
                    bool dumped2023 = false, dumpedMain = false;
                    for (int i = 0; i < n && !(dumped2023 && dumpedMain); ++i) {
                        void* item = nullptr;
                        SafeRead((char*)arr + kArrayData + (size_t)i * 8, item);
                        if (!item) continue;
                        void* sp = nullptr;
                        if (!ReadPtr(item, "sprite", sp) || !sp) continue;
                        auto getTex = FindMethod(SafeClassOf(sp), "get_texture", 0);
                        if (!getTex) continue;
                        Il2CppObject* e = nullptr;
                        auto tex = p_runtime_invoke(getTex, sp, nullptr, &e);
                        if (e || !tex) continue;
                        auto tnm = p_runtime_invoke(getName, tex, nullptr, &e);
                        std::string tn = (!e && tnm) ? ReadString((Il2CppString*)tnm) : "";
                        bool is2023 = (tn == "STICKERS_NEW_2023" && !dumped2023);
                        bool isMain = (tn == "main car sticker" && !dumpedMain);
                        if (is2023 || isMain) {
                            auto getW = FindMethodInHierarchy(SafeClassOf(tex), "get_width", 0);
                            auto getH = FindMethodInHierarchy(SafeClassOf(tex), "get_height", 0);
                            int tw = 0, th = 0;
                            if (getW && getH) {
                                auto rw = p_runtime_invoke(getW, tex, nullptr, &e);
                                auto rh = p_runtime_invoke(getH, tex, nullptr, &e);
                                if (rw && rh) { SafeRead((char*)rw + 0x10, tw); SafeRead((char*)rh + 0x10, th); }
                            }
                            Log("probe atlas " + tn + " " + std::to_string(tw) + "x" + std::to_string(th));
                            if (tw > 0 && th > 0) {
                                int depth = 0;
                                void* ta[3] = { &tw, &th, &depth }; e = nullptr;
                                auto tmpRT = p_runtime_invoke(getTemp, nullptr, ta, &e);
                                if (!e && tmpRT) {
                                    Guard rtG; rtG.root(tmpRT);
                                    void* ba[2] = { tex, tmpRT }; e = nullptr;
                                    p_runtime_invoke(blitM, nullptr, ba, &e);
                                    std::string fn = is2023 ? "\\atlas_stickers_new_2023.png" : "\\atlas_main_car_sticker.png";
                                    SaveRTToPng(tmpRT, g_outDir + fn);
                                    void* ra[1] = { tmpRT }; e = nullptr;
                                    p_runtime_invoke(relTemp, nullptr, ra, &e);
                                }
                            }
                            if (is2023) dumped2023 = true;
                            if (isMain) dumpedMain = true;
                        }
                    }
                }
            }
        }
        // Check live PlatesComposition mesh bounds!
        void* platesComp = GetComp(vehicle, "get_PlatesComposition");
        if (platesComp) {
            void* pArr = nullptr;
            if (ReadPtr(platesComp, "Plates", pArr) && pArr) {
                int pn = p_array_length((Il2CppArray*)pArr);
                Log("probe PlatesComposition.Plates len=" + std::to_string(pn));
                auto mfCls = FindClass(g_core, "MeshFilter");
                auto mfType = mfCls ? p_type_get_object(p_class_get_type(mfCls)) : nullptr;
                auto goCls = FindClass(g_core, "GameObject");
                auto getCompInCh = goCls ? FindMethod(goCls, "GetComponentInChildren", 1) : nullptr;
                for (int i = 0; i < pn && i < 4; ++i) {
                    void* pgo = nullptr;
                    SafeRead((char*)pArr + kArrayData + (size_t)i * 8, pgo);
                    if (!pgo || !mfType || !getCompInCh) continue;
                    void* ca[1] = { mfType }; Il2CppObject* e = nullptr;
                    auto mf = p_runtime_invoke(getCompInCh, pgo, ca, &e);
                    if (!e && mf) {
                        auto getMesh = FindMethod(SafeClassOf(mf), "get_sharedMesh", 0);
                        auto mesh = getMesh ? p_runtime_invoke(getMesh, mf, nullptr, &e) : nullptr;
                        if (!e && mesh) {
                            auto getBnd = FindMethod(SafeClassOf(mesh), "get_bounds", 0);
                            auto bnd = getBnd ? p_runtime_invoke(getBnd, mesh, nullptr, &e) : nullptr;
                            if (!e && bnd) {
                                float bv[6] = {}; // center(3), extents(3)
                                SafeCopy(bv, (char*)bnd + 0x10, 24);
                                char buf[160];
                                snprintf(buf, sizeof(buf), "  plate[%d] mesh size=(%.4f, %.4f, %.4f)", i, bv[3]*2, bv[4]*2, bv[5]*2);
                                Log(buf);
                            }
                        }
                    }
                }
            }
        }
    }

    // Also capture all RTs in RGBA32!
    CaptureAllRTs(vehicle, vinylComp);
}

static void RunInjection() {
    g_domain = p_domain_get();
    EnsureThread();
    Log("RunInjection start");
    g_img = FindImage("Assembly-CSharp.dll");
    g_core = FindImage("UnityEngine.CoreModule.dll");
    if (!g_img || !g_core) { Log("images missing"); return; }

    // Parse work/cmd.txt:
    //   Line 1: "capture" | "probe" | "inject <layers_file>"
    //   Subsequent lines (optional, any order):
    //     car=<carId>
    //     type=<1|2|4>          (1=CarVinyls, 2=PlateVinyls, 4=WindowVinyls)
    //     plate=<front|rear|both>
    //     plateId=<int>         (override plate slot ID for type=2)
    //     capture=1             (also dump RTs after inject)
    //   OR atomic multi-surface batch mode:
    //     inject_batch
    //     car=<carId>
    //     body=<layers.txt>
    //     windows=<layers.txt>
    //     plate_file=<layers.txt>
    //     backup=1
    //     capture=1
    std::string mode = "inject";
    std::string layersFile = g_outDir + "\\test_layers.txt";
    std::string batchBodyFile, batchWinFile, batchPlateFile;
    std::string targetInstanceId;
    int targetCarId = -1;
    int vinylTypeInt = 1;
    std::string plateSide = "both";
    int plateIdOverride = -1;
    bool captureAfter = false;
    bool doBackup = false;
    {
        std::ifstream cf(g_outDir + "\\cmd.txt");
        std::string line;
        if (cf && std::getline(cf, line)) {
            line = Trim(line);
            size_t sp = line.find(' ');
            if (sp == std::string::npos) {
                mode = line;
            } else {
                mode = Trim(line.substr(0, sp));
                std::string arg = Trim(line.substr(sp + 1));
                if (!arg.empty()) layersFile = arg;
            }
        }
        while (cf && std::getline(cf, line)) {
            line = Trim(line);
            if (line.empty() || line[0] == '#') continue;
            if (line.rfind("car=", 0) == 0) targetCarId = std::atoi(line.c_str() + 4);
            else if (line.rfind("instance=", 0) == 0) targetInstanceId = Trim(line.substr(9));
            else if (line.rfind("type=", 0) == 0) vinylTypeInt = std::atoi(line.c_str() + 5);
            else if (line.rfind("plate=", 0) == 0) plateSide = line.substr(6);
            else if (line.rfind("plateId=", 0) == 0) plateIdOverride = std::atoi(line.c_str() + 8);
            else if (line.rfind("capture=", 0) == 0) captureAfter = (std::atoi(line.c_str() + 8) != 0);
            else if (line.rfind("backup=", 0) == 0) doBackup = (std::atoi(line.c_str() + 7) != 0);
            else if (line.rfind("body=", 0) == 0) batchBodyFile = Trim(line.substr(5));
            else if (line.rfind("windows=", 0) == 0) batchWinFile = Trim(line.substr(8));
            else if (line.rfind("plate_file=", 0) == 0) batchPlateFile = Trim(line.substr(11));
        }
    }
    Log("mode=" + mode + " file=" + layersFile + " targetCar=" + std::to_string(targetCarId) +
        " targetInst=" + targetInstanceId +
        " type=" + std::to_string(vinylTypeInt) + " plate=" + plateSide + " plateId=" + std::to_string(plateIdOverride) +
        " backup=" + (doBackup ? "1" : "0"));

    // Find ALL live VehicleData instances in memory (handles duplicate models in garage cleanly!)
    struct LiveVeh {
        void* veh = nullptr;
        void* vc = nullptr;
        void* info = nullptr;
        std::string instanceId;
        int carId = -1;
    };
    std::vector<LiveVeh> liveVehs;
    void* activeVehicle = nullptr;
    void* vinylComp = nullptr;
    void* carInfo = nullptr;
    std::string instanceId;
    int carId = -1;
    auto vehCls = FindClassSimple("VehicleData");
    auto vehicles = FindAllObjects(vehCls);
    Log("VehicleData=" + std::to_string(vehicles.size()));
    for (auto v : vehicles) {
        void* info = nullptr;
        if (ReadPtr(v, "_lastKnownCarInfo", info) && info) {
            std::string s; int cid = -1;
            if (ReadStr(info, "CarInstanceId", s) && !s.empty()) {
                ReadInt(info, "CarID", cid);
                void* vc = GetComp(v, "get_VinylsComposition");
                Log("  veh id=" + s + " carId=" + std::to_string(cid) + " vc=" + (vc ? "ok" : "null"));
                liveVehs.push_back({v, vc, info, s, cid});
            }
        }
    }

    // Priority selection:
    // 1) Exact targetInstanceId match (resolves duplicate models in garage with 100% precision!)
    // 2) Else match targetCarId (preferring one with active VinylsComposition)
    // 3) Else any active vehicle with VinylsComposition
    for (const auto& lv : liveVehs) {
        if (!targetInstanceId.empty()) {
            if (lv.instanceId != targetInstanceId) continue;
        } else if (targetCarId >= 0) {
            if (lv.carId != targetCarId) continue;
        }
        bool prefer = (!activeVehicle) || (lv.vc != nullptr && vinylComp == nullptr);
        if (prefer) {
            activeVehicle = lv.veh; vinylComp = lv.vc; carInfo = lv.info;
            instanceId = lv.instanceId; carId = lv.carId;
        }
    }
    // Even if user targeted a non-mounted duplicate CarInstanceId in their garage, allow direct ES3 save via TrySaveCarVinyls!
    if (instanceId.empty() && !targetInstanceId.empty()) {
        instanceId = targetInstanceId;
        carId = targetCarId;
        Log("Targeting non-mounted garage instance directly: " + instanceId);
    }
    Log("selected carInfo id=" + instanceId + " carId=" + std::to_string(carId) + " vc=" + (vinylComp ? "ok" : "null"));

    if (mode == "scan_garage") {
        std::ofstream gf(g_outDir + "\\garage_live.json");
        if (gf) {
            gf << "{\n  \"active_instance\": \"" << instanceId << "\",\n"
               << "  \"active_car_id\": " << carId << ",\n"
               << "  \"live_vehicles\": [\n";
            for (size_t i = 0; i < liveVehs.size(); ++i) {
                gf << "    {\"instance_id\":\"" << liveVehs[i].instanceId
                   << "\",\"car_id\":" << liveVehs[i].carId
                   << ",\"has_vinyl_comp\":" << (liveVehs[i].vc ? "true" : "false") << "}";
                if (i + 1 < liveVehs.size()) gf << ",";
                gf << "\n";
            }
            gf << "  ]\n}\n";
        }
        if (captureAfter && activeVehicle) CaptureAllRTs(activeVehicle, vinylComp);
        std::ofstream done(g_outDir + "\\inject_done.txt");
        if (done) done << "done\nmode=scan_garage\ncar=" << instanceId << "\ncarId=" << carId
                       << "\nlive_count=" << liveVehs.size() << "\nexc=0\n";
        InterlockedExchange(&g_done, 1);
        return;
    }

    if (mode == "capture") {
        CaptureAllRTs(activeVehicle, vinylComp);
        std::ofstream done(g_outDir + "\\inject_done.txt");
        if (done) done << "done\nmode=capture\ncar=" << instanceId << "\ncarId=" << carId << "\n";
        InterlockedExchange(&g_done, 1);
        return;
    }

    if (mode == "probe") {
        ProbeVehicle(activeVehicle, vinylComp, instanceId);
        std::ofstream done(g_outDir + "\\inject_done.txt");
        if (done) done << "done\nmode=probe\ncar=" << instanceId << "\ncarId=" << carId << "\n";
        InterlockedExchange(&g_done, 1);
        return;
    }

    if (instanceId.empty()) { Log("no target CarInstanceId"); return; }
    Guard carGuard; if (carInfo) carGuard.root(carInfo);

    // Build list of (vinylType, filePath) jobs (supports both single 'inject' and atomic 'inject_batch')
    struct SurfaceJob { uint8_t vtype; std::string file; int quota; };
    std::vector<SurfaceJob> jobs;
    if (mode == "inject_batch") {
        if (!batchBodyFile.empty())  jobs.push_back({1, batchBodyFile, 300});
        if (!batchWinFile.empty())   jobs.push_back({4, batchWinFile, 150});
        if (!batchPlateFile.empty()) jobs.push_back({2, batchPlateFile, 100});
    } else {
        int q = (vinylTypeInt == 4) ? 150 : (vinylTypeInt == 2 ? 100 : 300);
        jobs.push_back({(uint8_t)vinylTypeInt, layersFile, q});
    }
    if (jobs.empty()) { Log("no surface jobs to inject"); return; }

    // Helper: non-destructive timestamped ES3 backup before writing (CPM2 stores ES3 files as Base64("cfa" + relPath))
    int backupsCreated = 0;
    auto backupEs3IfExists = [&](const std::string& relSavePath) {
        if (!doBackup || relSavePath.empty()) return;
        char userProf[MAX_PATH] = {};
        if (!GetEnvironmentVariableA("USERPROFILE", userProf, MAX_PATH)) return;
        std::string norm = relSavePath;
        for (char& c : norm) if (c == '\\') c = '/';
        std::string b64Name = Base64Encode("cfa" + norm);
        std::string es3Base = std::string(userProf) + "\\AppData\\LocalLow\\Olzhas\\Car Parking 2\\es3\\";
        std::string src = es3Base + b64Name;
        if (GetFileAttributesA(src.c_str()) == INVALID_FILE_ATTRIBUTES) {
            // Fallback plain relative path check
            std::string alt = es3Base + relSavePath;
            for (char& c : alt) if (c == '/') c = '\\';
            if (GetFileAttributesA(alt.c_str()) == INVALID_FILE_ATTRIBUTES) return;
            src = alt;
        }
        std::string bakDir = g_outDir + "\\backups";
        CreateDirectoryA(bakDir.c_str(), nullptr);
        SYSTEMTIME st{}; GetLocalTime(&st);
        char ts[64];
        std::snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        std::string flat = norm;
        for (char& c : flat) if (c == '/' || c == '\\' || c == ':') c = '_';
        std::string dst = bakDir + "\\" + ts + "_" + flat + ".bak";
        if (CopyFileA(src.c_str(), dst.c_str(), FALSE)) {
            backupsCreated++;
            Log("ES3 backup ok -> " + dst);
        }
    };

    // Discover concrete List<VinylData> Il2CppClass from a preset WITHOUT mutating the preset!
    auto renderer = FindClassSimple("CarVinylRenderer");
    auto tryLoad = FindMethod(renderer, "TryLoadDefaultPreset", 2);
    if (!tryLoad) { Log("TryLoadDefaultPreset missing"); return; }
    Il2CppClass* listCls = nullptr;
    for (int id = 0; id <= 300 && !listCls; ++id) {
        void* preset = nullptr; int cid = id;
        void* args[2] = { &cid, &preset };
        Il2CppObject* exc = nullptr;
        auto r = p_runtime_invoke(tryLoad, nullptr, args, &exc);
        bool ok = false; if (!exc && r) SafeRead((char*)r + 0x10, ok);
        if (!ok || !preset) continue;
        void* vynil = nullptr;
        if (!ReadPtr(preset, "vynilOneCarList", vynil) || !vynil) continue;
        void* l = nullptr;
        if (!ReadPtr(vynil, "allVynils", l) || !l) continue;
        listCls = SafeClassOf(l);
        Log("discovered List<VinylData> class from preset car=" + std::to_string(id));
    }
    if (!listCls) { Log("no List<VinylData> class"); return; }

    auto listCtor = FindMethod(listCls, ".ctor", 0);
    auto addM = FindMethod(listCls, "Add", 1);
    auto vdCls = FindClassSimple("VinylData");
    auto dirCls = FindClassSimple("DirectoryHelper");
    EnsureInit(dirCls);
    auto util = FindClassSimple("VinylsDataUtility");
    auto save = FindMethod(util, "TrySaveCarVinyls", 3);
    if (!listCtor || !addM || !vdCls || !save) { Log("core IL2CPP methods missing"); return; }

    auto emptyStr = p_string_new("");
    Guard emptyStrG; emptyStrG.root(emptyStr);

    int totalLayersInjected = 0;
    int anyExc = 0;
    std::string lastSavePath;
    uint8_t lastType = jobs.back().vtype;

    for (const auto& job : jobs) {
        std::vector<Layer> layers;
        if (!LoadLayers(job.file, layers)) { Log("cannot read " + job.file); continue; }
        if ((int)layers.size() > job.quota) {
            Log("WARNING clamping type=" + std::to_string((int)job.vtype) + " from " + std::to_string(layers.size()) + " to quota " + std::to_string(job.quota));
            layers.resize((size_t)job.quota);
        }
        Log("job type=" + std::to_string((int)job.vtype) + " layers=" + std::to_string(layers.size()));

        void* list = p_object_new(listCls);
        if (!list) { Log("List alloc failed"); continue; }
        Guard listGuard; listGuard.root(list);
        Il2CppObject* exc = nullptr;
        p_runtime_invoke(listCtor, list, nullptr, &exc);
        if (exc) { Log("List .ctor exception"); anyExc = 1; continue; }

        for (size_t idx = 0; idx < layers.size(); ++idx) {
            const auto& l = layers[idx];
            auto obj = p_object_new(vdCls);
            if (!obj) { Log("object_new VinylData failed"); break; }
            WriteVec3(obj, "position", l.px, l.py, (float)idx);
            WriteVec3(obj, "scaleRotation", l.sx, l.sy, l.rot);
            WriteVec3(obj, "iconPosition", l.ix, l.iy, l.iz);
            void* txtPtr = l.text.empty() ? (void*)emptyStr : (void*)p_string_new(l.text.c_str());
            WritePtr(obj, "text", txtPtr);
            WriteUInt(obj, "color", l.color);
            int64_t packed = ((int64_t)(l.isText & 1)) |
                             ((int64_t)(l.italic & 1) << 1) |
                             ((int64_t)(l.bold & 1) << 2) |
                             ((int64_t)(l.flipX & 1) << 3) |
                             ((int64_t)(l.fontIndex & 0x1F) << 4) |
                             ((int64_t)(l.sticker & 0x1FF) << 9) |
                             ((int64_t)(l.xAngle & 0x1FF) << 18) |
                             ((int64_t)(l.yAngle & 0x1FF) << 27);
            WriteLong(obj, "packedData", packed);
            void* addArgs[1] = { obj };
            exc = nullptr;
            p_runtime_invoke(addM, list, addArgs, &exc);
            if (exc) { Log("List.Add exception"); anyExc = 1; break; }
        }
        std::vector<void*> check; ReadList(list, check);
        totalLayersInjected += (int)check.size();
        Log("list size after add=" + std::to_string(check.size()));

        uint8_t vinylType = job.vtype;
        if (vinylType == 1 || vinylType == 4) {
            const char* pathMethod = (vinylType == 4) ? "GetWindowVinylsPath" : "GetCarVinylsPath";
            auto getPath = FindMethod(dirCls, pathMethod, 1);
            if (!getPath) { Log(std::string("DirectoryHelper.") + pathMethod + " missing"); continue; }
            auto idStr = p_string_new(instanceId.c_str());
            void* gpArgs[1] = { idStr };
            exc = nullptr;
            auto pathObj = p_runtime_invoke(getPath, nullptr, gpArgs, &exc);
            if (exc || !pathObj) { Log(std::string(pathMethod) + " failed"); anyExc = 1; continue; }
            Guard pathGuard; pathGuard.root(pathObj);
            lastSavePath = ReadString((Il2CppString*)pathObj);
            Log("savePath=" + lastSavePath);
            backupEs3IfExists(lastSavePath);

            void* args[3] = { list, pathObj, &vinylType };
            exc = nullptr;
            p_runtime_invoke(save, nullptr, args, &exc);
            if (exc) anyExc = 1;
            Log(std::string("TrySaveCarVinyls exc=") + (exc ? "1" : "0") + " type=" + std::to_string((int)vinylType));

            if (vinylComp) {
                auto renderV = FindMethod(SafeClassOf(vinylComp), "RenderVinyls", 2);
                if (renderV) {
                    bool flag = false;
                    void* rArgs[2] = { &vinylType, &flag };
                    Il2CppObject* rexc = nullptr;
                    p_runtime_invoke(renderV, vinylComp, rArgs, &rexc);
                    Log(std::string("RenderVinyls exc=") + (rexc ? "1" : "0") + " type=" + std::to_string((int)vinylType));
                }
            }
        } else if (vinylType == 2) {
            auto pdu = FindClassSimple("PlateDataUtility");
            EnsureInit(pdu);
            auto tryGet = pdu ? FindMethod(pdu, "TryGetPlateIdForCar", 2) : nullptr;
            auto setPlate = pdu ? FindMethod(pdu, "SetPlateOnCar", 4) : nullptr;
            auto getPath = FindMethod(dirCls, "GetPlateVinylsPath", 1);
            if (!getPath) { Log("DirectoryHelper.GetPlateVinylsPath missing"); continue; }

            bool doFront = (plateSide == "front" || plateSide == "both");
            bool doRear = (plateSide == "rear" || plateSide == "both");
            int pid = plateIdOverride;
            if (pid <= 0 && tryGet) {
                bool isF = doFront;
                auto s = p_string_new(instanceId.c_str());
                void* a[2] = { s, &isF }; Il2CppObject* e = nullptr;
                auto r = p_runtime_invoke(tryGet, nullptr, a, &e);
                if (!e && r) SafeRead((char*)r + 0x10, pid);
                if (pid <= 0 && doRear) {
                    isF = false;
                    void* a2[2] = { s, &isF }; e = nullptr;
                    auto r2 = p_runtime_invoke(tryGet, nullptr, a2, &e);
                    if (!e && r2) SafeRead((char*)r2 + 0x10, pid);
                }
            }
            if (pid <= 0) pid = 1;
            Log("plate target pid=" + std::to_string(pid));

            if (setPlate) {
                auto s = p_string_new(instanceId.c_str());
                if (doFront) {
                    bool isFront = true; bool enable = true;
                    void* sa[4] = { &pid, s, &isFront, &enable }; Il2CppObject* e = nullptr;
                    p_runtime_invoke(setPlate, nullptr, sa, &e);
                }
                if (doRear) {
                    bool isFront = false; bool enable = true;
                    void* sa[4] = { &pid, s, &isFront, &enable }; Il2CppObject* e = nullptr;
                    p_runtime_invoke(setPlate, nullptr, sa, &e);
                }
            }

            std::string key = std::to_string(pid);
            auto keyStr = p_string_new(key.c_str());
            void* gpArgs[1] = { keyStr };
            exc = nullptr;
            auto pathObj = p_runtime_invoke(getPath, nullptr, gpArgs, &exc);
            if (exc || !pathObj) { Log("GetPlateVinylsPath failed"); anyExc = 1; continue; }
            Guard pathGuard; pathGuard.root(pathObj);
            lastSavePath = ReadString((Il2CppString*)pathObj);
            Log("plate savePath=" + lastSavePath);
            backupEs3IfExists(lastSavePath);

            void* args[3] = { list, pathObj, &vinylType };
            exc = nullptr;
            p_runtime_invoke(save, nullptr, args, &exc);
            if (exc) anyExc = 1;
            Log(std::string("TrySaveCarVinyls plate exc=") + (exc ? "1" : "0"));

            auto pcCls = FindClassSimple("PlateConstructor");
            auto setPlatesM = pcCls ? FindMethod(pcCls, "SetPlates", 2) : nullptr;
            if (setPlatesM && activeVehicle && carInfo) {
                void* pa[2] = { activeVehicle, carInfo }; Il2CppObject* pe = nullptr;
                p_runtime_invoke(setPlatesM, nullptr, pa, &pe);
            }
            if (vinylComp) {
                auto renderV = FindMethod(SafeClassOf(vinylComp), "RenderVinyls", 2);
                if (renderV) {
                    if (doFront) {
                        bool isFront = true;
                        void* rArgs[2] = { &vinylType, &isFront }; Il2CppObject* rexc = nullptr;
                        p_runtime_invoke(renderV, vinylComp, rArgs, &rexc);
                    }
                    if (doRear) {
                        bool isFront = false;
                        void* rArgs[2] = { &vinylType, &isFront }; Il2CppObject* rexc = nullptr;
                        p_runtime_invoke(renderV, vinylComp, rArgs, &rexc);
                    }
                }
            }
        }
    }

    if (captureAfter) {
        CaptureAllRTs(activeVehicle, vinylComp);
    }

    std::ofstream done(g_outDir + "\\inject_done.txt");
    if (done) done << "done\nmode=" << mode << "\ntype=" << (int)lastType << "\njobs=" << jobs.size()
                   << "\nlayers=" << totalLayersInjected
                   << "\nbackups=" << backupsCreated
                   << "\nmounted=" << (vinylComp ? 1 : 0)
                   << "\ncar=" << instanceId << "\ncarId=" << carId
                   << "\npath=" << lastSavePath << "\nexc=" << anyExc << "\n";
    InterlockedExchange(&g_done, 1);
}
static bool RunGuarded() {
    __try { RunInjection(); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static HMODULE g_self = nullptr;
static LRESULT CALLBACK HookProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_RUN) {
        WNDPROC old = g_old;
        if (old) SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)old);
        if (!RunGuarded()) Log("RunInjection SEH");
        InterlockedExchange(&g_done, 1);
        return 0;
    }
    return CallWindowProcW(g_old, h, m, w, l);
}
static BOOL CALLBACK EnumProc(HWND h, LPARAM l) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    char cls[256] = {}; GetClassNameA(h, cls, sizeof(cls));
    if (strstr(cls, "Unity")) { *reinterpret_cast<HWND*>(l) = h; return FALSE; }
    return TRUE;
}
static void ThreadMain() {
    for (int i = 0; i < 120 && !GetModuleHandleA("GameAssembly.dll"); ++i) Sleep(1000);
    HMODULE ga = GetModuleHandleA("GameAssembly.dll");
    if (!ga) { Log("GameAssembly missing"); return; }
    #define R(n) p_##n=(t_##n)GetProcAddress(ga,"il2cpp_" #n); if(!p_##n){Log("missing " #n);return;}
    R(domain_get); R(thread_attach); R(domain_get_assemblies); R(assembly_get_image); R(image_get_name);
    R(image_get_class_count); R(image_get_class); R(class_from_name); R(class_get_name); R(class_get_namespace);
    R(class_get_parent); R(class_get_field_from_name); R(class_get_methods); R(method_get_name);
    R(method_get_param_count); R(field_get_offset); R(class_get_fields); R(field_get_name);
    R(class_get_static_field_data); R(class_get_type); R(type_get_object); R(runtime_invoke);
    R(array_length); R(object_new); R(string_new);
    #undef R
    p_runtime_class_init=(t_runtime_class_init)GetProcAddress(ga,"il2cpp_runtime_class_init");
    p_gchandle_new=(t_gchandle_new)GetProcAddress(ga,"il2cpp_gchandle_new");
    p_gchandle_free=(t_gchandle_free)GetProcAddress(ga,"il2cpp_gchandle_free");
    g_domain=p_domain_get();
    Log("IL2CPP ready");
    for (int i=0;i<60 && !g_hwnd;i++){ EnumWindows(EnumProc,(LPARAM)&g_hwnd); if(!g_hwnd) Sleep(1000); }
    if (!g_hwnd){ Log("window missing"); return; }
    g_old=(WNDPROC)SetWindowLongPtrW(g_hwnd,GWLP_WNDPROC,(LONG_PTR)HookProc);
    if (!g_old){ Log("SetWindowLongPtr failed"); return; }
    PostMessageW(g_hwnd,WM_RUN,0,0);
    for (int i=0;i<120 && !g_done;i++) Sleep(1000);
    Log("thread exit");
    Sleep(500);
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_logLock);
        g_outDir = ResolveWorkDirFromDll(h);
        CreateDirectoryA(g_outDir.c_str(), nullptr);
        g_logPath = g_outDir + "\\vinyl_injector.log";
        Log("DLL attached (workDir=" + g_outDir + ")");
        HANDLE t=CreateThread(nullptr,0,[](LPVOID)->DWORD{ThreadMain(); FreeLibraryAndExitThread(g_self, 0); return 0;},nullptr,0,nullptr);
        if(t) CloseHandle(t);
    }
    return TRUE;
}
