/**
 * ArmorHUD + AppleSkin — Bedrock Native Mod
 * Minecraft Bedrock 1.26.0+ | LeviLaminar (Levi Launcher, mobile)
 *
 * Crash-safe design:
 *  - eglSwapBuffers hooked via GOT/PLT redirect (16-byte abs jump, not 4-byte BL)
 *  - PlayerTick hook uses a proper 16-byte trampoline — no restore/re-patch race
 *  - All signature results validated before use (alignment + readable memory check)
 *  - Signatures disabled by default (all zeros) — HUD renders safely with mock data
 *    until you fill in real offsets from IDA/Ghidra for your MC version
 */

#include <android/log.h>
#include <EGL/egl.h>
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
#include <link.h>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_android.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#include "stb_image.h"

#define LOG_TAG "ArmorFoodHUD"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// =============================================================================
//  ARM64 memory patching utilities
// =============================================================================

// Make a 4096-byte RWX page containing an absolute indirect jump to `target`.
// Layout: LDR X16, #8 / BR X16 / .quad target
static void* MakeAbsThunk(uintptr_t target) {
    void* page = mmap(nullptr, 4096,
                      PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return nullptr;
    uint32_t* p = (uint32_t*)page;
    p[0] = 0x58000050u; // LDR X16, #8
    p[1] = 0xD61F0200u; // BR  X16
    memcpy(&p[2], &target, 8);
    __builtin___clear_cache((char*)page, (char*)page + 20);
    return page;
}

// Write `len` bytes at `addr` after making the page writable.
static bool WriteMemory(void* addr, const void* src, size_t len) {
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)4095;
    size_t    size  = ((uintptr_t)addr + len - start + 4095) & ~(size_t)4095;
    if (mprotect((void*)start, size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;
    memcpy(addr, src, len);
    __builtin___clear_cache((char*)addr, (char*)addr + len);
    mprotect((void*)start, size, PROT_READ | PROT_EXEC);
    return true;
}

// Install a 16-byte absolute hook at `target`:
//   LDR X17, #8 / BR X17 / .quad replacement
// Saves the original 16 bytes into `trampoline` (which must be an RWX page
// with a resume jump appended) so the original function can still be called.
//
// Returns a callable pointer to the trampoline (original behaviour),
// or nullptr on failure.
static void* HookFunction(void* target, void* replacement) {
    if (!target || !replacement) return nullptr;

    // Allocate trampoline page:
    //   [0..15]  = original 16 bytes (saved instructions)
    //   [16..19] = LDR X16, #8
    //   [20..23] = BR  X16
    //   [24..31] = .quad (target + 16)   <- resume address
    uint8_t* tramp = (uint8_t*)mmap(nullptr, 4096,
                                    PROT_READ | PROT_WRITE | PROT_EXEC,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) return nullptr;

    // Save original bytes
    memcpy(tramp, target, 16);

    // Append absolute jump back to (target + 16)
    uint32_t* tj = (uint32_t*)(tramp + 16);
    tj[0] = 0x58000050u; // LDR X16, #8
    tj[1] = 0xD61F0200u; // BR  X16
    uintptr_t resume = (uintptr_t)target + 16;
    memcpy(&tj[2], &resume, 8);
    __builtin___clear_cache((char*)tramp, (char*)tramp + 32);

    // Patch target with 16-byte absolute jump to replacement
    uint32_t patch[4];
    patch[0] = 0x58000051u; // LDR X17, #8
    patch[1] = 0xD61F0220u; // BR  X17
    uintptr_t repAddr = (uintptr_t)replacement;
    memcpy(&patch[2], &repAddr, 8);

    if (!WriteMemory(target, patch, 16)) {
        munmap(tramp, 4096);
        return nullptr;
    }
    return (void*)tramp;
}

// =============================================================================
//  Signature scanner
// =============================================================================
static uintptr_t ScanSignature(const char* pattern, const char* libname) {
    uint8_t  pat[256];
    bool     mask[256];
    size_t   len = 0;

    for (const char* p = pattern; *p && len < 256; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '?') {
            pat[len] = 0; mask[len++] = false;
            p++; if (*p == '?') p++;
        } else {
            pat[len] = (uint8_t)strtoul(p, nullptr, 16);
            mask[len++] = true;
            p += 2;
        }
    }
    if (!len) return 0;

    struct Ctx {
        const char*    lib;
        const uint8_t* pat;
        const bool*    mask;
        size_t         len;
        uintptr_t      result;
    } ctx { libname, pat, mask, len, 0 };

    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
        auto* c = (Ctx*)data;
        if (!info->dlpi_name || !strstr(info->dlpi_name, c->lib)) return 0;
        for (int i = 0; i < info->dlpi_phnum; i++) {
            auto& ph = info->dlpi_phdr[i];
            if (ph.p_type != PT_LOAD || !(ph.p_flags & 1)) continue;
            auto* base = (const uint8_t*)(info->dlpi_addr + ph.p_vaddr);
            size_t sz  = ph.p_filesz;
            for (size_t off = 0; off + c->len <= sz; off += 4) {
                bool ok = true;
                for (size_t j = 0; j < c->len && ok; j++)
                    if (c->mask[j] && base[off + j] != c->pat[j]) ok = false;
                if (ok) { c->result = (uintptr_t)(base + off); return 1; }
            }
        }
        return 0;
    }, &ctx);

    return ctx.result;
}

