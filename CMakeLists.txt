cmake_minimum_required(VERSION 3.22)
project(ArmorFoodHUD VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Target: Android ARM64-v8a ────────────────────────────────────────────────
# Build with Android NDK r26+ targeting API 26 (Android 8.0+, covers Minecraft Bedrock mobile)
# cmake -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#       -DANDROID_ABI=arm64-v8a \
#       -DANDROID_PLATFORM=android-26 \
#       -DCMAKE_BUILD_TYPE=Release \
#       -B build && cmake --build build

# ── Locate pl (Gloss + Signature) ────────────────────────────────────────────
# Clone https://github.com/SYsstemXD/Gloss and https://github.com/Suicolen/Signature
# into deps/pl/ before building.
# Expected layout:
#   deps/pl/include/pl/Gloss.h
#   deps/pl/include/pl/Signature.h
#   deps/pl/src/Gloss.cpp
#   deps/pl/src/Signature.cpp

add_library(pl STATIC
    deps/pl/src/Gloss.cpp
    deps/pl/src/Signature.cpp
)
target_include_directories(pl PUBLIC deps/pl/include)
target_compile_options(pl PRIVATE -O2 -fPIC)

# ── Main mod library ──────────────────────────────────────────────────────────
add_library(ArmorFoodHUD SHARED
    src/main.cpp
)

target_include_directories(ArmorFoodHUD PRIVATE
    deps/pl/include
)

target_link_libraries(ArmorFoodHUD PRIVATE
    pl
    android
    log
)

target_compile_options(ArmorFoodHUD PRIVATE
    -O2
    -fPIC
    -fvisibility=hidden
    -fno-rtti
    -Wno-unused-result
)

# Strip debug symbols in release
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(ArmorFoodHUD PRIVATE -s)
endif()

set_target_properties(ArmorFoodHUD PROPERTIES
    OUTPUT_NAME "ArmorFoodHUD"
    PREFIX      ""
    SUFFIX      ".so"
)

message(STATUS "ArmorFoodHUD: targeting Android ARM64 API 26")
message(STATUS "Output: libArmorFoodHUD.so  (rename to ArmorFoodHUD.so for LeviLaminar)")
