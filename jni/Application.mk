# jni/Application.mk
APP_ABI      := arm64-v8a          # MCBE Android is ARM64
APP_PLATFORM := android-29         # Android 10+ (MCBE minimum)
APP_STL      := c++_static         # statically link libc++ (no runtime dep)
APP_OPTIM    := release
