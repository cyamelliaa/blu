# ArmorHUD + AppleSkin — Bedrock Native Mod

Port of **uku's Armor HUD** + **AppleSkin** to Minecraft Bedrock 1.26.0+ for [LeviLaunchroid](https://github.com/LiteLDev/LeviLaunchroid) (Levi Launcher, mobile).

## Features

**Armor HUD** (bottom-left)
- Shows all 4 armor slots with durability % bar
- Flashing orange warning icon when durability < 10%

**AppleSkin Food HUD** (bottom-right)
- Golden saturation overlay over hunger icons
- Brown exhaustion underlay (shows hunger drain progress)
- Uses the original texture sprites from both mods

## Building

Push this repo to GitHub. The Actions workflow builds automatically and uploads `ArmorFoodHUD.so` as an artifact.

Or build locally:
```bash
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Installing

1. Copy `ArmorFoodHUD.so` to your LeviLaminar mods folder
2. Import `armorfoodnud.mcpack` (texture pack companion)
3. Enable the resource pack in MC Settings → Global Resources
4. Launch Minecraft

## Filling in MC offsets

The file `src/main.cpp` has these constants near the top:

```cpp
static constexpr uintptr_t OFF_GET_ARMOR_DUR  = 0x0;
static constexpr uintptr_t OFF_GET_FOOD       = 0x0;
// etc.
```

Set them to the correct offsets for your MC version by reverse-engineering
`libminecraftpe.so` (IDA Pro / Ghidra). The HUD renders without them but
will show static placeholder values until the offsets are filled.

## Credits

- uku's Armor HUD — BerdinskiyBear / uku (MIT)
- AppleSkin — squeek502 (Unlicense)
- Build pattern — mrover2503-del/MotionBlur
