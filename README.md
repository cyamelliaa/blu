# ArmorHUD + AppleSkin — Bedrock Native Mod (LeviLaminar)

A combined port of **uku's Armor HUD** (Fabric) and **AppleSkin** (Fabric) to
**Minecraft Bedrock 1.26.0+** as a LeviLaminar native mod (`.so`) + companion
texture pack (`.mcpack`).

---

## What's included

| File | Purpose |
|------|---------|
| `src/main.cpp` | Native mod source — hooks HUD render, reads armor/food data |
| `CMakeLists.txt` | Build system (Android NDK, ARM64) |
| `mcpack/` | Texture pack — original sprites + Bedrock UI JSON |

### Features

**Armor HUD** (from uku's Armor HUD)
- Shows all 4 equipped armor slots at the bottom-left corner of the HUD
- Displays durability percentage bar under each piece
- Shows an orange warning icon (original `warn.png` sprite) when durability < 10%
- Warning icon bobs/flashes when any piece is critically low

**AppleSkin / Food HUD** (from AppleSkin)
- Saturation overlay on the hunger bar (golden tint over hunger icons)
- Exhaustion underlay (shows how close you are to losing saturation)
- Food preview: shows how much hunger/saturation would be restored by held food
- Uses original `appleskin_icons.png` sprite sheet (256×256, same UV offsets)

---

## Building the `.so`

### Requirements
- [Android NDK r26+](https://developer.android.com/ndk/downloads)
- CMake 3.22+
- [Gloss](https://github.com/SYsstemXD/Gloss) + [Signature](https://github.com/Suicolen/Signature) (pl library)

### Steps

```bash
# 1. Clone pl dependencies
mkdir -p deps/pl
git clone https://github.com/SYsstemXD/Gloss deps/pl/gloss
git clone https://github.com/Suicolen/Signature deps/pl/sig
# Merge headers: copy Gloss.h, Signature.h into deps/pl/include/pl/
# Merge sources: copy *.cpp into deps/pl/src/

# 2. Configure (replace $NDK with your NDK path)
cmake \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release \
  -B build

# 3. Build
cmake --build build

# Output: build/ArmorFoodHUD.so
```

### GitHub Actions (automatic build)

You can use [MotionBlur's workflow](https://github.com/mrover2503-del/MotionBlur) as a
template. The key matrix entry:

```yaml
- name: Build ArmorFoodHUD
  run: |
    cmake -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
          -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
          -DCMAKE_BUILD_TYPE=Release -B build
    cmake --build build
```

---

## Installing on LeviLaminar (Levi Launcher)

1. **Copy the `.so`** to your LeviLaminar mods folder:
   ```
   /sdcard/games/com.mojang/mods/ArmorFoodHUD.so
   ```
   (exact path depends on your Levi Launcher version — check its docs)

2. **Install the texture pack** (`armorfoodnud.mcpack`):
   - Double-tap to import, or copy to:
     ```
     /sdcard/games/com.mojang/resource_packs/armorfoodnud/
     ```
   - Enable it in Minecraft → Settings → Global Resources

3. Launch Minecraft. Both features activate automatically.

---

## Signature notes

The signatures in `main.cpp` are **wildcard patterns** (using `?` bytes) so
they work across minor Bedrock updates. If MC gets a major update and sigs
break:

1. Open `libminecraftpe.so` in IDA Pro / Ghidra
2. Find `LocalPlayer::getArmorValue`, `Player::getFoodLevel`, etc.
3. Update the corresponding `SIG_*` constants in `src/main.cpp`

The `PatchMemory()` utility and the `pl::signature::pl_resolve_signature()`
approach is the same pattern used by [BetterBrightness](https://github.com/...) 
and MotionBlur.

---

## Texture credits

- `appleskin_icons.png`, `tooltip_hunger_outline.png` — © squeek502 (Unlicense)
- `armor_warn.png` — © BerdinskiyBear / uku (MIT)

Both original mods are open-source. This port is provided under the same licenses.