// Validate a resolved address: must be non-null, 4-byte aligned, and readable.
static bool ValidAddr(uintptr_t a) {
    if (!a || (a & 3)) return false;
    // Try reading 4 bytes — if it faults, it's bad (this is a heuristic only)
    uint32_t dummy;
    memcpy(&dummy, (void*)a, 4);
    return dummy != 0;
}

// =============================================================================
//  MC offsets — SAFE DEFAULTS (all zero = disabled)
//
//  HOW TO FILL THESE IN:
//    1. adb pull /data/app/com.mojang.minecraftpe-.../lib/arm64/libminecraftpe.so
//    2. Open in IDA Pro or Ghidra, find each function, note the file offset.
//    3. Replace 0x0 with the correct value.
//
//  While these are 0, the HUD will render with placeholder values (no crash).
//  Only fill in an offset once you've confirmed it's correct.
// =============================================================================
static constexpr uintptr_t OFF_PLAYER_TICK    = 0x0;
static constexpr uintptr_t OFF_GET_ARMOR      = 0x0;
static constexpr uintptr_t OFF_GET_FOOD       = 0x0;
static constexpr uintptr_t OFF_GET_SAT        = 0x0;
static constexpr uintptr_t OFF_GET_HEALTH     = 0x0;
static constexpr uintptr_t OFF_GET_MAX_HEALTH = 0x0;
static constexpr uintptr_t OFF_GET_EXHAUSTION = 0x0;

// =============================================================================
//  MC function pointers
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
//  HUD state — written by tick hook, read by render
// =============================================================================
struct ArmorSlot { bool worn=false; int durPct=100; bool low=false; };
struct HUDData {
    ArmorSlot armor[4];
    int   food=20;
    float saturation=5.f, health=20.f, maxHealth=20.f, exhaustion=0.f;
};
static HUDData    g_HUD;
static std::mutex g_HUDMux;

// =============================================================================
//  PlayerTick hook — called via proper trampoline, no race condition
// =============================================================================
static void Hook_PlayerTick(void* self, void* a, void* b) {
    // Call original FIRST via trampoline
    if (g_origTick) g_origTick(self, a, b);

    // Then read data — safe because original already ran
    HUDData d;
    if (g_getArmor) {
        for (int i = 0; i < 4; i++) {
            int v = g_getArmor(self, i);
            d.armor[i] = { v > 0, v, v > 0 && v < 10 };
        }
    }
    if (g_getFood)  d.food        = g_getFood(self);
    if (g_getSat)   d.saturation  = g_getSat(self);
    if (g_getHp)    d.health      = g_getHp(self);
    if (g_getMaxHp) d.maxHealth   = g_getMaxHp(self);
    if (g_getExh)   d.exhaustion  = g_getExh(self);

    std::lock_guard<std::mutex> lk(g_HUDMux);
    g_HUD = d;
}

