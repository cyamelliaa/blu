LOCAL_PATH := $(call my-dir)/..

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

include $(CLEAR_VARS)
LOCAL_MODULE    := PvpHud
LOCAL_SRC_FILES := src/main.cpp
LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/deps/imgui \
    $(LOCAL_PATH)/deps/imgui/backends \
    $(LOCAL_PATH)/deps/GlossHook \
    $(LOCAL_PATH)/include
LOCAL_STATIC_LIBRARIES := imgui
LOCAL_LDLIBS    := -llog -landroid -lEGL -lGLESv3
LOCAL_CPPFLAGS  := -std=c++17 -O2 -fvisibility=hidden
include $(BUILD_SHARED_LIBRARY)
