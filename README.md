# libPvpHud — Minecraft Bedrock Native PvP HUD Mod

A native Android `.so` mod for Minecraft Bedrock Edition that adds a PvP-focused HUD overlay using Dear ImGui, built on the same architecture as `libNoDisconnect` (GlossHook + ImGui over eglSwapBuffers).

## Features

| Panel | Info shown |
|-------|-----------|
| **Health** | Current HP / Max HP, color-coded bar |
| **Food + Saturation** | Hunger bar + hidden saturation bar (big PvP factor) |
| **Armor HUD** | All 4 slots with color-coded durability bars |
| **Offhand** | Item name + durability bar |
| **XP Level** | Current level number |

---

## Project structure

```
pvp_hud/
├── src/
│   └── main.cpp          ← All mod code
├── include/
│   └── mcbe_sdk.h        ← MCBE vtable/symbol helpers (UPDATE OFFSETS!)
├── jni/
│   ├── Android.mk        ← ndk-build script
│   └── Application.mk
└── deps/                 ← you provide these (see below)
    ├── imgui/
    ├── GlossHook/        (or Dobby)
    └── (no MCBE SDK needed — resolved at runtime via dlsym)
```

---

## Prerequisites

### NDK
```bash
# Download NDK r26b (same version used to build libNoDisconnect)
# https://developer.android.com/ndk/downloads
export NDK=/path/to/android-ndk-r26b
```

### Dear ImGui
```bash
git clone https://github.com/ocornut/imgui deps/imgui
# Make sure you have:
#   deps/imgui/backends/imgui_impl_android.cpp
#   deps/imgui/backends/imgui_impl_opengl3.cpp
```

### GlossHook  *(same hooking lib used in libNoDisconnect)*
```bash
# Binary release or build from source:
# https://github.com/axhlzy/GlossHook
# Place .h in deps/GlossHook/include/
# Place libGlossHook.a in deps/GlossHook/libs/arm64-v8a/
```

Alternative: replace GlossHook calls with **Dobby**:
```cpp
// In main.cpp replace:
GlossHook(addr, hookFn, &origFn);
// With:
DobbyHook(addr, hookFn, &origFn);
```

---

## Build

```bash
cd pvp_hud
$NDK/ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk -j$(nproc)
# Output: libs/arm64-v8a/libPvpHud.so
```

---

## Updating Minecraft offsets

The symbols in `include/mcbe_sdk.h` are **mangled C++ names** that change between MCBE versions.

### Method 1: bedrock-headers (recommended)
```
https://github.com/mcbedrock/bedrock-headers
```
Find the method you want, look up its mangled name, put it in `mcbe_sdk.h`.

### Method 2: Ghidra / IDA
1. Open `libminecraftpe.so` in Ghidra
2. Search for the function (e.g. `getHealth`)
3. Copy the mangled symbol name
4. Paste into `resolveSymbol(...)` calls in `mcbe_sdk.h`

### Method 3: offset scanning at runtime
If symbols are stripped, scan for byte patterns:
```cpp
// Example: scan for a known byte sequence
uint8_t pattern[] = {0x08, 0x28, 0x40, 0xF9, ...};
uintptr_t addr = scanMemory("libminecraftpe.so", pattern, sizeof(pattern));
```

---

## Integration with ModMenu / loader

If you're using a mod loader (e.g. the one that loaded `libNoDisconnect`), just drop `libPvpHud.so` in the same mods folder.

For standalone injection via `addongen` / custom launcher:
1. Package the `.so` into the APK or side-load it
2. Call `System.loadLibrary("PvpHud")` from Java, or use `dlopen` from another `.so`
3. Forward `Surface.surfaceCreated()` to `NativeLib.onSurfaceCreated()` so ImGui gets the window

```java
// In your Java bridge (optional if using eglSwapBuffers hook alone)
public class NativeLib {
    static { System.loadLibrary("PvpHud"); }
    public static native void onSurfaceCreated(Surface s);
    public static native void onSurfaceChanged(int w, int h);
}
```

---

## HUD layout (default)

```
[XP Lvl: 5]           [Helm  ████████░░]
                       [Chest ██████████]
                       [Legs  ███████░░░]
                       [Boots ████░░░░░░]
                       [Off   ██████░░░░]

[♥ HP  18.0 / 20  ███████████░]
[🍖 Food 20/20   ████████████]
[★ Sat 4.2       █████░░░░░░░]
```

---

## Common issues

| Problem | Fix |
|---------|-----|
| HUD not appearing | Check `initImGui` is called; verify `eglSwapBuffers` hook succeeded (check logcat) |
| All zeros / wrong values | Symbol names need updating for your MCBE version — use Ghidra |
| Crash on load | GlossHook version mismatch; try Dobby instead |
| ImGui crashes | Ensure OpenGL ES 3.0 context exists before `ImGui_ImplOpenGL3_Init` |
| Touch not working | Wire up `hook_handleInputEvent` to your input dispatcher |

---

## License
Do whatever you want. Credit appreciated.
