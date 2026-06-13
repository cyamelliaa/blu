/**
 * libPvpHud.so
 * Minecraft Bedrock Native PvP HUD Mod
 *
 * Features:
 *  - Armor HUD (helmet, chestplate, leggings, boots) with durability bars
 *  - Saturation bar (alongside hunger)
 *  - Offhand slot display
 *  - Health display (numeric + bar)
 *  - XP level indicator
 *
 * Architecture: ARM64 Android, GlossHook + Dear ImGui overlay (same as libNoDisconnect)
 *
 * Build: see jni/Android.mk
 */

#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/input.h>
#include <GLES3/gl3.h>
#include <string>
#include <cstring>
#include <cmath>

// --- Third-party headers (included in your project) ---
#include "GlossHook.h"   // or Dobby: #include "dobby.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_android.h"

#define LOG_TAG "PvpHud"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ============================================================
//  Minecraft Bedrock SDK offsets / types
//  Update these for your MCBE version via bedrock-headers!
//  https://github.com/mcbedrock/bedrock-headers
// ============================================================

// These are example offsets for MCBE 1.20.x arm64
// You MUST update them for your target version.
namespace Offsets {
    // LocalPlayer vtable method indices (example)
    constexpr int getHealth       = 0x2A8; // float getHealth()
    constexpr int getMaxHealth    = 0x2AC; // float getMaxHealth()
    constexpr int getFoodLevel    = 0x2B0; // int getFoodLevel()
    constexpr int getSaturation   = 0x2B4; // float getFoodSaturationLevel()
    constexpr int getXpLevel      = 0x300; // int getXpLevel()

    // Armor slot indices in Inventory
    // slots: 0=helmet 1=chestplate 2=leggings 3=boots  offhand=4
    constexpr int armorSlotStart  = 0;
    constexpr int offhandSlot     = 4;
}

// ============================================================
//  Forward declarations of hooked functions
// ============================================================

// Pointer to the real eglSwapBuffers (our render hook point)
using fnEglSwapBuffers = unsigned int (*)(void* dpy, void* surface);
fnEglSwapBuffers orig_eglSwapBuffers = nullptr;

// Pointer to real Android input dispatch
using fnHandleInputEvent = bool (*)(void* self, AInputEvent* event);
fnHandleInputEvent orig_handleInputEvent = nullptr;

// ============================================================
//  Global state
// ============================================================
struct HudState {
    float health      = 20.f;
    float maxHealth   = 20.f;
    int   foodLevel   = 20;
    float saturation  = 5.f;
    int   xpLevel     = 0;

    // Armor durability: [0]=helmet [1]=chest [2]=legs [3]=boots
    // Values: current durability / max durability (0.0 - 1.0)
    float armorDur[4] = {1.f, 1.f, 1.f, 1.f};
    bool  armorEquipped[4] = {false, false, false, false};
    char  armorName[4][64] = {};

    float offhandDur  = 1.f;
    bool  offhandEquipped = false;
    char  offhandName[64] = {};

    bool  imguiInitialized = false;
    ANativeWindow* window  = nullptr;
};

static HudState g_hud;

// ============================================================
//  Minecraft data reading (stubs — fill with real SDK calls)
// ============================================================

// These functions read data from the Minecraft process.
// In a real mod you'd call into libminecraftpe.so via hooks or
// resolved symbols.  Replace these stubs with actual calls.

// Example pattern to find LocalPlayer:
//   void* ClientInstance = *(void**)resolveSymbol("?getInstance@ClientInstance@@SAPEAV1@XZ");
//   void* LocalPlayer    = callVtable<void*>(ClientInstance, vtableIdx_getLocalPlayer);

static void* g_localPlayer = nullptr;

