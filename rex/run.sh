#!/bin/sh
# Roda o Rayman Origins recompilado (runtime ReXGlue) no macOS.
#
# Uso: sh rex/run.sh [opções extras do ReXGlue]
#
# Controles no teclado (controle de Xbox/PlayStation funciona direto):
#   W A S D = mover        Espaço = A (pular)     L = X (atacar)
#   Backspace = B          P = Y                  E = gatilho direito (correr)
#   Enter = Start          Tab = Back             Shift+setas = direcional
set -e
R=$(cd "$(dirname "$0")" && pwd)
P=$R/..
SDK=$P/tools/rexglue/mac-arm64
BUILD=$R/out/build/mac-arm64-release

# O plugin de GPU e o Vulkan (MoltenVK) precisam estar ao lado do executável.
for lib in librexgpu-xenos.dylib libvulkan.1.dylib libMoltenVK.dylib; do
    [ -f "$BUILD/$lib" ] || cp "$SDK/lib/$lib" "$BUILD/"
done

export VK_DRIVER_FILES="$SDK/share/vulkan/icd.d/MoltenVK_icd.json"
exec "$BUILD/rayman" \
    --game_data_root="$P/private/game" \
    --gpu_plugin=xenos \
    --mnk_mode=true \
    "$@"
