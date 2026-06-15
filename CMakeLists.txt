cmake_minimum_required(VERSION 3.18)
cmake_policy(SET CMP0002 OLD)
project(ArmorFoodHUDProject LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS}  -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -fexceptions -w")
set(CMAKE_C_FLAGS    "${CMAKE_C_FLAGS}    -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -w")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections,--strip-all -s")

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ── Dear ImGui ────────────────────────────────────────────────────────────────
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# ── stb ───────────────────────────────────────────────────────────────────────
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(stb)

# ── Generate pl/Gloss.h (no Dobby — pure mmap inline hook) ───────────────────
set(PL_DIR "${CMAKE_BINARY_DIR}/pl_headers")
file(MAKE_DIRECTORY "${PL_DIR}/pl")

file(WRITE "${PL_DIR}/pl/Gloss.h" [[
#pragma once
// pl/Gloss.h — zero-dependency inline hook for ARM64 Android
// Replaces Dobby. Uses mmap RWX trampoline + PatchMemory BL patch.
#include <dlfcn.h>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>

typedef void* GHandle;

inline void GlossInit(bool) {}

inline GHandle GlossOpen(const char* lib) {
    return dlopen(lib, RTLD_NOW | RTLD_NOLOAD);
}

inline uintptr_t GlossSymbol(GHandle h, const char* s, void*) {
    return h ? (uintptr_t)dlsym(h, s) : 0;
}

// ARM64 inline hook:
//   1. Allocate RWX page for trampoline (original 4 bytes + absolute jump back)
//   2. Patch target with LDR+BR to replacement (position-independent, any distance)
//   3. *orig points to the trampoline so callers can invoke the original
inline bool GlossHook(void* target, void* replacement, void** orig) {
    if (!target || !replacement) return false;

    // Allocate trampoline page
    uint8_t* tramp = (uint8_t*)mmap(nullptr, 4096,
        PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) return false;

    // Save original first instruction (we overwrite 16 bytes: LDR+BR+addr)
    uint32_t orig_insn[4];
    memcpy(orig_insn, target, sizeof(orig_insn));

    // Trampoline layout:
    //   [0] = original instruction 0  (saved)
    //   [1] = original instruction 1  (saved, in case hook is 8 bytes)
    //   [2] = original instruction 2  (saved)
    //   [3] = original instruction 3  (saved)
    //   [4] LDR X16, #8
    //   [5] BR  X16
    //   [6..7] = (target + 16) as 64-bit absolute address (continue after hook)
    memcpy(tramp, orig_insn, 16);
    uint32_t* t = (uint32_t*)(tramp + 16);
    t[0] = 0x58000050u; // LDR X16, #8
    t[1] = 0xD61F0200u; // BR  X16
    uintptr_t resume = (uintptr_t)target + 16;
    memcpy(&t[2], &resume, 8);
    __builtin___clear_cache((char*)tramp, (char*)tramp + 36);

    if (orig) *orig = (void*)tramp;

    // Patch target: LDR X17, #8 / BR X17 / .quad replacement
    // This is 16 bytes — covers the 4 saved instructions
    uintptr_t page   = (uintptr_t)target & ~(uintptr_t)4095;
    size_t    pgsz   = ((uintptr_t)target + 16 - page + 4095) & ~(size_t)4095;
    if (mprotect((void*)page, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;

    uint32_t* tgt = (uint32_t*)target;
    tgt[0] = 0x58000051u; // LDR X17, #8
    tgt[1] = 0xD61F0220u; // BR  X17
    uintptr_t raddr = (uintptr_t)replacement;
    memcpy(&tgt[2], &raddr, 8);
    __builtin___clear_cache((char*)target, (char*)target + 16);

    mprotect((void*)page, pgsz, PROT_READ | PROT_EXEC);
    return true;
}
]])

# ── Generate pl/Signature.h ───────────────────────────────────────────────────
file(WRITE "${PL_DIR}/pl/Signature.h" [[
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <link.h>
namespace pl::signature {
static uintptr_t pl_resolve_signature(const char* pattern, const char* libname) {
    uint8_t pat[512]; bool mask[512]; size_t len = 0;
    for (const char* p = pattern; *p && len < 512; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '?') { pat[len]=0; mask[len++]=false; p++; if(*p=='?')p++; }
        else { pat[len]=(uint8_t)strtoul(p,nullptr,16); mask[len++]=true; p+=2; }
    }
    if (!len) return 0;
    struct Ctx { const char* lib; const uint8_t* pat; const bool* mask; size_t len; uintptr_t result; };
    Ctx ctx{libname, pat, mask, len, 0};
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
        auto* c = (Ctx*)data;
        if (!info->dlpi_name || !strstr(info->dlpi_name, c->lib)) return 0;
        for (int i = 0; i < info->dlpi_phnum; i++) {
            auto& ph = info->dlpi_phdr[i];
            if (ph.p_type != PT_LOAD || !(ph.p_flags & 1)) continue;
            auto* base = (const uint8_t*)(info->dlpi_addr + ph.p_vaddr);
            size_t sz = ph.p_filesz;
            for (size_t off = 0; off + c->len <= sz; off += 4) {
                bool ok = true;
                for (size_t j = 0; j < c->len && ok; j++)
                    if (c->mask[j] && base[off+j] != c->pat[j]) ok = false;
                if (ok) { c->result = (uintptr_t)(base+off); return 1; }
            }
        }
        return 0;
    }, &ctx);
    return ctx.result;
}
} // namespace pl::signature
]])

# ── Include paths ─────────────────────────────────────────────────────────────
include_directories(
    src
    ${PL_DIR}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${stb_SOURCE_DIR}
)

# ── Mod target ────────────────────────────────────────────────────────────────
if(NOT TARGET armorfoodnud_mod)
    add_library(armorfoodnud_mod SHARED
        src/main.cpp
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_android.cpp
    )
    target_link_libraries(armorfoodnud_mod PRIVATE
        log android EGL GLESv3 GLESv2
    )
    set_target_properties(armorfoodnud_mod PROPERTIES
        OUTPUT_NAME "ArmorFoodHUD"
        PREFIX ""
    )
endif()
