#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <string>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <vector>
#include <unistd.h>

#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define LOG_TAG "PvpHud"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ============================================================
//  Pattern scanner
// ============================================================
struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool>    mask;
    static Pattern parse(const char* pat) {
        Pattern p;
        const char* c = pat;
        while (*c) {
            while (*c == ' ') c++;
            if (!*c) break;
            if (c[0] == '?' && (c[1] == '?' || c[1] == ' ' || !c[1])) {
                p.bytes.push_back(0x00);
                p.mask.push_back(false);
                c += (c[1] == '?') ? 2 : 1;
            } else {
                p.bytes.push_back((uint8_t)strtol(c, nullptr, 16));
                p.mask.push_back(true);
                c += 2;
            }
        }
        return p;
    }
};

struct SoInfo { uintptr_t base = 0; size_t size = 0; };

static SoInfo getSoInfo(const char* soName) {
    SoInfo info;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return info;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, soName)) {
            uintptr_t start, end;
            if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
                if (info.base == 0) info.base = start;
                info.size = end - info.base;
            }
        }
    }
    fclose(f);
    return info;
}

static uintptr_t patternScan(const char* soName, const char* patStr) {
    SoInfo so = getSoInfo(soName);
    if (!so.base || !so.size) { LOGE("patternScan: could not find %s", soName); return 0; }
    Pattern pat = Pattern::parse(patStr);
    if (pat.bytes.empty()) return 0;
    const uint8_t* mem  = (const uint8_t*)so.base;
    const size_t   plen = pat.bytes.size();
    for (size_t i = 0; i + plen <= so.size; i++) {
        bool found = true;
        for (size_t j = 0; j < plen; j++) {
            if (pat.mask[j] && mem[i + j] != pat.bytes[j]) { found = false; break; }
        }
        if (found) {
            LOGI("patternScan: '%s' found at 0x%lx", patStr, so.base + i);
            return so.base + i;
        }
    }
    LOGE("patternScan: '%s' NOT found in %s", patStr, soName);
    return 0;
}

// ============================================================
//  Game function pointers
// ============================================================
static float (*fn_getHealth)(void*)     = nullptr;
static float (*fn_getMaxHealth)(void*)  = nullptr;
static int   (*fn_getFoodLevel)(void*)  = nullptr;
static float (*fn_getSaturation)(void*) = nullptr;
static int   (*fn_getXpLevel)(void*)    = nullptr;
static void* (*fn_getInventory)(void*)  = nullptr;
static void* (*fn_getItem)(void*, int)  = nullptr;
static bool  (*fn_itemIsNull)(void*)    = nullptr;
static int   (*fn_getDamage)(void*)     = nullptr;
static int   (*fn_getMaxDamage)(void*)  = nullptr;

namespace Patterns {
    const char* getHealth    = "F4 4F BE A9 FD 7B 01 A9 FD 43 00 91 F3 03 00 AA";
    const char* getMaxHealth = "F4 4F BE A9 FD 7B 01 A9 FD 43 00 91 F3 03 00 AA ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? E0 03 13 AA";
    const char* getFoodLevel = "08 28 40 F9 09 01 40 F9 28 01 40 F9 00 01 40 F9";
    const char* getSaturation= "08 28 40 F9 09 01 40 F9 28 01 40 F9 20 01 40 F9";
    const char* getXpLevel   = "08 28 40 F9 ?? ?? 40 F9 00 01 40 F9 ?? ?? ?? 94";
    const char* getInventory = "F3 03 00 AA ?? ?? ?? ?? 60 ?? 40 F9 ?? ?? ?? 94 E0 03 00 AA";
    const char* getItem      = "09 00 80 52 2A 69 6A 78 4A 01 09 0B 00 69 6A 38";
    const char* itemIsNull   = "00 00 80 D2 ?? ?? ?? ?? ?? ?? 40 F9 C0 03 5F D6";
    const char* getDamage    = "F4 4F BE A9 FD 7B 01 A9 FD 43 00 91 ?? ?? 40 F9 ?? ?? 40 F9 ?? ?? 40 F9 00 04 40 F9";
    const char* getMaxDamage = "F4 4F BE A9 FD 7B 01 A9 FD 43 00 91 ?? ?? 40 F9 ?? ?? 40 F9 60 00 40 F9";
}