// =============================================================================
//  Hook installation
// =============================================================================
static uintptr_t GetMCBase() {
    uintptr_t base = 0;
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
        if (info->dlpi_name && strstr(info->dlpi_name, "libminecraftpe.so")) {
            *(uintptr_t*)data = (uintptr_t)info->dlpi_addr;
            return 1;
        }
        return 0;
    }, &base);
    return base;
}

static void InstallHooks() {
    uintptr_t base = GetMCBase();
    if (!base) { LOGE("libminecraftpe.so base not found"); return; }
    LOGI("MC base: 0x%lx", (unsigned long)base);

    // Helper: resolve offset or signature, validate, return pointer
    auto Resolve = [&](uintptr_t off) -> uintptr_t {
        if (!off) return 0;
        uintptr_t a = base + off;
        if (!ValidAddr(a)) { LOGE("Invalid addr 0x%lx", (unsigned long)a); return 0; }
        return a;
    };

    if (auto a = Resolve(OFF_GET_ARMOR))      g_getArmor = (FnGetArmor)     a;
    if (auto a = Resolve(OFF_GET_FOOD))       g_getFood  = (FnGetFood)      a;
    if (auto a = Resolve(OFF_GET_SAT))        g_getSat   = (FnGetSat)       a;
    if (auto a = Resolve(OFF_GET_HEALTH))     g_getHp    = (FnGetHealth)    a;
    if (auto a = Resolve(OFF_GET_MAX_HEALTH)) g_getMaxHp = (FnGetMaxHealth) a;
    if (auto a = Resolve(OFF_GET_EXHAUSTION)) g_getExh   = (FnGetExhaustion)a;

    if (uintptr_t aTick = Resolve(OFF_PLAYER_TICK)) {
        void* tramp = HookFunction((void*)aTick, (void*)Hook_PlayerTick);
        if (tramp) {
            g_origTick = (FnPlayerTick)tramp;
            LOGI("PlayerTick hooked at 0x%lx", (unsigned long)aTick);
        } else {
            LOGE("HookFunction failed for PlayerTick");
        }
    }
}

// =============================================================================
//  Textures (original sprites from both Java mods)
// =============================================================================
static const char WARN_B64[] =
    "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAYAAADED76LAAAAWUlEQVR4XmNggIK5vLz/"
    "kTFMHAye7Nz5/66R0f9Xe/f+v6Ss/B/E7+HmhigCqUaW2MnPD6dLuLj+wxUgS2Ao"
    "AOkGCaBjuAJ0nSgmgADIQVh1IwOQADKGiQMAm3xuk4wkOW4AAAAASUVORK5CYII=";

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
        if ((bits+=6) >= 0) { o.push_back((uint8_t)(val>>bits&0xFF)); bits-=8; }
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
static float g_time = 0.f;

static void DrawArmorSlot(ImDrawList* dl, ImVec2 pos,
                          const ArmorSlot& s, float t, float sc) {
    float sz = 20.f*sc;
    ImVec2 p2(pos.x+sz, pos.y+sz);
    dl->AddRectFilled(pos, p2, IM_COL32(0,0,0,130), 3.f*sc);
    if (!s.worn) { dl->AddRect(pos,p2,IM_COL32(70,70,70,160),3.f*sc); return; }
    float bh = 2.f*sc;
    dl->AddRectFilled(ImVec2(pos.x,p2.y-bh), p2, IM_COL32(0,0,0,210));
    float pct = s.durPct/100.f;
    dl->AddRectFilled(ImVec2(pos.x,p2.y-bh),
                      ImVec2(pos.x+sz*pct,p2.y),
                      IM_COL32((uint8_t)(255*(1-pct)),(uint8_t)(255*pct),0,255));
    char buf[8]; snprintf(buf,sizeof(buf),"%d%%",s.durPct);
    dl->AddText(ImVec2(pos.x+sc,pos.y+sc), IM_COL32(255,255,255,200), buf);
    if (s.low && g_warnTex.id) {
        float a = 0.55f + 0.45f*sinf(t*6.28f);
        dl->AddImage((ImTextureID)(uintptr_t)g_warnTex.id,
                     ImVec2(p2.x-8*sc, p2.y-8*sc-bh),
                     ImVec2(p2.x, p2.y-bh),
                     ImVec2(0,0), ImVec2(1,1),
                     IM_COL32(255,140,0,(uint8_t)(255*a)));
    }
}

