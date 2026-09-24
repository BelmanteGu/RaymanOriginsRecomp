#!/bin/sh
# Compila o diagnóstico contra as libs estáticas do XenonRecomp (tools/setup.sh antes).
set -e
D=$(cd "$(dirname "$0")" && pwd)
R=$D/../XenonRecomp
clang++ -std=c++17 -O0 -g \
  -I "$R/XenonUtils" -I "$R/thirdparty/disasm" -I "$R/thirdparty/fmt/include" \
  "$D/diag.cpp" \
  "$R/build/XenonUtils/libXenonUtils.a" \
  "$R/build/thirdparty/disasm/libdisasm.a" \
  "$R/build/thirdparty/fmt/libfmt.a" \
  -o "$D/diag"
