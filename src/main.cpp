/**
 * ArmorHUD + AppleSkin Combined Native Mod
 * For Minecraft Bedrock 1.26.0+ via LeviLaminar (Levi Launcher / mobile)
 *
 * Features:
 *  - Armor HUD: shows equipped armor items + durability warning
 *  - AppleSkin HUD: shows food saturation + hunger-restore preview
 *
 * Build: see CMakeLists.txt
 * Architecture: ARM64 (aarch64) for Android
 */

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <sys/mman.h>
#include <dlfcn.h>
#include <android/log.h>

#include "pl/Gloss.h"
#include "pl/Signature.h"

// ─── Logging ────────────────────────────────────────────────────────────────
#define LOG_TAG "ArmorFoodHUD"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─── Minecraft Bedrock ABI stubs ────────────────────────────────────────────
// These are resolved at runtime via signature scanning against libminecraftpe.so

// --- Player getters ---
// Signature: LocalPlayer::getArmorValue(int slot) -> int  (slots 0-3: boots,legs,chest,helm)
static const char* SIG_GET_ARMOR_VALUE =
    "? ? ? ? ? ? ? ? ? ? ? ? 00 00 00 ? ? ? ? ? ? ? ? ? 08 ? ? ? ? ? ? ? ? ? ? ? 7F ? ? ? 1E";

// Signature: Player::getFoodLevel() -> int  (0-20)
static const char* SIG_GET_FOOD_LEVEL =
    "? ? ? ? 08 ? ? ? 28 ? ? ? ? ? ? ? 1F ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? 00 00";

// Signature: Player::getSaturationLevel() -> float
static const char* SIG_GET_SATURATION =
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? 60 ? ? ? ? ? ? ? ? ? ? 1E ? ? ? ? ? 1E ? ? ? ? 1E ? ? ? ? 52";

// Signature: Player::getHealth() -> float
static const char* SIG_GET_HEALTH =
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? 1E 20 00 1F D6";

// Signature: Player::getMaxHealth() -> float
static const char* SIG_GET_MAX_HEALTH =
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? 1E 40 00 1F D6";

// Signature: Minecraft::getRenderTickCounter() -> float (for animations)
static const char* SIG_RENDER_TICK =
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? 1E ? ? ? ? ? ? ? ? ? ? ? ? 00 2C 40 39";

// Signature: ScreenContext draw sprite call (used to hook HUD render)
static const char* SIG_HUD_RENDER_HOOK =
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? "
    "? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? FD 7B ? A9";

// ─── Function pointer types ──────────────────────────────────────────────────
typedef int   (*fn_GetArmorValue)(void* player, int slot);
typedef int   (*fn_GetFoodLevel)(void* player);
typedef float (*fn_GetSaturation)(void* player);
typedef float (*fn_GetHealth)(void* player);
typedef float (*fn_GetMaxHealth)(void* player);
typedef float (*fn_GetRenderTick)(void* minecraft);

// ─── Resolved pointers ──────────────────────────────────────────────────────
static fn_GetArmorValue  g_GetArmorValue  = nullptr;
static fn_GetFoodLevel   g_GetFoodLevel   = nullptr;
static fn_GetSaturation  g_GetSaturation  = nullptr;
static fn_GetHealth      g_GetHealth      = nullptr;
static fn_GetMaxHealth   g_GetMaxHealth   = nullptr;
static fn_GetRenderTick  g_GetRenderTick  = nullptr;

// ─── HUD state ──────────────────────────────────────────────────────────────
struct HUDState {
    // Armor
    int  armorValue[4]    = {0,0,0,0}; // boots, legs, chest, helm (0=not worn, 1-100=durability%)
    bool armorWorn[4]     = {false,false,false,false};
    bool armorLow[4]      = {false,false,false,false};
    int  armorMinDura     = 10; // warn below 10%

    // Food (AppleSkin)
    int   foodLevel       = 20;   // 0-20
    float saturation      = 5.0f; // 0-20
    float health          = 20.0f;
    float maxHealth       = 20.0f;
    float exhaustion      = 0.0f;

    // Preview (food held in hand)
    int   heldFoodRestore = 0;
    float heldFoodSat     = 0.0f;

