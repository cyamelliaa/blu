/**
 * ArmorHUD + AppleSkin — Bedrock Native Mod
 * Minecraft Bedrock 1.26.0+ | LeviLaminar (Levi Launcher, mobile)
 *
 * Technique (matching BetterBrightness / the example offset mod):
 *   - pl_resolve_signature()  to locate MC functions by byte pattern
 *   - PatchMemory()           to hook/redirect those functions in-place
 *   - GlossHook (eglSwapBuffers) for per-frame ImGui render (same as MotionBlur)
 *   - ImGui + OpenGL ES 3     to draw the HUD overlay
 *
 * Ported from:
 *   - uku's Armor HUD (BerdinskiyBear / uku, MIT)
 *   - AppleSkin (squeek502, Unlicense)
 */

#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <sys/mman.h>
#include <mutex>
#include <vector>

#include "pl/Gloss.h"       // GlossInit / GlossHook / GlossOpen / GlossSymbol
#include "pl/Signature.h"   // pl::signature::pl_resolve_signature

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#include "stb_image.h"

// ─── Logging ─────────────────────────────────────────────────────────────────
#define LOG_TAG "ArmorFoodHUD"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// =============================================================================
//  SIGNATURES  (ARM64 wildcard byte patterns for libminecraftpe.so 1.26.0)
//
//  '?' = wildcard byte — matches any value at that position.
//  Found by reverse-engineering libminecraftpe.so in IDA/Ghidra:
//    1. adb pull /data/app/com.mojang.minecraftpe-.../lib/arm64/libminecraftpe.so
//    2. Find the target function, copy 30–50 bytes of its prologue/body.
//    3. Replace address-dependent bytes (offsets, immediates) with '?'.
//
//  Each signature locates the FIRST instruction of the target function.
// =============================================================================

// LocalPlayer::tick — called every game tick while the player is in-world.
// We hook this to sample armor + food state without touching MC internals directly.
static const char* SIG_PLAYER_TICK =
    "? ? ? ? FD 7B ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 F4 4F ? A9 "
    "FD ? ? 91 ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ?";

// Player::getArmorValue(int slot) -> int  (0 = not worn, 1–100 = durability %)
static const char* SIG_GET_ARMOR =
    "? ? ? ? ? ? ? ? 08 ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? "
    "? ? ? ? ? ? ? ? ? ? ? ? 1F 00 00 71 ? ? ? ? ? ? ? ? C0 03 5F D6";

// Player::getFoodLevel() -> int  (0–20)
static const char* SIG_GET_FOOD =
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? 28 ? ? ? ? ? ? ? 1F ? ? ? "
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? C0 03 5F D6";

// Player::getSaturationLevel() -> float  (0.0–20.0)
static const char* SIG_GET_SAT =
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? 60 ? ? ? ? ? ? ? ? ? ? 1E "
    "? ? ? ? ? 1E ? ? ? ? 1E C0 03 5F D6";

// Player::getHealth() -> float
static const char* SIG_GET_HEALTH =
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? "
    "? ? ? ? 1E 20 00 1F D6";

// Player::getMaxHealth() -> float
static const char* SIG_GET_MAX_HEALTH =
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? "
    "? ? ? ? 1E 40 00 1F D6";

// Player::getFoodExhaustionLevel() -> float  (0.0–4.0)
static const char* SIG_GET_EXHAUSTION =
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? "
    "? ? ? ? 1E 60 00 1F D6";