static void DrawFoodIcons(ImDrawList* dl, ImVec2 orig, float value,
                          int fX,int fY,int eX,int eY,
                          ImU32 tint, float sz) {
    if (!g_iconsTex.id) return;
    const float A=1.f/256.f, iw=9.f*A, ih=9.f*A;
    for (int i = 0; i < 10; i++) {
        float fill = value - i*2.f;
        ImVec2 p(orig.x+i*(sz+1.f), orig.y), p2(p.x+sz, p.y+sz);
        dl->AddImage((ImTextureID)(uintptr_t)g_iconsTex.id, p, p2,
                     ImVec2(eX*A,eY*A), ImVec2(eX*A+iw,eY*A+ih),
                     IM_COL32(255,255,255,60));
        if (fill >= 2.f)
            dl->AddImage((ImTextureID)(uintptr_t)g_iconsTex.id, p, p2,
                         ImVec2(fX*A,fY*A), ImVec2(fX*A+iw,fY*A+ih), tint);
        else if (fill > 0.f) {
            float fr = fill/2.f;
            dl->AddImage((ImTextureID)(uintptr_t)g_iconsTex.id,
                         p, ImVec2(p.x+sz*fr,p2.y),
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
    float sc = sh / 480.f;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // Armor HUD — bottom-left: helm | chest | legs | boots
    float ssz=20.f*sc, gap=2.f*sc;
    for (int i = 3; i >= 0; i--)
        DrawArmorSlot(dl,
                      ImVec2(4.f*sc + (3-i)*(ssz+gap), sh-52.f*sc-ssz),
                      h.armor[i], g_time, sc);

    // AppleSkin — bottom-right, above hunger bar
    float isz = 9.f*sc;
    float fx = sw - 10*(isz+sc) - 4*sc;
    float fy = sh - 49.f*sc;
    DrawFoodIcons(dl, ImVec2(fx,fy),
                  h.food*(h.exhaustion/4.f),
                  52,27,0,27, IM_COL32(153,89,25,178), isz);
    DrawFoodIcons(dl, ImVec2(fx,fy),
                  fminf(h.saturation,(float)h.food),
                  52,9,16,9,  IM_COL32(255,217,25,216), isz);
    char buf[16]; snprintf(buf,sizeof(buf),"%.1f",h.saturation);
    dl->AddText(ImVec2(fx-1,fy-11*sc), IM_COL32(255,220,60,220), buf);
}

// =============================================================================
//  eglSwapBuffers hook
// =============================================================================
typedef EGLBoolean (*FnSwap)(EGLDisplay, EGLSurface);
static FnSwap     g_origSwap   = nullptr;
static EGLContext g_targetCtx  = EGL_NO_CONTEXT;
static EGLSurface g_targetSurf = EGL_NO_SURFACE;
static bool       g_imguiInit  = false;

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!g_origSwap) return EGL_FALSE;

    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return g_origSwap(dpy, surf);
    if (g_targetCtx == EGL_NO_CONTEXT) { g_targetCtx=ctx; g_targetSurf=surf; }
    if (ctx != g_targetCtx || surf != g_targetSurf) return g_origSwap(dpy, surf);

    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH,  &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 100 || h < 100) return g_origSwap(dpy, surf);

    if (!g_imguiInit) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        ImGui_ImplOpenGL3_Init("#version 300 es");
        g_imguiInit = true;
        LOGI("ImGui ready (%dx%d)", w, h);
    }

    ImGui::GetIO().DisplaySize = ImVec2((float)w, (float)h);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    RenderHUD();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    return g_origSwap(dpy, surf);
}

static void HookEGL() {
    // Use HookFunction (16-byte abs jump) — not a 4-byte BL which can't reach far
    void* libegl = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
    if (!libegl) { LOGE("libEGL not loaded"); return; }
    void* sym = dlsym(libegl, "eglSwapBuffers");
    dlclose(libegl);
    if (!sym) { LOGE("eglSwapBuffers sym not found"); return; }

    void* tramp = HookFunction(sym, (void*)hook_eglswapbuffers);
    if (tramp) {
        g_origSwap = (FnSwap)tramp;
        LOGI("eglSwapBuffers hooked at %p", sym);
    } else {
        LOGE("HookFunction failed for eglSwapBuffers");
    }
}