static bool g_offsetsFound = false;
static void initOffsets() {
    if (g_offsetsFound) return;
    LOGI("Scanning for offsets...");
    #define SCAN(fn, pat) { uintptr_t a = patternScan("libminecraftpe.so", pat); if (a) fn = (decltype(fn))a; else LOGE("MISSING: " #fn); }
    SCAN(fn_getHealth,    Patterns::getHealth)
    SCAN(fn_getMaxHealth, Patterns::getMaxHealth)
    SCAN(fn_getFoodLevel, Patterns::getFoodLevel)
    SCAN(fn_getSaturation,Patterns::getSaturation)
    SCAN(fn_getXpLevel,   Patterns::getXpLevel)
    SCAN(fn_getInventory, Patterns::getInventory)
    SCAN(fn_getItem,      Patterns::getItem)
    SCAN(fn_itemIsNull,   Patterns::itemIsNull)
    SCAN(fn_getDamage,    Patterns::getDamage)
    SCAN(fn_getMaxDamage, Patterns::getMaxDamage)
    #undef SCAN
    g_offsetsFound = true;
    LOGI("Offset scan complete.");
}

static void* getLocalPlayer() {
    using Fn = void*(*)();
    static auto getInstance = (Fn)dlsym(
        dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD),
        "_ZN14ClientInstance11getInstanceEv"
    );
    if (!getInstance) return nullptr;
    void* ci = getInstance();
    if (!ci) return nullptr;
    void** vtable = *(void***)ci;
    using FnLP = void*(*)(void*);
    return ((FnLP)vtable[3])(ci);
}

// ============================================================
//  HUD state
// ============================================================
struct HudState {
    float health = 20.f, maxHealth = 20.f;
    int   foodLevel = 20;
    float saturation = 5.f;
    int   xpLevel = 0;
    float armorDur[4]      = {1,1,1,1};
    bool  armorEquipped[4] = {};
    float offhandDur       = 1.f;
    bool  offhandEquipped  = false;
    bool  imguiInitialized = false;
    ANativeWindow* window  = nullptr;
};
static HudState g_hud;

static float safeGetDurability(void* item) {
    if (!item) return 1.f;
    if (fn_itemIsNull && fn_itemIsNull(item)) return 1.f;
    int dmg = fn_getDamage    ? fn_getDamage(item)    : 0;
    int max = fn_getMaxDamage ? fn_getMaxDamage(item) : 0;
    if (max <= 0) return 1.f;
    return 1.f - (float)dmg / (float)max;
}

static void refreshData() {
    void* player = getLocalPlayer();
    if (!player) return;
    if (fn_getHealth)    g_hud.health     = fn_getHealth(player);
    if (fn_getMaxHealth) g_hud.maxHealth  = fn_getMaxHealth(player);
    if (fn_getFoodLevel) g_hud.foodLevel  = fn_getFoodLevel(player);
    if (fn_getSaturation)g_hud.saturation = fn_getSaturation(player);
    if (fn_getXpLevel)   g_hud.xpLevel    = fn_getXpLevel(player);
    void* inv = fn_getInventory ? fn_getInventory(player) : nullptr;
    if (inv && fn_getItem) {
        for (int i = 0; i < 4; i++) {
            void* item = fn_getItem(inv, 36 + i);
            bool  eq   = item && fn_itemIsNull && !fn_itemIsNull(item);
            g_hud.armorEquipped[i] = eq;
            g_hud.armorDur[i]      = eq ? safeGetDurability(item) : 1.f;
        }
        void* oh = fn_getItem(inv, 40);
        g_hud.offhandEquipped = oh && fn_itemIsNull && !fn_itemIsNull(oh);
        g_hud.offhandDur      = safeGetDurability(oh);
    }
}

// ============================================================
//  ImGui drawing
// ============================================================
static ImVec4 durColor(float t) {
    if (t > 0.5f) return ImVec4(2.f*(1.f-t), 1.f, 0.f, 1.f);
    return ImVec4(1.f, 2.f*t, 0.f, 1.f);
}