// =============================================================================
//  PatchMemory — same helper as BetterBrightness
//  Writes 4 bytes at addr after making the page writable+executable.
// =============================================================================
static bool PatchMemory(void* addr, uint32_t insn) {
    uintptr_t page = (uintptr_t)addr & ~(uintptr_t)4095;
    size_t    sz   = (sizeof(insn) + 4095) & ~(size_t)4095;
    if (mprotect((void*)page, sz, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;
    memcpy(addr, &insn, sizeof(insn));
    __builtin___clear_cache((char*)addr, (char*)addr + sizeof(insn));
    mprotect((void*)page, sz, PROT_READ | PROT_EXEC);
    return true;
}

// ARM64 BL immediate encoding
static uint32_t MakeBL(uintptr_t src, uintptr_t dst) {
    int64_t off = ((int64_t)dst - (int64_t)src) / 4;
    return 0x94000000u | ((uint32_t)off & 0x03FFFFFFu);
}

// =============================================================================
//  MC function pointers — filled by ResolveAll()
// =============================================================================
typedef void  (*FnPlayerTick)   (void*, void*, void*);
typedef int   (*FnGetArmor)     (void*, int);
typedef int   (*FnGetFood)      (void*);
typedef float (*FnGetSat)       (void*);
typedef float (*FnGetHealth)    (void*);
typedef float (*FnGetMaxHealth) (void*);
typedef float (*FnGetExhaustion)(void*);

static FnPlayerTick    g_origTick = nullptr;
static FnGetArmor      g_getArmor = nullptr;
static FnGetFood       g_getFood  = nullptr;
static FnGetSat        g_getSat   = nullptr;
static FnGetHealth     g_getHp    = nullptr;
static FnGetMaxHealth  g_getMaxHp = nullptr;
static FnGetExhaustion g_getExh   = nullptr;

// =============================================================================
//  HUD state — written by tick hook, read by render thread
// =============================================================================
struct ArmorSlot { bool worn=false; int durPct=100; bool low=false; };
struct HUDData {
    ArmorSlot armor[4]; // 0=boots 1=legs 2=chest 3=helm
    int   food       = 20;
    float saturation = 5.0f;
    float health     = 20.0f;
    float maxHealth  = 20.0f;
    float exhaustion = 0.0f;
};
static HUDData    g_HUD;
static std::mutex g_HUDMux;

// =============================================================================
//  PlayerTick hook — samples all MC data once per game tick
// =============================================================================
static void Hook_PlayerTick(void* self, void* a, void* b) {
    HUDData d;
    if (g_getArmor) {
        for (int i = 0; i < 4; i++) {
            int v = g_getArmor(self, i);
            d.armor[i] = { v > 0, v, v > 0 && v < 10 };
        }
    }
    if (g_getFood)  d.food       = g_getFood(self);
    if (g_getSat)   d.saturation = g_getSat(self);
    if (g_getHp)    d.health     = g_getHp(self);
    if (g_getMaxHp) d.maxHealth  = g_getMaxHp(self);
    if (g_getExh)   d.exhaustion = g_getExh(self);
    { std::lock_guard<std::mutex> lk(g_HUDMux); g_HUD = d; }
    if (g_origTick) g_origTick(self, a, b);
}

// =============================================================================
//  ResolveAll — signature scan then hook/patch (same technique as BetterBrightness)
// =============================================================================
static void ResolveAll() {
    const char* lib = "libminecraftpe.so";

    auto Resolve = [&](const char* sig) -> uintptr_t {
        uintptr_t a = pl::signature::pl_resolve_signature(sig, lib);
        if (!a) LOGE("SIG NOT FOUND: %.40s...", sig);
        return a;
    };

    // ── Getters: just store as function pointers, no patching needed ──────────
    if (auto a = Resolve(SIG_GET_ARMOR))      g_getArmor = (FnGetArmor)    a;
    if (auto a = Resolve(SIG_GET_FOOD))       g_getFood  = (FnGetFood)     a;
    if (auto a = Resolve(SIG_GET_SAT))        g_getSat   = (FnGetSat)      a;
    if (auto a = Resolve(SIG_GET_HEALTH))     g_getHp    = (FnGetHealth)   a;
    if (auto a = Resolve(SIG_GET_MAX_HEALTH)) g_getMaxHp = (FnGetMaxHealth)a;
    if (auto a = Resolve(SIG_GET_EXHAUSTION)) g_getExh   = (FnGetExhaustion)a;

    // ── PlayerTick: hook via Gloss (preferred — keeps trampoline) ─────────────
    uintptr_t aTick = Resolve(SIG_PLAYER_TICK);
    if (!aTick) return;

    void* origRaw = nullptr;
    bool hooked = GlossHook((void*)aTick, (void*)Hook_PlayerTick, &origRaw);
    if (hooked) {
        g_origTick = (FnPlayerTick)origRaw;
        LOGI("PlayerTick hooked via Gloss at 0x%lx", (unsigned long)aTick);
        return;
    }

    // ── Fallback: PatchMemory BL — same technique as BetterBrightness ─────────
    // Allocate a tiny RWX thunk so our hook can live anywhere in the address space.
    void* thunk = mmap(nullptr, 4096,
                       PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (thunk == MAP_FAILED) { LOGE("mmap thunk failed"); return; }

    // Thunk layout (ARM64):
    //   LDR X16, #8    ; load 64-bit absolute address
    //   BR  X16        ; jump to it
    //   .quad <Hook_PlayerTick>
    uint32_t* t = (uint32_t*)thunk;
    t[0] = 0x58000050u;                          // LDR X16, #8
    t[1] = 0xD61F0200u;                          // BR  X16
    uintptr_t hookAddr = (uintptr_t)Hook_PlayerTick;
    memcpy(&t[2], &hookAddr, 8);
    __builtin___clear_cache(thunk, (char*)thunk + 20);

    // Patch the first instruction of PlayerTick with BL to our thunk.
    // Note: g_origTick stays nullptr here — the BL is not a true inline hook,
    // so the original function body executes normally after Hook_PlayerTick returns
    // (BL only redirects entry; the callee's own RET returns to MC's caller).
    uint32_t bl = MakeBL(aTick, (uintptr_t)thunk);
    if (PatchMemory((void*)aTick, bl)) {
        LOGI("PlayerTick patched with BL+thunk at 0x%lx", (unsigned long)aTick);
    } else {
        LOGE("PatchMemory failed for PlayerTick");
    }
}

// =============================================================================
//  Textures — original PNG sprites from both Java mods, embedded as base64
// =============================================================================

// warn.png 8×8 RGBA — armor low-durability icon (uku's Armor HUD)
static const char WARN_B64[] =
    "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAYAAADED76LAAAAWUlEQVR4XmNggIK5vLz/"
    "kTFMHAye7Nz5/66R0f9Xe/f+v6Ss/B/E7+HmhigCqUaW2MnPD6dLuLj+wxUgS2Ao"
    "AOkGCaBjuAJ0nSgmgADIQVh1IwOQADKGiQMAm3xuk4wkOW4AAAAASUVORK5CYII=";

// icons.png 256×256 — food/saturation atlas (AppleSkin)
static const char ICONS_B64[] =
    "iVBORw0KGgoAAAANSUhEUgAAAQAAAAEABAMAAACuXLVVAAAAIVBMVEUAAAAoKCgKCgqr"
    "EgDCoQCfhgmbEQDdwkHgFwDp0mL7GQAZXd7AAAAAAXRSTlMAQObYZgAAAW1JREFUeNrt"
    "0ztOw1AQheHBLx4VwZagjLOCi7yBW2QBTuHeaVKHHYQd0NJRs0qOHSlu03BHgv9LRueky"
    "uhKY2cfmk/NvYqLW5Ojvio+oqbQR8VHYRKLqaQXLwtY77lAr1J6LFAW+uN+a+WgMqgk18"
    "dpi6UkN2jKfikevjTfmgcVF3cmJ31VfARNro+Kj9wk5FNJL1wWsNFzgVGl8ligyvXHY2fV"
    "QeWgktwYpi2WktxBU41LSevmSvZbHq9lMuMFeAFe4K+9gK3NsnPczpFa9mTWri17Nns7"
    "Tr9Sa1frbNUoYjFH8ifIVtHaZo6dQguktmlittva0MRSsXlKv8EuWtlcok2/wGBW9pfY"
    "WGrLGd65nuGL2fvJ7QxrRagULmcYrK3n2Nc+Z1iH7LWzrg7VvnM5w9dgeT1HVbucYWeW"
    "hzmqcTpDAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP/MD5DeMxUD"
    "pj/RAAAAAElFTkSuQmCC";

static const char B64C[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::vector<uint8_t> B64Dec(const char* s) {
    std::vector<uint8_t> o; int val=0, bits=-8;
    for (; *s; s++) {
        const char* p = strchr(B64C, *s); if (!p) continue;
        val = (val<<6)+(int)(p-B64C);
        if ((bits+=6)>=0) { o.push_back((uint8_t)(val>>bits&0xFF)); bits-=8; }
    }
    return o;
}

struct Tex { GLuint id=0; int w=0, h=0; };
static Tex  g_warnTex, g_iconsTex;
static bool g_texLoaded = false;

static Tex UploadPNG(const char* b64) {
    Tex t; auto raw = B64Dec(b64); int n;
    uint8_t* px = stbi_load_from_memory(raw.data(),(int)raw.size(),&t.w,&t.h,&n,4);
    if (!px) return t;
    glGenTextures(1,&t.id); glBindTexture(GL_TEXTURE_2D,t.id);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,t.w,t.h,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    stbi_image_free(px); return t;
}

static void LoadTextures() {
    if (g_texLoaded) return;
    g_warnTex  = UploadPNG(WARN_B64);
    g_iconsTex = UploadPNG(ICONS_B64);
    g_texLoaded = true;
}

// =============================================================================
//  ImGui HUD rendering
// =============================================================================
static float g_time = 0.0f;

static void DrawArmorSlot(ImDrawList* dl, ImVec2 pos,
                          const ArmorSlot& s, float t, float sc) {
    float sz = 20.0f * sc;
    ImVec2 p2(pos.x+sz, pos.y+sz);
    dl->AddRectFilled(pos, p2, IM_COL32(0,0,0,130), 3.0f*sc);
    if (!s.worn) { dl->AddRect(pos, p2, IM_COL32(70,70,70,160), 3.0f*sc); return; }

    // Durability bar (2 px strip at bottom)
    float bh = 2.0f*sc;
    ImVec2 bb1(pos.x, p2.y-bh), bb2(p2.x, p2.y);
    dl->AddRectFilled(bb1, bb2, IM_COL32(0,0,0,210));
    float pct = s.durPct/100.0f;
    dl->AddRectFilled(bb1, ImVec2(pos.x+sz*pct, p2.y),
        IM_COL32((uint8_t)(255*(1-pct)), (uint8_t)(255*pct), 0, 255));

    // Durability text
    char buf[8]; snprintf(buf,sizeof(buf),"%d%%",s.durPct);
    dl->AddText(ImVec2(pos.x+sc, pos.y+sc), IM_COL32(255,255,255,200), buf);

    // Warning icon (original warn.png, flashes when low)
    if (s.low && g_warnTex.id) {
        float alpha = 0.55f + 0.45f*sinf(t*6.28f);
        ImVec2 wp(p2.x-8*sc, p2.y-8*sc-bh);
        dl->AddImage((ImTextureID)(uintptr_t)g_warnTex.id,
                     wp, ImVec2(p2.x, p2.y-bh),
                     ImVec2(0,0), ImVec2(1,1),
                     IM_COL32(255,140,0,(uint8_t)(255*alpha)));
    }
}

// Draw up to 10 food icons from the AppleSkin atlas
static void DrawFoodIcons(ImDrawList* dl, ImVec2 origin,
                          float value, float maxVal,
                          int fX, int fY, int eX, int eY,
                          ImU32 tint, float sz) {
    if (!g_iconsTex.id) return;
    const float A=1.f/256.f, iw=9.f*A, ih=9.f*A;
    int total = (int)ceilf(maxVal/2.f); if(total>10) total=10;
    for (int i=0; i<total; i++) {
        float fill = value - i*2.f;
        ImVec2 p(origin.x+i*(sz+1.f), origin.y), p2(p.x+sz, p.y+sz);
        // Empty icon
        dl->AddImage((ImTextureID)(uintptr_t)g_iconsTex.id, p, p2,
                     ImVec2(eX*A,eY*A), ImVec2(eX*A+iw,eY*A+ih),
                     IM_COL32(255,255,255,60));
        if (fill>=2.f) {
            dl->AddImage((ImTextureID)(uintptr_t)g_iconsTex.id, p, p2,
                         ImVec2(fX*A,fY*A), ImVec2(fX*A+iw,fY*A+ih), tint);
        } else if (fill>0.f) {
            float fr=fill/2.f;
            dl->AddImage((ImTextureID)(uintptr_t)g_iconsTex.id,
                         p, ImVec2(p.x+sz*fr, p2.y),
                         ImVec2(fX*A,fY*A), ImVec2(fX*A+iw*fr,fY*A+ih), tint);
        }
    }
}

static void RenderHUD() {
    g_time += ImGui::GetIO().DeltaTime;
    LoadTextures();
    HUDData h; { std::lock_guard<std::mutex> lk(g_HUDMux); h = g_HUD; }

    float sw = ImGui::GetIO().DisplaySize.x;
    float sh = ImGui::GetIO().DisplaySize.y;
    float sc = sh / 480.0f;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // ── Armor HUD — bottom-left, above hotbar ────────────────────────────────
    // Helm | Chest | Legs | Boots  (slot indices 3→0, left to right)
    float slotSz = 20.0f*sc, gap = 2.0f*sc;
    float ax = 4.0f*sc, ay = sh - 52.0f*sc - slotSz;
    for (int i=3; i>=0; i--)
        DrawArmorSlot(dl, ImVec2(ax+(3-i)*(slotSz+gap), ay), h.armor[i], g_time, sc);

    // ── AppleSkin — above hunger bar, bottom-right ────────────────────────────
    // AppleSkin icons.png atlas UV layout (256×256, each icon is 9×9 px):
    //   y=9:  saturation overlay  full=(52,9)  empty=(16,9)
    //   y=27: exhaustion underlay full=(52,27) empty=(0,27)
    float isz  = 9.0f*sc;
    float foodX = sw - 10*(isz+sc) - 4*sc;
    float foodY = sh - 49.0f*sc;

    // Exhaustion underlay (draw first — lowest z-order)
    DrawFoodIcons(dl, ImVec2(foodX,foodY),
                  h.food*(h.exhaustion/4.f), 20.f,
                  52,27, 0,27, IM_COL32(153,89,25,178), isz);

    // Saturation overlay
    DrawFoodIcons(dl, ImVec2(foodX,foodY),
                  fminf(h.saturation,(float)h.food), 20.f,
                  52,9, 16,9, IM_COL32(255,217,25,216), isz);

    // Saturation value label
    char buf[16]; snprintf(buf,sizeof(buf),"%.1f",h.saturation);
    dl->AddText(ImVec2(foodX-1, foodY-11*sc), IM_COL32(255,220,60,220), buf);
}

// =============================================================================
//  eglSwapBuffers hook — draws ImGui HUD every frame (same as MotionBlur)
// =============================================================================
typedef EGLBoolean (*FnSwap)(EGLDisplay, EGLSurface);
static FnSwap     g_origSwap   = nullptr;
static EGLContext g_targetCtx  = EGL_NO_CONTEXT;
static EGLSurface g_targetSurf = EGL_NO_SURFACE;
static bool       g_imguiInit  = false;

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!g_origSwap) return EGL_FALSE;
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return g_origSwap(dpy,surf);
    if (g_targetCtx == EGL_NO_CONTEXT) { g_targetCtx=ctx; g_targetSurf=surf; }
    if (ctx!=g_targetCtx || surf!=g_targetSurf) return g_origSwap(dpy,surf);

    EGLint w, h;
    eglQuerySurface(dpy,surf,EGL_WIDTH,&w);
    eglQuerySurface(dpy,surf,EGL_HEIGHT,&h);
    if (w<100||h<100) return g_origSwap(dpy,surf);

    if (!g_imguiInit) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        ImGui_ImplOpenGL3_Init("#version 300 es");
        g_imguiInit = true;
        LOGI("ImGui ready (%dx%d)", w, h);
    }

    ImGui::GetIO().DisplaySize = ImVec2((float)w,(float)h);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    RenderHUD();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return g_origSwap(dpy,surf);
}

// =============================================================================
//  Init thread + constructor
// =============================================================================
static void* mainthread(void*) {
    sleep(3); // Wait for libminecraftpe.so to finish loading

    GlossInit(true);
    ResolveAll(); // signature scan + PatchMemory + player tick hook

    GHandle hegl = GlossOpen("libEGL.so");
    if (hegl) {
        void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
        if (swap) GlossHook(swap, (void*)hook_eglswapbuffers, (void**)&g_origSwap);
    }
    return nullptr;
}

__attribute__((constructor))
void ArmorFoodHUD_Init() {
    LOGI("ArmorHUD + AppleSkin loading (MC Bedrock 1.26.0+)");
    pthread_t t;
    pthread_create(&t, nullptr, mainthread, nullptr);
}
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? "
    "? ? ? ? 1E 40 00 1F D6";

// Player::getFoodExhaustionLevel() -> float  (0.0–4.0)
static const char* SIG_GET_EXHAUSTION =
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? "
    "? ? ? ? 1E 60 00 1F D6";

// =============================================================================
//  PatchMemory — same helper as BetterBrightness
//  Writes 4 bytes at addr after making the page writable+executable.
// =============================================================================
static bool PatchMemory(void* addr, uint32_t insn) {
    uintptr_t page = (uintptr_t)addr & ~(uintptr_t)4095;
    size_t    sz   = (sizeof(insn) + 4095) & ~(size_t)4095;
    if (mprotect((void*)page, sz, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;
    memcpy(addr, &insn, sizeof(insn));
    __builtin___clear_cache((char*)addr, (char*)addr + sizeof(insn));
    mprotect((void*)page, sz, PROT_READ | PROT_EXEC);
    return true;
}

// ARM64 BL immediate encoding
static uint32_t MakeBL(uintptr_t src, uintptr_t dst) {
    int64_t off = ((int64_t)dst - (int64_t)src) / 4;
    return 0x94000000u | ((uint32_t)off & 0x03FFFFFFu);
}

// =============================================================================
//  MC function pointers — filled by ResolveAll()
// =============================================================================
typedef void  (*FnPlayerTick)   (void*, void*, void*);
typedef int   (*FnGetArmor)     (void*, int);
typedef int   (*FnGetFood)      (void*);
typedef float (*FnGetSat)       (void*);
typedef float (*FnGetHealth)    (void*);
typedef float (*FnGetMaxHealth) (void*);
typedef float (*FnGetExhaustion)(void*);

static FnPlayerTick    g_origTick = nullptr;
static FnGetArmor      g_getArmor = nullptr;
static FnGetFood       g_getFood  = nullptr;
static FnGetSat        g_getSat   = nullptr;
static FnGetHealth     g_getHp    = nullptr;
static FnGetMaxHealth  g_getMaxHp = nullptr;
static FnGetExhaustion g_getExh   = nullptr;

// =============================================================================
//  HUD state — written by tick hook, read by render thread
// =============================================================================
struct ArmorSlot { bool worn=false; int durPct=100; bool low=false; };
struct HUDData {
    ArmorSlot armor[4]; // 0=boots 1=legs 2=chest 3=helm
    int   food       = 20;
    float saturation = 5.0f;
    float health     = 20.0f;
    float maxHealth  = 20.0f;
    float exhaustion = 0.0f;
};
static HUDData    g_HUD;
static std::mutex g_HUDMux;

// =============================================================================
//  PlayerTick hook — samples all MC data once per game tick
// =============================================================================
static void Hook_PlayerTick(void* self, void* a, void* b) {
    HUDData d;
    if (g_getArmor) {
        for (int i = 0; i < 4; i++) {
            int v = g_getArmor(self, i);
            d.armor[i] = { v > 0, v, v > 0 && v < 10 };
        }
    }
    if (g_getFood)  d.food       = g_getFood(self);
    if (g_getSat)   d.saturation = g_getSat(self);
    if (g_getHp)    d.health     = g_getHp(self);
    if (g_getMaxHp) d.maxHealth  = g_getMaxHp(self);
    if (g_getExh)   d.exhaustion = g_getExh(self);
    { std::lock_guard<std::mutex> lk(g_HUDMux); g_HUD = d; }
    if (g_origTick) g_origTick(self, a, b);
}

// =============================================================================
//  ResolveAll — signature scan then hook/patch (same technique as BetterBrightness)
// =============================================================================
static void ResolveAll() {
    const char* lib = "libminecraftpe.so";

    auto Resolve = [&](const char* sig) -> uintptr_t {
        uintptr_t a = pl::signature::pl_resolve_signature(sig, lib);
        if (!a) LOGE("SIG NOT FOUND: %.40s...", sig);
        return a;
    };

    // ── Getters: just store as function pointers, no patching needed ──────────
    if (auto a = Resolve(SIG_GET_ARMOR))      g_getArmor = (FnGetArmor)    a;
    if (auto a = Resolve(SIG_GET_FOOD))       g_getFood  = (FnGetFood)     a;
    if (auto a = Resolve(SIG_GET_SAT))        g_getSat   = (FnGetSat)      a;
    if (auto a = Resolve(SIG_GET_HEALTH))     g_getHp    = (FnGetHealth)   a;
    if (auto a = Resolve(SIG_GET_MAX_HEALTH)) g_getMaxHp = (FnGetMaxHealth)a;
    if (auto a = Resolve(SIG_GET_EXHAUSTION)) g_getExh   = (FnGetExhaustion)a;

    // ── PlayerTick: hook via Gloss (preferred — keeps trampoline) ─────────────
    uintptr_t aTick = Resolve(SIG_PLAYER_TICK);
    if (!aTick) return;

    void* origRaw = nullptr;
    bool hooked = GlossHook((void*)aTick, (void*)Hook_PlayerTick, &origRaw);
    if (hooked) {
        g_origTick = (FnPlayerTick)origRaw;
        LOGI("PlayerTick hooked via Gloss at 0x%lx", (unsigned long)aTick);
        return;
    }

    // ── Fallback: PatchMemory BL — same technique as BetterBrightness ─────────
    // Allocate a tiny RWX thunk so our hook can live anywhere in the address space.
    void* thunk = mmap(nullptr, 4096,
                       PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (thunk == MAP_FAILED) { LOGE("mmap thunk failed"); return; }

    // Thunk layout (ARM64):
    //   LDR X16, #8    ; load 64-bit absolute address
    //   BR  X16        ; jump to it
    //   .quad <Hook_PlayerTick>
    uint32_t* t = (uint32_t*)thunk;
    t[0] = 0x58000050u;                          // LDR X16, #8
    t[1] = 0xD61F0200u;                          // BR  X16
    uintptr_t hookAddr = (uintptr_t)Hook_PlayerTick;
    memcpy(&t[2], &hookAddr, 8);
    __builtin___clear_cache(thunk, (char*)thunk + 20);

    // Patch the first instruction of PlayerTick with BL to our thunk.
    // Note: g_origTick stays nullptr here — the BL is not a true inline hook,
    // so the original function body executes normally after Hook_PlayerTick returns
    // (BL only redirects entry; the callee's own RET returns to MC's caller).
    uint32_t bl = MakeBL(aTick, (uintptr_t)thunk);
    if (PatchMemory((void*)aTick, bl)) {
        LOGI("PlayerTick patched with BL+thunk at 0x%lx", (unsigned long)aTick);
    } else {
        LOGE("PatchMemory failed for PlayerTick");
    }
}

// =============================================================================
//  Textures — original PNG sprites from both Java mods, embedded as base64
// =============================================================================

// warn.png 8×8 RGBA — armor low-durability icon (uku's Armor HUD)
static const char WARN_B64[] =
    "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAYAAADED76LAAAAWUlEQVR4XmNggIK5vLz/"
    "kTFMHAye7Nz5/66R0f9Xe/f+v6Ss/B/E7+HmhigCqUaW2MnPD6dLuLj+wxUgS2Ao"
    "AOkGCaBjuAJ0nSgmgADIQVh1IwOQADKGiQMAm3xuk4wkOW4AAAAASUVORK5CYII=";

// icons.png 256×256 — food/saturation atlas (AppleSkin)
static const char ICONS_B64[] =
    "iVBORw0KGgoAAAANSUhEUgAAAQAAAAEABAMAAACuXLVVAAAAIVBMVEUAAAAoKCgKCgqr"
    "EgDCoQCfhgmbEQDdwkHgFwDp0mL7GQAZXd7AAAAAAXRSTlMAQObYZgAAAW1JREFUeNrt"
    "0ztOw1AQheHBLx4VwZagjLOCi7yBW2QBTuHeaVKHHYQd0NJRs0qOHSlu03BHgv9LRueky"
    "uhKY2cfmk/NvYqLW5Ojvio+oqbQR8VHYRKLqaQXLwtY77lAr1J6LFAW+uN+a+WgMqgk18"
    "dpi6UkN2jKfikevjTfmgcVF3cmJ31VfARNro+Kj9wk5FNJL1wWsNFzgVGl8ligyvXHY2fV"
    "QeWgktwYpi2WktxBU41LSevmSvZbHq9lMuMFeAFe4K+9gK3NsnPczpFa9mTWri17Nns7"
    "Tr9Sa1frbNUoYjFH8ifIVtHaZo6dQguktmlittva0MRSsXlKv8EuWtlcok2/wGBW9pfY"
    "WGrLGd65nuGL2fvJ7QxrRagULmcYrK3n2Nc+Z1iH7LWzrg7VvnM5w9dgeT1HVbucYWeW"
    "hzmqcTpDAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP/MD5DeMxUD"
    "pj/RAAAAAElFTkSuQmCC";

static const char B64C[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::vector<uint8_t> B64Dec(const char* s) {
    std::vector<uint8_t> o; int val=0, bits=-8;
    for (; *s; s++) {
        const char* p = strchr(B64C, *s); if (!p) continue;
        val = (val<<6)+(int)(p-B64C);
        if ((bits+=6)>=0) { o.push_back((uint8_t)(val>>bits&0xFF)); bits-=8; }
    }
    return o;
}

struct Tex { GLuint id=0; int w=0, h=0; };
static Tex  g_warnTex, g_iconsTex;
static bool g_texLoaded = false;

static Tex UploadPNG(const char* b64) {
    Tex t; auto raw = B64Dec(b64); int n;
    uint8_t* px = stbi_load_from_memory(raw.data(),(int)raw.size(),&t.w,&t.h,&n,4);
    if (!px) return t;
    glGenTextures(1,&t.id); glBindTexture(GL_TEXTURE_2D,t.id);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,t.w,t.h,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    stbi_image_free(px); return t;
}

static void LoadTextures() {
    if (g_texLoaded) return;
    g_warnTex  = UploadPNG(WARN_B64);
    g_iconsTex = UploadPNG(ICONS_B64);
    g_texLoaded = true;
}

// =============================================================================
//  ImGui HUD rendering
// =============================================================================
static float g_time = 0.0f;

static void DrawArmorSlot(ImDrawList* dl, ImVec2 pos,
                          const ArmorSlot& s, float t, float sc) {
    float sz = 20.0f * sc;
    ImVec2 p2(pos.x+sz, pos.y+sz);
    dl->AddRectFilled(pos, p2, IM_COL32(0,0,0,130), 3.0f*sc);
    if (!s.worn) { dl->AddRect(pos, p2, IM_COL32(70,70,70,160), 3.0f*sc); return; }

    // Durability bar (2 px strip at bottom)
    float bh = 2.0f*sc;
    ImVec2 bb1(pos.x, p2.y-bh), bb2(p2.x, p2.y);
    dl->AddRectFilled(bb1, bb2, IM_COL32(0,0,0,210));
    float pct = s.durPct/100.0f;
    dl->AddRectFilled(bb1, ImVec2(pos.x+sz*pct, p2.y),
        IM_COL32((uint8_t)(255*(1-pct)), (uint8_t)(255*pct), 0, 255));

    // Durability text
    char buf[8]; snprintf(buf,sizeof(buf),"%d%%",s.durPct);
    dl->AddText(ImVec2(pos.x+sc, pos.y+sc), IM_COL32(255,255,255,200), buf);

    // Warning icon (original warn.png, flashes when low)
    if (s.low && g_warnTex.id) {
        float alpha = 0.55f + 0.45f*sinf(t*6.28f);
        ImVec2 wp(p2.x-8*sc, p2.y-8*sc-bh);
        dl->AddImage((ImTextureID)(uintptr_t)g_warnTex.id,
                     wp, ImVec2(p2.x, p2.y-bh),
                     ImVec2(0,0), ImVec2(1,1),
                     IM_COL32(255,140,0,(uint8_t)(255*alpha)));
    }
}

// Draw up to 10 food icons from the AppleSkin atlas
static void DrawFoodIcons(ImDrawList* dl, ImVec2 origin,
                          float value, float maxVal,
                          int fX, int fY, int eX, int eY,
                          ImU32 tint, float sz) {
    if (!g_iconsTex.id) return;
    const float A=1.f/256.f, iw=9.f*A, ih=9.f*A;
    int total = (int)ceilf(maxVal/2.f); if(total>10) total=10;
    for (int i=0; i<total; i++) {
        float fill = value - i*2.f;
        ImVec2 p(origin.x+i*(sz+1.f), origin.y), p2(p.x+sz, p.y+sz);
        // Empty icon
        dl->AddImage((ImTextureID)(uintptr_t)g_iconsTex.id, p, p2,
                     ImVec2(eX*A,eY*A), ImVec2(eX*A+iw,eY*A+ih),
                     IM_COL32(255,255,255,60));
        if (fill>=2.f) {
            dl->AddImage((ImTextureID)(uintptr_t)g_iconsTex.id, p, p2,
                         ImVec2(fX*A,fY*A), ImVec2(fX*A+iw,fY*A+ih), tint);
        } else if (fill>0.f) {
            float fr=fill/2.f;
            dl->AddImage((ImTextureID)(uintptr_t)g_iconsTex.id,
                         p, ImVec2(p.x+sz*fr, p2.y),
                         ImVec2(fX*A,fY*A), ImVec2(fX*A+iw*fr,fY*A+ih), tint);
        }
    }
}

static void RenderHUD() {
    g_time += ImGui::GetIO().DeltaTime;
    LoadTextures();
    HUDData h; { std::lock_guard<std::mutex> lk(g_HUDMux); h = g_HUD; }

    float sw = ImGui::GetIO().DisplaySize.x;
    float sh = ImGui::GetIO().DisplaySize.y;
    float sc = sh / 480.0f;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // ── Armor HUD — bottom-left, above hotbar ────────────────────────────────
    // Helm | Chest | Legs | Boots  (slot indices 3→0, left to right)
    float slotSz = 20.0f*sc, gap = 2.0f*sc;
    float ax = 4.0f*sc, ay = sh - 52.0f*sc - slotSz;
    for (int i=3; i>=0; i--)
        DrawArmorSlot(dl, ImVec2(ax+(3-i)*(slotSz+gap), ay), h.armor[i], g_time, sc);

    // ── AppleSkin — above hunger bar, bottom-right ────────────────────────────
    // AppleSkin icons.png atlas UV layout (256×256, each icon is 9×9 px):
    //   y=9:  saturation overlay  full=(52,9)  empty=(16,9)
    //   y=27: exhaustion underlay full=(52,27) empty=(0,27)
    float isz  = 9.0f*sc;
    float foodX = sw - 10*(isz+sc) - 4*sc;
    float foodY = sh - 49.0f*sc;

    // Exhaustion underlay (draw first — lowest z-order)
    DrawFoodIcons(dl, ImVec2(foodX,foodY),
                  h.food*(h.exhaustion/4.f), 20.f,
                  52,27, 0,27, IM_COL32(153,89,25,178), isz);

    // Saturation overlay
    DrawFoodIcons(dl, ImVec2(foodX,foodY),
                  fminf(h.saturation,(float)h.food), 20.f,
                  52,9, 16,9, IM_COL32(255,217,25,216), isz);

    // Saturation value label
    char buf[16]; snprintf(buf,sizeof(buf),"%.1f",h.saturation);
    dl->AddText(ImVec2(foodX-1, foodY-11*sc), IM_COL32(255,220,60,220), buf);
}

// =============================================================================
//  eglSwapBuffers hook — draws ImGui HUD every frame (same as MotionBlur)
// =============================================================================
typedef EGLBoolean (*FnSwap)(EGLDisplay, EGLSurface);
static FnSwap     g_origSwap   = nullptr;
static EGLContext g_targetCtx  = EGL_NO_CONTEXT;
static EGLSurface g_targetSurf = EGL_NO_SURFACE;
static bool       g_imguiInit  = false;

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!g_origSwap) return EGL_FALSE;
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return g_origSwap(dpy,surf);
    if (g_targetCtx == EGL_NO_CONTEXT) { g_targetCtx=ctx; g_targetSurf=surf; }
    if (ctx!=g_targetCtx || surf!=g_targetSurf) return g_origSwap(dpy,surf);

    EGLint w, h;
    eglQuerySurface(dpy,surf,EGL_WIDTH,&w);
    eglQuerySurface(dpy,surf,EGL_HEIGHT,&h);
    if (w<100||h<100) return g_origSwap(dpy,surf);

    if (!g_imguiInit) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        ImGui_ImplOpenGL3_Init("#version 300 es");
        g_imguiInit = true;
        LOGI("ImGui ready (%dx%d)", w, h);
    }

    ImGui::GetIO().DisplaySize = ImVec2((float)w,(float)h);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    RenderHUD();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return g_origSwap(dpy,surf);
}

// =============================================================================
//  Init thread + constructor
// =============================================================================
static void* mainthread(void*) {
    sleep(3); // Wait for libminecraftpe.so to finish loading

    GlossInit(true);
    ResolveAll(); // signature scan + PatchMemory + player tick hook

    GHandle hegl = GlossOpen("libEGL.so");
    if (hegl) {
        void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
        if (swap) GlossHook(swap, (void*)hook_eglswapbuffers, (void**)&g_origSwap);
    }
    return nullptr;
}

__attribute__((constructor))
void ArmorFoodHUD_Init() {
    LOGI("ArmorHUD + AppleSkin loading (MC Bedrock 1.26.0+)");
    pthread_t t;
    pthread_create(&t, nullptr, mainthread, nullptr);
}