static void refreshMinecraftData() {
    // TODO: resolve g_localPlayer each frame if it's null
    // g_localPlayer = ClientInstance::getInstance()->getLocalPlayer();

    if (!g_localPlayer) {
        // No player yet (main menu, loading). Use defaults.
        return;
    }

    // ----- Health -----
    // float health = callVtable<float>(g_localPlayer, Offsets::getHealth);
    // g_hud.health    = health;
    // g_hud.maxHealth = callVtable<float>(g_localPlayer, Offsets::getMaxHealth);

    // ----- Food / Saturation -----
    // g_hud.foodLevel   = callVtable<int>(g_localPlayer, Offsets::getFoodLevel);
    // g_hud.saturation  = callVtable<float>(g_localPlayer, Offsets::getSaturation);

    // ----- XP -----
    // g_hud.xpLevel = callVtable<int>(g_localPlayer, Offsets::getXpLevel);

    // ----- Armor -----
    // void* inventory = callVtable<void*>(g_localPlayer, vtable_getInventory);
    // for (int i = 0; i < 4; i++) {
    //     void* item = getArmorSlot(inventory, i);
    //     g_hud.armorEquipped[i] = (item != nullptr);
    //     if (item) {
    //         g_hud.armorDur[i] = (float)getDamage(item) / (float)getMaxDamage(item);
    //         strncpy(g_hud.armorName[i], getItemName(item), 63);
    //     }
    // }

    // ----- Offhand -----
    // void* offhandItem = getOffhandItem(g_localPlayer);
    // g_hud.offhandEquipped = (offhandItem != nullptr);
    // if (offhandItem) {
    //     g_hud.offhandDur  = (float)getDamage(offhandItem) / (float)getMaxDamage(offhandItem);
    //     strncpy(g_hud.offhandName, getItemName(offhandItem), 63);
    // }

    // ----- DEMO: animated values so you can see it working before hooking -----
    static float t = 0.f;
    t += 0.01f;
    g_hud.health     = 10.f + 10.f * (0.5f + 0.5f * sinf(t));
    g_hud.saturation = 5.f  + 3.f  * (0.5f + 0.5f * cosf(t * 0.7f));
    g_hud.armorDur[0] = 0.3f + 0.7f * (0.5f + 0.5f * sinf(t * 0.3f));
    g_hud.armorDur[1] = 0.5f + 0.5f * (0.5f + 0.5f * sinf(t * 0.5f));
    g_hud.armorDur[2] = 0.7f + 0.3f * (0.5f + 0.5f * sinf(t * 0.8f));
    g_hud.armorDur[3] = 1.0f - 0.4f * (0.5f + 0.5f * sinf(t * 1.1f));
    g_hud.armorEquipped[0] = g_hud.armorEquipped[1] =
    g_hud.armorEquipped[2] = g_hud.armorEquipped[3] = true;
    g_hud.offhandEquipped = true;
    g_hud.offhandDur = 0.6f + 0.4f * sinf(t * 0.4f);
}

// ============================================================
//  ImGui rendering helpers
// ============================================================

static ImVec4 durabilityColor(float t) {
    // green -> yellow -> red
    if (t > 0.5f) return ImVec4(1.f - 2.f*(t-0.5f)*1.f, 1.f, 0.f, 1.f);
    return ImVec4(1.f, 2.f*t, 0.f, 1.f);
}

static void drawHealthBar() {
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::SetNextWindowPos(ImVec2(10, ImGui::GetIO().DisplaySize.y - 160), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(200, 50), ImGuiCond_Always);
    ImGui::Begin("##health", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoInputs     | ImGuiWindowFlags_NoNav);

    float pct = (g_hud.maxHealth > 0) ? (g_hud.health / g_hud.maxHealth) : 0.f;
    ImVec4 col = (pct > 0.5f) ? ImVec4(0.2f,0.9f,0.2f,1.f)
               : (pct > 0.25f)? ImVec4(0.9f,0.7f,0.1f,1.f)
               :                 ImVec4(0.9f,0.1f,0.1f,1.f);

    ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "\u2665 HP");
    ImGui::SameLine();
    ImGui::TextColored(col, "%.1f / %.0f", g_hud.health, g_hud.maxHealth);

    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    ImGui::ProgressBar(pct, ImVec2(-1, 8), "");
    ImGui::PopStyleColor();

    ImGui::End();
}

static void drawSaturationBar() {
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::SetNextWindowPos(ImVec2(10, ImGui::GetIO().DisplaySize.y - 105), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(200, 50), ImGuiCond_Always);
    ImGui::Begin("##sat", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoInputs     | ImGuiWindowFlags_NoNav);

    // Hunger (food level /20)
    float hungerPct = g_hud.foodLevel / 20.f;
    ImGui::TextColored(ImVec4(0.9f,0.65f,0.2f,1), "\U0001F356 Food");
    ImGui::SameLine();
    ImGui::Text("%d/20", g_hud.foodLevel);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.8f,0.55f,0.1f,1.f));
    ImGui::ProgressBar(hungerPct, ImVec2(-1, 6), "");
    ImGui::PopStyleColor();

    // Saturation (0-20 effectively)
    float satPct = g_hud.saturation / 20.f;
    if (satPct > 1.f) satPct = 1.f;
    ImGui::TextColored(ImVec4(0.9f,0.3f,0.8f,1), "\u2605 Sat");
    ImGui::SameLine();
    ImGui::Text("%.1f", g_hud.saturation);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.8f,0.2f,0.8f,0.8f));
    ImGui::ProgressBar(satPct, ImVec2(-1, 6), "");
    ImGui::PopStyleColor();

    ImGui::End();
}

