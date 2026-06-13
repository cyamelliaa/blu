# jni/Android.mk
# Build: ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk
#
# Required local deps in jni/deps/:
#   GlossHook/  (GlossHook.h + libGlossHook.a or source files)
#   imgui/      (imgui + imgui_impl_android + imgui_impl_opengl3)
#   dobby/      (alternative to GlossHook — pick one)

LOCAL_PATH := $(call my-dir)/..

# ---- Dear ImGui static lib ----
include $(CLEAR_VARS)
LOCAL_MODULE    := imgui
LOCAL_SRC_FILES := \
    deps/imgui/imgui.cpp \
    deps/imgui/imgui_draw.cpp \
    deps/imgui/imgui_tables.cpp \
    deps/imgui/imgui_widgets.cpp \
    deps/imgui/backends/imgui_impl_android.cpp \
    deps/imgui/backends/imgui_impl_opengl3.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/deps/imgui \
                    $(LOCAL_PATH)/deps/imgui/backends
LOCAL_CPPFLAGS  := -std=c++17 -O2
include $(BUILD_STATIC_LIBRARY)

# ---- Main PvP HUD shared library ----
include $(CLEAR_VARS)
LOCAL_MODULE    := PvpHud
LOCAL_SRC_FILES := src/main.cpp
LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/deps/imgui \
    $(LOCAL_PATH)/deps/imgui/backends \
    $(LOCAL_PATH)/deps/GlossHook/include
LOCAL_STATIC_LIBRARIES := imgui
# Link GlossHook: either prebuilt .a or add its .cpp to LOCAL_SRC_FILES
LOCAL_LDLIBS    := -llog -landroid -lEGL -lGLESv3
LOCAL_CPPFLAGS  := -std=c++17 -O2 -fvisibility=hidden
LOCAL_LDFLAGS   := \
    -L$(LOCAL_PATH)/deps/GlossHook/libs/arm64-v8a \
    -lGlossHook \
    -Wl,--gc-sections
include $(BUILD_SHARED_LIBRARY)