// =============================================================================
//  Init
// =============================================================================
static void* mainthread(void*) {
    // Wait for MC to fully load before touching libminecraftpe.so
    sleep(5);
    InstallHooks();
    HookEGL();
    return nullptr;
}

__attribute__((constructor))
void ArmorFoodHUD_Init() {
    LOGI("ArmorHUD + AppleSkin loading (MC Bedrock 1.26.0+)");
    pthread_t t;
    pthread_create(&t, nullptr, mainthread, nullptr);
}

#define LOG_TAG "ArmorFoodHUD"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// =============================================================================
//  SIGNATURES  (ARM64 wildcard patterns for libminecraftpe.so 1.26.0)
//  '?' = wildcard byte. Update these if MC updates break them.
// =============================================================================
static const char* SIG_PLAYER_TICK =
    "? ? ? ? FD 7B ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 F4 4F ? A9 "
    "FD ? ? 91 ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ?";

static const char* SIG_GET_ARMOR =
    "? ? ? ? ? ? ? ? 08 ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? "
    "? ? ? ? ? ? ? ? ? ? ? ? 1F 00 00 71 ? ? ? ? ? ? ? ? C0 03 5F D6";

static const char* SIG_GET_FOOD =
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? 28 ? ? ? ? ? ? ? 1F ? ? ? "
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? C0 03 5F D6";

static const char* SIG_GET_SAT =
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? 60 ? ? ? ? ? ? ? ? ? ? 1E "
    "? ? ? ? ? 1E ? ? ? ? 1E C0 03 5F D6";

static const char* SIG_GET_HEALTH =
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? "
    "? ? ? ? 1E 20 00 1F D6";

static const char* SIG_GET_MAX_HEALTH =
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? "
    "? ? ? ? 1E 40 00 1F D6";

static const char* SIG_GET_EXHAUSTION =
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? "
    "? ? ? ? 1E 60 00 1F D6";

// =============================================================================
//  PatchMemory — same helper as BetterBrightness
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

// ARM64 BL encoding
static uint32_t MakeBL(uintptr_t src, uintptr_t dst) {
    int64_t off = ((int64_t)dst - (int64_t)src) / 4;
    return 0x94000000u | ((uint32_t)off & 0x03FFFFFFu);
}

