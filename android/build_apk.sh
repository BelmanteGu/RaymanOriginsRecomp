#!/bin/sh
# Builds the Android APK from the native libraries of the rex/ Android build.
#
# Prerequisites (see docs/ANDROID.md):
#   - ReXGlue SDK built and installed for android-arm64
#   - rex/ configured and built in rex/out/build/android-arm64
#   - Android SDK (ANDROID_HOME or ~/Library/Android/sdk) with NDK 28.2.13676358
#
# Usage: sh android/build_apk.sh [debug|release]
set -e
A=$(cd "$(dirname "$0")" && pwd)
P=$A/..
TYPE=${1:-debug}
SDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}
NDK=$SDK/ndk/28.2.13676358
REXSDK=${REXSDK_ANDROID:-$P/tools/forks/rexglue-src/out/install-android-arm64}
BUILD=$P/rex/out/build/android-arm64
LIBS=$A/app/libs/arm64-v8a

mkdir -p "$LIBS"
cp "$BUILD/librayman.so" "$LIBS/"
cp "$REXSDK/lib/librexruntime.so" "$REXSDK/lib/librexgpu-xenos.so" "$REXSDK/lib/librexgpu-null.so" "$LIBS/"
cp "$NDK/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$LIBS/"

# Game-derived files packed into this local APK (never committed, never
# published): the SPIR-V shaders from tools/native_renderer/build_spirv.sh and
# optional launcher art from private/launcher.
SPIRV=${RAYMAN_SPIRV:-$P/private/native/spirv_ubo}
rm -rf "$A/app/src/main/assets/spirv"
if [ -d "$SPIRV" ]; then
    mkdir -p "$A/app/src/main/assets/spirv"
    cp "$SPIRV"/*.spv "$A/app/src/main/assets/spirv/"
fi
rm -rf "$A/app/src/main/assets/launcher"
if [ -d "$P/private/launcher" ]; then
    mkdir -p "$A/app/src/main/assets/launcher"
    cp "$P/private/launcher"/* "$A/app/src/main/assets/launcher/"
fi

[ -f "$A/local.properties" ] || echo "sdk.dir=$SDK" > "$A/local.properties"

cd "$A"
if [ "$TYPE" = release ]; then
    ./gradlew assembleRelease
    echo "APK: $A/app/build/outputs/apk/release/app-release.apk"
else
    ./gradlew assembleDebug
    echo "APK: $A/app/build/outputs/apk/debug/app-debug.apk"
fi
