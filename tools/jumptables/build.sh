#!/bin/sh
# Compila o detector de jump tables contra as libs estáticas do XenonRecomp (tools/setup.sh antes).
set -e
D=$(cd "$(dirname "$0")" && pwd)
R=$D/../XenonRecomp
clang++ -std=c++17 -O2 \
  -I "$R/XenonUtils" -I "$R/thirdparty/disasm" -I "$R/thirdparty/fmt/include" \
  "$D/rayman_jumptables.cpp" \
  "$R/build/XenonUtils/libXenonUtils.a" \
  "$R/build/thirdparty/disasm/libdisasm.a" \
  "$R/build/thirdparty/fmt/libfmt.a" \
  -o "$D/rayman_jumptables"