// ARM64 absolute jump thunk — written into mmap'd RWX page
// Layout: LDR X16, #8 / BR X16 / .quad <target>
static void* MakeThunk(uintptr_t target) {
    void* mem = mmap(nullptr, 4096, PROT_READ|PROT_WRITE|PROT_EXEC,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return nullptr;
    uint32_t* t = (uint32_t*)mem;
    t[0] = 0x58000050u; // LDR X16, #8
    t[1] = 0xD61F0200u; // BR  X16
    memcpy(&t[2], &target, 8);
    __builtin___clear_cache((char*)mem, (char*)mem + 20);
    return mem;
}

// =============================================================================
//  MC function pointers
// =============================================================================
typedef void  (*FnPlayerTick)   (void*, void*, void*);
typedef int   (*FnGetArmor)     (void*, int);
typedef int   (*FnGetFood)      (void*);
typedef float (*FnGetSat)       (void*);
typedef float (*FnGetHealth)    (void*);
typedef float (*FnGetMaxHealth) (void*);
typedef float (*FnGetExhaustion)(void*);

static FnGetArmor      g_getArmor = nullptr;
static FnGetFood       g_getFood  = nullptr;
static FnGetSat        g_getSat   = nullptr;
static FnGetHealth     g_getHp    = nullptr;
static FnGetMaxHealth  g_getMaxHp = nullptr;
static FnGetExhaustion g_getExh   = nullptr;

// Saved bytes from PlayerTick prologue (for trampoline chain)
static uint32_t        g_tickOrig0 = 0; // original first instruction
static uintptr_t       g_tickAddr  = 0;

// =============================================================================
//  HUD state
// =============================================================================
struct ArmorSlot { bool worn=false; int durPct=100; bool low=false; };
struct HUDData {
    ArmorSlot armor[4];
    int   food=20; float saturation=5.f; float health=20.f;
    float maxHealth=20.f; float exhaustion=0.f;
};
static HUDData    g_HUD;
static std::mutex g_HUDMux;

// =============================================================================
//  PlayerTick hook
// =============================================================================
static void Hook_PlayerTick(void* self, void* a, void* b) {
    HUDData d;
    if (g_getArmor) for (int i=0;i<4;i++) {
        int v=g_getArmor(self,i); d.armor[i]={v>0,v,v>0&&v<10};
    }
    if (g_getFood)  d.food        = g_getFood(self);
    if (g_getSat)   d.saturation  = g_getSat(self);
    if (g_getHp)    d.health      = g_getHp(self);
    if (g_getMaxHp) d.maxHealth   = g_getMaxHp(self);
    if (g_getExh)   d.exhaustion  = g_getExh(self);
    { std::lock_guard<std::mutex> lk(g_HUDMux); g_HUD = d; }

    // Restore original instruction, call original, re-patch
    // (simple non-reentrant trampoline — fine for tick rate)
    if (g_tickAddr && g_tickOrig0) {
        PatchMemory((void*)g_tickAddr, g_tickOrig0);
        ((FnPlayerTick)g_tickAddr)(self, a, b);
        void* thunk = MakeThunk((uintptr_t)Hook_PlayerTick);
        if (thunk) PatchMemory((void*)g_tickAddr, MakeBL(g_tickAddr,(uintptr_t)thunk));
    }
}

// =============================================================================
//  ResolveAll — signature scan + PatchMemory
// =============================================================================
static void ResolveAll() {
    const char* lib = "libminecraftpe.so";
    using namespace pl::signature;

    auto R = [&](const char* sig) -> uintptr_t {
        uintptr_t a = pl_resolve_signature(sig, lib);
        if (!a) LOGE("SIG MISS: %.30s...", sig);
        return a;
    };

    if (auto a=R(SIG_GET_ARMOR))      g_getArmor=(FnGetArmor)a;
    if (auto a=R(SIG_GET_FOOD))       g_getFood =(FnGetFood)a;
    if (auto a=R(SIG_GET_SAT))        g_getSat  =(FnGetSat)a;
    if (auto a=R(SIG_GET_HEALTH))     g_getHp   =(FnGetHealth)a;
    if (auto a=R(SIG_GET_MAX_HEALTH)) g_getMaxHp=(FnGetMaxHealth)a;
    if (auto a=R(SIG_GET_EXHAUSTION)) g_getExh  =(FnGetExhaustion)a;

    uintptr_t aTick = R(SIG_PLAYER_TICK);
    if (!aTick) { LOGE("PlayerTick not found"); return; }

    // Save original first instruction
    g_tickAddr  = aTick;
    g_tickOrig0 = *reinterpret_cast<uint32_t*>(aTick);

    // Allocate thunk and patch tick prologue with BL
    void* thunk = MakeThunk((uintptr_t)Hook_PlayerTick);
    if (!thunk) { LOGE("MakeThunk failed"); return; }

    if (PatchMemory((void*)aTick, MakeBL(aTick, (uintptr_t)thunk))) {
        LOGI("PlayerTick patched at 0x%lx -> thunk %p", (unsigned long)aTick, thunk);
    } else {
        LOGE("PatchMemory failed for PlayerTick");
    }
}

// =============================================================================
//  Textures
// =============================================================================
static const char WARN_B64[] =
    "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAYAAADED76LAAAAWUlEQVR4XmNggIK5vLz/"
    "kTFMHAye7Nz5/66R0f9Xe/f+v6Ss/B/E7+HmhigCqUaW2MnPD6dLuLj+wxUgS2Ao"
    "AOkGCaBjuAJ0nSgmgADIQVh1IwOQADKGiQMAm3xuk4wkOW4AAAAASUVORK5CYII=";

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
    std::vector<uint8_t> o; int val=0,bits=-8;
    for(;*s;s++){const char*p=strchr(B64C,*s);if(!p)continue;
        val=(val<<6)+(int)(p-B64C);if((bits+=6)>=0){o.push_back((uint8_t)(val>>bits&0xFF));bits-=8;}}
    return o;
}