static void drawArmorHud() {
    const char* slotLabel[4] = {"Helm", "Chest", "Legs", "Boots"};
    // Unicode armor icons (fallback text if font lacks them)
    const char* slotIcon[4]  = {"\U0001F3A9", "\U0001F455", "\U0001F456", "\U0001F45F"};

    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 160, 10), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(150, 130), ImGuiCond_Always);
    ImGui::Begin("##armor", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoInputs     | ImGuiWindowFlags_NoNav);

    ImGui::TextColored(ImVec4(0.8f,0.8f,1,1), "  Armor");
    ImGui::Separator();

    for (int i = 0; i < 4; i++) {
        if (!g_hud.armorEquipped[i]) {
            ImGui::TextDisabled("  %s [empty]", slotLabel[i]);
            continue;
        }
        float dur = g_hud.armorDur[i];
        ImVec4 col = durabilityColor(dur);

        ImGui::Text(" %s", slotLabel[i]);
        ImGui::SameLine(60);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
        char id[16]; snprintf(id, sizeof(id), "##a%d", i);
        ImGui::ProgressBar(dur, ImVec2(80, 10), id);
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

static void drawOffhandHud() {
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 160, 148), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(150, 50), ImGuiCond_Always);
    ImGui::Begin("##offhand", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoInputs     | ImGuiWindowFlags_NoNav);

    ImGui::TextColored(ImVec4(0.8f,0.8f,1,1), "  Offhand");
    ImGui::Separator();

    if (!g_hud.offhandEquipped) {
        ImGui::TextDisabled("  [empty]");
    } else {
        float dur = g_hud.offhandDur;
        ImVec4 col = durabilityColor(dur);
        ImGui::Text(" Item");
        ImGui::SameLine(50);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
        ImGui::ProgressBar(dur, ImVec2(90, 10), "##oh");
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

static void drawXpIndicator() {
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(120, 35), ImGuiCond_Always);
    ImGui::Begin("##xp", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoInputs     | ImGuiWindowFlags_NoNav);

    ImGui::TextColored(ImVec4(0.3f,1.f,0.3f,1.f), "\u272A XP Lvl: %d", g_hud.xpLevel);
    ImGui::End();
}

// ============================================================
//  One-shot ImGui initialisation
// ============================================================
static void initImGui(ANativeWindow* window) {
    if (g_hud.imguiInitialized) return;
    g_hud.window = window;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(
        (float)ANativeWindow_getWidth(window),
        (float)ANativeWindow_getHeight(window)
    );
    io.IniFilename = nullptr; // no imgui.ini

    // Dark style with subtle PvP theming
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.f;
    style.FrameRounding  = 4.f;
    style.WindowBorderSize = 0.f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.05f, 0.08f, 0.55f);

    ImGui_ImplAndroid_Init(window);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    g_hud.imguiInitialized = true;
    LOGI("ImGui initialized for PvP HUD");
}

// ============================================================
//  eglSwapBuffers hook — our render entry point
// ============================================================
static unsigned int hook_eglSwapBuffers(void* dpy, void* surface) {
    // Lazy-init: grab window once we have a surface
    if (!g_hud.imguiInitialized) {
        // Attempt to find ANativeWindow via EGL
        // using eglGetNativeWindowType or saved reference from surfaceCreated
        // For now we rely on JNI surfaceCreated callback below
    }

    if (g_hud.imguiInitialized) {
        // Update game data
        refreshMinecraftData();

        // New ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplAndroid_NewFrame();
        ImGui::NewFrame();

        // Draw all HUD panels
        drawHealthBar();
        drawSaturationBar();
        drawArmorHud();
        drawOffhandHud();
        drawXpIndicator();

        // Render
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    return orig_eglSwapBuffers(dpy, surface);
}

// ============================================================
//  Touch input passthrough hook (so ImGui can receive touch)
// ============================================================
static bool hook_handleInputEvent(void* self, AInputEvent* event) {
    if (g_hud.imguiInitialized) {
        ImGui_ImplAndroid_HandleInputEvent(event);
    }
    return orig_handleInputEvent(self, event);
}

// ============================================================
//  JNI entry: called when the mod .so is loaded
// ============================================================
extern "C" __attribute__((visibility("default")))
jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    LOGI("libPvpHud loaded — hooking...");

    void* libegl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    if (!libegl) {
        LOGE("Failed to open libEGL.so: %s", dlerror());
        return JNI_VERSION_1_6;
    }

    void* eglSwapAddr = dlsym(libegl, "eglSwapBuffers");
    if (!eglSwapAddr) {
        LOGE("eglSwapBuffers not found");
        return JNI_VERSION_1_6;
    }

    // Hook eglSwapBuffers
    GlossHook(eglSwapAddr, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    LOGI("Hooked eglSwapBuffers @ %p", eglSwapAddr);

    // Optional: hook input events for touch passthrough
    // Find the input event dispatcher in libminecraftpe.so
    // void* mcpe = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
    // void* inputAddr = resolveSymbol(mcpe, "handleInputEvent");
    // GlossHook(inputAddr, (void*)hook_handleInputEvent, (void**)&orig_handleInputEvent);

    return JNI_VERSION_1_6;
}

// ============================================================
//  JNI surface callback — called by Java wrapper to pass window
// ============================================================
extern "C" __attribute__((visibility("default")))
void Java_net_yourdomain_pvphud_NativeLib_onSurfaceCreated(
        JNIEnv* env, jclass cls, jobject surface) {
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window) {
        initImGui(window);
    }
}

extern "C" __attribute__((visibility("default")))
void Java_net_yourdomain_pvphud_NativeLib_onSurfaceChanged(
        JNIEnv* env, jclass cls, jint width, jint height) {
    if (g_hud.imguiInitialized) {
        ImGui::GetIO().DisplaySize = ImVec2((float)width, (float)height);
    }
}