static void drawHUD() {
    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::SetNextWindowPos({10, io.DisplaySize.y - 160});
    ImGui::SetNextWindowSize({210, 50});
    ImGui::Begin("##hp", nullptr, ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoMove);
    float hpPct = g_hud.maxHealth > 0 ? g_hud.health / g_hud.maxHealth : 0;
    ImVec4 hcol = hpPct > 0.5f ? ImVec4(.2f,.9f,.2f,1) : hpPct > 0.25f ? ImVec4(.9f,.7f,.1f,1) : ImVec4(.9f,.1f,.1f,1);
    ImGui::TextColored({1,.3f,.3f,1}, "\u2665 HP"); ImGui::SameLine();
    ImGui::TextColored(hcol, "%.1f / %.0f", g_hud.health, g_hud.maxHealth);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hcol);
    ImGui::ProgressBar(hpPct, {-1,8}, "");
    ImGui::PopStyleColor();
    ImGui::End();

    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::SetNextWindowPos({10, io.DisplaySize.y - 105});
    ImGui::SetNextWindowSize({210, 55});
    ImGui::Begin("##food", nullptr, ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoMove);
    ImGui::TextColored({.9f,.65f,.2f,1}, "Food"); ImGui::SameLine();
    ImGui::Text("%d/20", g_hud.foodLevel);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(.8f,.55f,.1f,1));
    ImGui::ProgressBar(g_hud.foodLevel/20.f, {-1,6}, "");
    ImGui::PopStyleColor();
    float satPct = fminf(g_hud.saturation/20.f, 1.f);
    ImGui::TextColored({.9f,.3f,.8f,1}, "Sat"); ImGui::SameLine();
    ImGui::Text("%.1f", g_hud.saturation);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(.8f,.2f,.8f,.8f));
    ImGui::ProgressBar(satPct, {-1,6}, "");
    ImGui::PopStyleColor();
    ImGui::End();

    const char* slotName[4] = {"Helm","Chest","Legs","Boots"};
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::SetNextWindowPos({io.DisplaySize.x - 165, 10});
    ImGui::SetNextWindowSize({155, 130});
    ImGui::Begin("##armor", nullptr, ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoMove);
    ImGui::TextColored({.8f,.8f,1,1}, "  Armor"); ImGui::Separator();
    for (int i = 0; i < 4; i++) {
        if (!g_hud.armorEquipped[i]) { ImGui::TextDisabled("  %s [--]", slotName[i]); continue; }
        ImGui::Text(" %s", slotName[i]); ImGui::SameLine(60);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, durColor(g_hud.armorDur[i]));
        char id[8]; snprintf(id,8,"##a%d",i);
        ImGui::ProgressBar(g_hud.armorDur[i], {85,10}, id);
        ImGui::PopStyleColor();
    }
    ImGui::End();

    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::SetNextWindowPos({io.DisplaySize.x - 165, 148});
    ImGui::SetNextWindowSize({155, 45});
    ImGui::Begin("##oh", nullptr, ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoMove);
    ImGui::TextColored({.8f,.8f,1,1}, "  Offhand"); ImGui::Separator();
    if (!g_hud.offhandEquipped) ImGui::TextDisabled("  [empty]");
    else {
        ImGui::Text(" Item"); ImGui::SameLine(50);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, durColor(g_hud.offhandDur));
        ImGui::ProgressBar(g_hud.offhandDur, {95,10}, "##oh2");
        ImGui::PopStyleColor();
    }
    ImGui::End();

    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::SetNextWindowPos({10, 10});
    ImGui::SetNextWindowSize({130, 30});
    ImGui::Begin("##xp", nullptr, ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoMove);
    ImGui::TextColored({.3f,1,.3f,1}, "XP Lvl: %d", g_hud.xpLevel);
    ImGui::End();
}

static void initImGui(ANativeWindow* window) {
    if (g_hud.imguiInitialized) return;
    g_hud.window = window;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {(float)ANativeWindow_getWidth(window), (float)ANativeWindow_getHeight(window)};
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 6.f; s.FrameRounding = 4.f; s.WindowBorderSize = 0.f;
    s.Colors[ImGuiCol_WindowBg] = {.05f,.05f,.08f,.6f};
    ImGui_ImplAndroid_Init(window);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    g_hud.imguiInitialized = true;
    LOGI("ImGui ready");
}

// ============================================================
//  eglSwapBuffers hook
// ============================================================
using fnEgl = unsigned int(*)(void*,void*);
static fnEgl orig_eglSwapBuffers = nullptr;

static unsigned int hook_eglSwapBuffers(void* dpy, void* surface) {
    if (g_hud.imguiInitialized) {
        static bool scanned = false;
        if (!scanned) { initOffsets(); scanned = true; }
        refreshData();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplAndroid_NewFrame();
        ImGui::NewFrame();
        drawHUD();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
    return orig_eglSwapBuffers(dpy, surface);
}

// ============================================================
//  JNI entry — use preloader's Gloss hook
// ============================================================
extern "C" __attribute__((visibility("default")))
jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    LOGI("libPvpHud loading...");

    void* libegl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    if (!libegl) { LOGE("libEGL.so open failed"); return JNI_VERSION_1_6; }

    void* addr = dlsym(libegl, "eglSwapBuffers");
    if (!addr) { LOGE("eglSwapBuffers not found"); return JNI_VERSION_1_6; }

    GlossHook(addr, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    LOGI("Hooked eglSwapBuffers @ %p", addr);

    return JNI_VERSION_1_6;
}

extern "C" __attribute__((visibility("default")))
void Java_net_pvphud_NativeLib_onSurfaceCreated(JNIEnv* env, jclass, jobject surface) {
    ANativeWindow* w = ANativeWindow_fromSurface(env, surface);
    if (w) initImGui(w);
}

extern "C" __attribute__((visibility("default")))
void Java_net_pvphud_NativeLib_onSurfaceChanged(JNIEnv* env, jclass, jint w, jint h) {
    if (g_hud.imguiInitialized)
        ImGui::GetIO().DisplaySize = {(float)w, (float)h};
}