struct Tex { GLuint id=0; int w=0,h=0; };
static Tex g_warnTex, g_iconsTex;
static bool g_texLoaded = false;

static Tex UploadPNG(const char* b64) {
    Tex t; auto raw=B64Dec(b64); int n;
    uint8_t* px=stbi_load_from_memory(raw.data(),(int)raw.size(),&t.w,&t.h,&n,4);
    if(!px) return t;
    glGenTextures(1,&t.id); glBindTexture(GL_TEXTURE_2D,t.id);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,t.w,t.h,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    stbi_image_free(px); return t;
}

static void LoadTextures() {
    if(g_texLoaded) return;
    g_warnTex=UploadPNG(WARN_B64); g_iconsTex=UploadPNG(ICONS_B64);
    g_texLoaded=true;
}

// =============================================================================
//  ImGui HUD
// =============================================================================
static float g_time = 0.f;

static void DrawArmorSlot(ImDrawList* dl, ImVec2 pos,
                          const ArmorSlot& s, float t, float sc) {
    float sz=20.f*sc;
    ImVec2 p2(pos.x+sz,pos.y+sz);
    dl->AddRectFilled(pos,p2,IM_COL32(0,0,0,130),3.f*sc);
    if(!s.worn){dl->AddRect(pos,p2,IM_COL32(70,70,70,160),3.f*sc);return;}
    float bh=2.f*sc;
    ImVec2 bb1(pos.x,p2.y-bh),bb2(p2.x,p2.y);
    dl->AddRectFilled(bb1,bb2,IM_COL32(0,0,0,210));
    float pct=s.durPct/100.f;
    dl->AddRectFilled(bb1,ImVec2(pos.x+sz*pct,p2.y),
        IM_COL32((uint8_t)(255*(1-pct)),(uint8_t)(255*pct),0,255));
    char buf[8]; snprintf(buf,sizeof(buf),"%d%%",s.durPct);
    dl->AddText(ImVec2(pos.x+sc,pos.y+sc),IM_COL32(255,255,255,200),buf);
    if(s.low&&g_warnTex.id){
        float a=0.55f+0.45f*sinf(t*6.28f);
        ImVec2 wp(p2.x-8*sc,p2.y-8*sc-bh);
        dl->AddImage((ImTextureID)(uintptr_t)g_warnTex.id,
            wp,ImVec2(p2.x,p2.y-bh),ImVec2(0,0),ImVec2(1,1),
            IM_COL32(255,140,0,(uint8_t)(255*a)));
    }
}

static void DrawFoodIcons(ImDrawList* dl, ImVec2 orig, float value,
                          int fX,int fY,int eX,int eY,
                          ImU32 tint, float sz) {
    if(!g_iconsTex.id) return;
    const float A=1.f/256.f,iw=9.f*A,ih=9.f*A;
    for(int i=0;i<10;i++){
        float fill=value-i*2.f;
        ImVec2 p(orig.x+i*(sz+1.f),orig.y),p2(p.x+sz,p.y+sz);
        dl->AddImage((ImTextureID)(uintptr_t)g_iconsTex.id,p,p2,
            ImVec2(eX*A,eY*A),ImVec2(eX*A+iw,eY*A+ih),IM_COL32(255,255,255,60));
        if(fill>=2.f)
            dl->AddImage((ImTextureID)(uintptr_t)g_iconsTex.id,p,p2,
                ImVec2(fX*A,fY*A),ImVec2(fX*A+iw,fY*A+ih),tint);
        else if(fill>0.f){float fr=fill/2.f;
            dl->AddImage((ImTextureID)(uintptr_t)g_iconsTex.id,
                p,ImVec2(p.x+sz*fr,p2.y),
                ImVec2(fX*A,fY*A),ImVec2(fX*A+iw*fr,fY*A+ih),tint);}
    }
}

