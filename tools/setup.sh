#!/bin/sh
# Prepara as ferramentas: submódulo do XenonRecomp, build das ferramentas e,
# no macOS, o LLVM oficial (o Homebrew compila o LLVM do zero no macOS 14).
set -e
P=$(cd "$(dirname "$0")/.." && pwd)
cd "$P"

git submodule update --init --recursive

if [ "$(uname)" = "Darwin" ] && [ ! -x tools/llvm/LLVM-23.1.2-macOS-ARM64/bin/clang++ ]; then
    echo "Baixando LLVM 23.1.2 (macOS ARM64, ~1.5 GB)..."
    mkdir -p tools/llvm
    curl -L --fail -o tools/llvm/llvm.tar.xz \
        https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.2/LLVM-23.1.2-macOS-ARM64.tar.xz
    tar -xJf tools/llvm/llvm.tar.xz -C tools/llvm
    rm tools/llvm/llvm.tar.xz
fi

cmake -S tools/XenonRecomp -B tools/XenonRecomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -Wno-dev
ninja -C tools/XenonRecomp/build XenonRecomp XenonAnalyse

sh tools/jumptables/build.sh
sh tools/diag/build.sh
echo "Pronto. Coloque seu default.xex em private/game/ e rode: sh tools/regen_config.sh"
