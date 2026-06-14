#pragma once
// Gloss.h — thin wrapper around Dobby for LeviLaminar mods
// Matches the GlossInit/GlossHook/GlossOpen/GlossSymbol API

#include <dlfcn.h>
#include <cstdint>
#include "dobby.h"

typedef void* GHandle;

inline void GlossInit(bool) {
    // Dobby requires no global init
}

inline GHandle GlossOpen(const char* lib) {
    return dlopen(lib, RTLD_NOW | RTLD_NOLOAD);
}

inline uintptr_t GlossSymbol(GHandle handle, const char* name, void* /*unused*/) {
    if (!handle) return 0;
    return (uintptr_t)dlsym(handle, name);
}

inline bool GlossHook(void* target, void* replacement, void** original) {
    return DobbyHook(target, replacement, original) == 0;
}