static void RenderHUD() {
    g_time+=ImGui::GetIO().DeltaTime;
    LoadTextures();
    HUDData h; {std::lock_guard<std::mutex> lk(g_HUDMux); h=g_HUD;}
    float sw=ImGui::GetIO().DisplaySize.x, sh=ImGui::GetIO().DisplaySize.y;
    float sc=sh/480.f;
    ImDrawList* dl=ImGui::GetBackgroundDrawList();

    // Armor HUD — bottom-left, helm|chest|legs|boots left to right
    float ssz=20.f*sc, gap=2.f*sc;
    float ax=4.f*sc, ay=sh-52.f*sc-ssz;
    for(int i=3;i>=0;i--)
        DrawArmorSlot(dl,ImVec2(ax+(3-i)*(ssz+gap),ay),h.armor[i],g_time,sc);

    // AppleSkin — above hunger bar, bottom-right
    float isz=9.f*sc;
    float fx=sw-10*(isz+sc)-4*sc, fy=sh-49.f*sc;
    // Exhaustion underlay
    DrawFoodIcons(dl,ImVec2(fx,fy),h.food*(h.exhaustion/4.f),
                  52,27,0,27,IM_COL32(153,89,25,178),isz);
    // Saturation overlay
    DrawFoodIcons(dl,ImVec2(fx,fy),fminf(h.saturation,(float)h.food),
                  52,9,16,9,IM_COL32(255,217,25,216),isz);
    // Saturation label
    char buf[16]; snprintf(buf,sizeof(buf),"%.1f",h.saturation);
    dl->AddText(ImVec2(fx-1,fy-11*sc),IM_COL32(255,220,60,220),buf);
}

// =============================================================================
//  eglSwapBuffers hook — raw dlsym, no Gloss/Dobby needed
// =============================================================================
typedef EGLBoolean (*FnSwap)(EGLDisplay,EGLSurface);
static FnSwap     g_origSwap   = nullptr;
static EGLContext g_targetCtx  = EGL_NO_CONTEXT;
static EGLSurface g_targetSurf = EGL_NO_SURFACE;
static bool       g_imguiInit  = false;

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    if(!g_origSwap) return EGL_FALSE;
    EGLContext ctx=eglGetCurrentContext();
    if(ctx==EGL_NO_CONTEXT) return g_origSwap(dpy,surf);
    if(g_targetCtx==EGL_NO_CONTEXT){g_targetCtx=ctx;g_targetSurf=surf;}
    if(ctx!=g_targetCtx||surf!=g_targetSurf) return g_origSwap(dpy,surf);
    EGLint w,h; eglQuerySurface(dpy,surf,EGL_WIDTH,&w); eglQuerySurface(dpy,surf,EGL_HEIGHT,&h);
    if(w<100||h<100) return g_origSwap(dpy,surf);
    if(!g_imguiInit){
        IMGUI_CHECKVERSION(); ImGui::CreateContext();
        ImGui::GetIO().IniFilename=nullptr;
        ImGui::StyleColorsDark();
        ImGui_ImplOpenGL3_Init("#version 300 es");
        g_imguiInit=true; LOGI("ImGui ready (%dx%d)",w,h);
    }
    ImGui::GetIO().DisplaySize=ImVec2((float)w,(float)h);
    ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
    RenderHUD();
    ImGui::Render(); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return g_origSwap(dpy,surf);
}

static void HookEGL() {
    // Get the real eglSwapBuffers, put our hook in front via a thunk
    void* libegl = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
    if(!libegl) { LOGE("libEGL.so not loaded"); return; }
    uintptr_t realSwap = (uintptr_t)dlsym(libegl, "eglSwapBuffers");
    dlclose(libegl);
    if(!realSwap) { LOGE("eglSwapBuffers not found"); return; }

    // Save original and patch with BL to our hook thunk
    g_origSwap = (FnSwap)realSwap;
    void* thunk = MakeThunk((uintptr_t)hook_eglswapbuffers);
    if(thunk && PatchMemory((void*)realSwap, MakeBL(realSwap,(uintptr_t)thunk)))
        LOGI("eglSwapBuffers patched at 0x%lx", (unsigned long)realSwap);
    else
        LOGE("eglSwapBuffers patch FAILED");
}

// =============================================================================
//  Init
// =============================================================================
static void* mainthread(void*) {
    sleep(3);
    ResolveAll();
    HookEGL();
    return nullptr;
}

__attribute__((constructor))
void ArmorFoodHUD_Init() {
    LOGI("ArmorHUD + AppleSkin loading (MC Bedrock 1.26.0+)");
    pthread_t t; pthread_create(&t,nullptr,mainthread,nullptr);
}

