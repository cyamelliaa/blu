cmake_minimum_required(VERSION 3.18)

# CMP0002: allow redefining targets (safety net if old files linger in the repo)
cmake_policy(SET CMP0002 OLD)

project(ArmorFoodHUDProject LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS}  -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -fexceptions -w")
set(CMAKE_C_FLAGS    "${CMAKE_C_FLAGS}    -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -w")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections,--strip-all -s")

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ── Dobby ─────────────────────────────────────────────────────────────────────
set(DOBBY_BUILD_SHARED  OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(DOBBY_DEBUG         OFF CACHE BOOL "" FORCE)
FetchContent_Declare(dobby
    GIT_REPOSITORY https://github.com/jmpews/Dobby.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(dobby)

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

# ── Generate pl/Gloss.h ───────────────────────────────────────────────────────
set(PL_DIR "${CMAKE_BINARY_DIR}/pl_headers")
file(MAKE_DIRECTORY "${PL_DIR}/pl")

file(WRITE "${PL_DIR}/pl/Gloss.h" [[
#pragma once
#include <dlfcn.h>
#include <cstdint>
#include "dobby.h"
typedef void* GHandle;
inline void      GlossInit(bool)                               {}
inline GHandle   GlossOpen(const char* lib)                    { return dlopen(lib, RTLD_NOW | RTLD_NOLOAD); }
inline uintptr_t GlossSymbol(GHandle h, const char* s, void*) { return h ? (uintptr_t)dlsym(h,s) : 0; }
inline bool      GlossHook(void* t, void* r, void** o)        { return DobbyHook(t,r,o) == 0; }
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

# ── Mod target (uniquely named to avoid any possible clash) ───────────────────
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
        dobby_static log android EGL GLESv3 GLESv2
    )
    set_target_properties(armorfoodnud_mod PROPERTIES
        OUTPUT_NAME "ArmorFoodHUD"
        PREFIX ""
    )
endif()