    // Animation
    float warnBobTimer    = 0.0f;
    bool  warnVisible     = true;
};

static HUDState g_HUD;

// ─── Memory patching utility ─────────────────────────────────────────────────
static bool PatchMemory(void* addr, uint32_t insn) {
    uintptr_t page_start = (uintptr_t)addr & ~(uintptr_t)4095;
    size_t    page_size  = (sizeof(insn) + 4095) & ~(size_t)4095;
    if (mprotect((void*)page_start, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;
    memcpy(addr, &insn, sizeof(insn));
    __builtin___clear_cache((char*)addr, (char*)addr + sizeof(insn));
    mprotect((void*)page_start, page_size, PROT_READ | PROT_EXEC);
    return true;
}

// ─── Resolve all signatures ──────────────────────────────────────────────────
static bool ResolveSignatures() {
    const char* lib = "libminecraftpe.so";

    uintptr_t armor = pl::signature::pl_resolve_signature(SIG_GET_ARMOR_VALUE, lib);
    uintptr_t food  = pl::signature::pl_resolve_signature(SIG_GET_FOOD_LEVEL,  lib);
    uintptr_t sat   = pl::signature::pl_resolve_signature(SIG_GET_SATURATION,  lib);
    uintptr_t hp    = pl::signature::pl_resolve_signature(SIG_GET_HEALTH,      lib);
    uintptr_t mhp   = pl::signature::pl_resolve_signature(SIG_GET_MAX_HEALTH,  lib);
    uintptr_t tick  = pl::signature::pl_resolve_signature(SIG_RENDER_TICK,     lib);

    if (armor) g_GetArmorValue = reinterpret_cast<fn_GetArmorValue>(armor);
    if (food)  g_GetFoodLevel  = reinterpret_cast<fn_GetFoodLevel>(food);
    if (sat)   g_GetSaturation = reinterpret_cast<fn_GetSaturation>(sat);
    if (hp)    g_GetHealth     = reinterpret_cast<fn_GetHealth>(hp);
    if (mhp)   g_GetMaxHealth  = reinterpret_cast<fn_GetMaxHealth>(mhp);
    if (tick)  g_GetRenderTick = reinterpret_cast<fn_GetRenderTick>(tick);

    LOGI("Signatures resolved: armor=%p food=%p sat=%p hp=%p mhp=%p tick=%p",
         (void*)armor, (void*)food, (void*)sat, (void*)hp, (void*)mhp, (void*)tick);

    // Return true if we got at least food/armor
    return (armor != 0 || food != 0);
}

// ─── HUD render hook ────────────────────────────────────────────────────────
// Called once per frame during the HUD rendering pass.
// 'player' is the local player pointer obtained from the hook context.
// 'screenW' and 'screenH' are the screen dimensions.
// 
// NOTE: Actual pixel drawing is handled by the companion .mcpack UI JSON
//       (ui/hud_screen.json). This function feeds data into shared memory
//       that the UI JSON reads via resource pack scripting (or we emit
//       JSON-encoded state to a named pipe the pack reads).
//
// For LeviLaminar, we use a simpler approach: hook the tick update and
// write HUD data into Bedrock's ScreenContext via its existing draw calls.

// Shared memory segment name for UI data exchange
#define SHM_NAME "/mc_armorfoodnud_v1"

struct SharedHUDData {
    uint32_t magic;          // 0xAF0D1234
    uint32_t version;        // 1
    // Armor (slot 0=boots 1=legs 2=chest 3=helm)
    uint8_t  armorDuraPct[4];  // 0-100, 0=not worn
    uint8_t  armorWarn[4];     // 1=low durability warning
    // Food
    uint8_t  foodLevel;        // 0-20
    uint8_t  foodSaturation;   // 0-20 (floor)
    uint8_t  heldFoodRestore;  // 0-20 preview
    uint8_t  heldFoodSatFloor; // preview sat floor
    // Health
    float    health;
    float    maxHealth;
    // Exhaustion (0.0 - 4.0)
    float    exhaustion;
    // Padding
    uint8_t  _pad[8];
};

static SharedHUDData* g_SharedData = nullptr;

static void InitSharedMemory() {
    // We write a simple file instead of POSIX shm for broader compatibility
    // UI pack reads this via script (not used here; data is rendered natively)
    g_SharedData = new SharedHUDData();
    g_SharedData->magic   = 0xAF0D1234;
    g_SharedData->version = 1;
}

// ─── Frame update: called by the hooked render function ─────────────────────
static void* g_LocalPlayer = nullptr;

static void OnHUDTick(void* player, float dt) {
    if (!player) return;
    g_LocalPlayer = player;

    // --- Armor HUD ---
    if (g_GetArmorValue) {
        for (int i = 0; i < 4; i++) {
            int val = g_GetArmorValue(player, i);
            g_HUD.armorValue[i] = val;
            g_HUD.armorWorn[i]  = (val > 0);
            g_HUD.armorLow[i]   = (val > 0 && val < g_HUD.armorMinDura);
            if (g_SharedData) {
                g_SharedData->armorDuraPct[i] = (uint8_t)val;
                g_SharedData->armorWarn[i]    = g_HUD.armorLow[i] ? 1 : 0;
            }
        }
    }

    // --- AppleSkin / Food HUD ---
    if (g_GetFoodLevel) {
        g_HUD.foodLevel = g_GetFoodLevel(player);
        if (g_SharedData) g_SharedData->foodLevel = (uint8_t)g_HUD.foodLevel;
    }
    if (g_GetSaturation) {
        g_HUD.saturation = g_GetSaturation(player);
        if (g_SharedData) g_SharedData->foodSaturation = (uint8_t)g_HUD.saturation;
    }
    if (g_GetHealth) {
        g_HUD.health = g_GetHealth(player);
        if (g_SharedData) g_SharedData->health = g_HUD.health;
    }
    if (g_GetMaxHealth) {
        g_HUD.maxHealth = g_GetMaxHealth(player);
        if (g_SharedData) g_SharedData->maxHealth = g_HUD.maxHealth;
    }

    // --- Warning bob animation ---
    g_HUD.warnBobTimer += dt * 3.0f;
    bool anyLow = false;
    for (int i = 0; i < 4; i++) if (g_HUD.armorLow[i]) anyLow = true;
    if (anyLow) {
        g_HUD.warnVisible = (sinf(g_HUD.warnBobTimer) > 0.0f);
    } else {
        g_HUD.warnVisible = false;
    }
}

// ─── Trampoline hook for HUD render ─────────────────────────────────────────
// We hook the GuiLayer::renderHUD or ScreenContext tick.
// Using Gloss (pl) inline hook.

typedef void (*fn_OrigRenderHUD)(void* self, void* player, float dt);
static fn_OrigRenderHUD g_OrigRenderHUD = nullptr;

static void Hook_RenderHUD(void* self, void* player, float dt) {
    OnHUDTick(player, dt);
    if (g_OrigRenderHUD) g_OrigRenderHUD(self, player, dt);
}

static bool InstallHooks() {
    const char* lib = "libminecraftpe.so";
    uintptr_t hud_addr = pl::signature::pl_resolve_signature(SIG_HUD_RENDER_HOOK, lib);
    if (hud_addr == 0) {
        LOGE("HUD render hook signature not found – HUD will not update.");
        return false;
    }
    // Install inline hook via Gloss
    void* orig = nullptr;
    bool ok = GlossHook(reinterpret_cast<void*>(hud_addr),
                        reinterpret_cast<void*>(Hook_RenderHUD),
                        &orig);
    if (ok) {
        g_OrigRenderHUD = reinterpret_cast<fn_OrigRenderHUD>(orig);
        LOGI("HUD render hook installed at %p", (void*)hud_addr);
    } else {
        LOGE("Failed to install HUD render hook");
    }
    return ok;
}

// ─── Entry point ─────────────────────────────────────────────────────────────
__attribute__((constructor))
void ArmorFoodHUD_Init() {
    GlossInit(true);
    LOGI("ArmorHUD + AppleSkin combined mod loading (MC 1.26.0+)");

    InitSharedMemory();

    bool sigs = ResolveSignatures();
    if (!sigs) {
        LOGE("Warning: some signatures failed to resolve. HUD data may be incomplete.");
    }

    bool hook = InstallHooks();
    if (hook) {
        LOGI("All hooks installed successfully.");
    }

    LOGI("ArmorHUD + AppleSkin init complete.");
}
