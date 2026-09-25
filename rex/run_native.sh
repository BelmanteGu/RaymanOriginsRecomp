#!/bin/sh
# Runs Rayman Origins with the native renderer: no Xbox 360 GPU emulation.
# The game draws through tools/native_renderer (Vulkan/MoltenVK + the game's
# shaders as SPIR-V) straight into its own window.
#
# Needs: librexgpu-null.dylib (ReXGlue with android/rexglue-patches applied)
# next to the executable, and the SPIR-V shaders in private/native/shaders_by_hash
# (see docs/NATIVE_RENDERER.md).
#
# Usage: sh rex/run_native.sh [extra ReXGlue options]
set -e
R=$(cd "$(dirname "$0")" && pwd)
P=$R/..
SDK=$P/tools/rexglue/mac-arm64
BUILD=$R/out/build/mac-arm64-release

for lib in libvulkan.1.dylib libMoltenVK.dylib; do
    [ -f "$BUILD/$lib" ] || cp "$SDK/lib/$lib" "$BUILD/"
done
if [ ! -f "$BUILD/librexgpu-null.dylib" ]; then
    echo "librexgpu-null.dylib is missing from $BUILD (see docs/NATIVE_RENDERER.md)" >&2
    exit 1
fi

cd "$R"
export VK_DRIVER_FILES="$SDK/share/vulkan/icd.d/MoltenVK_icd.json"
export MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1
export RAYMAN_NATIVE_RENDER=main
export RAYMAN_NATIVE_SPIRV="${RAYMAN_NATIVE_SPIRV:-$P/private/native/spirv_ubo}"
exec "$BUILD/rayman" \
    --game_data_root="$P/private/game" \
    --gpu_plugin=null \
    --mnk_mode=true \
    "$@"
