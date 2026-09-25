cmake_minimum_required(VERSION 3.20)
project(RaymanPort CXX C)

# Requer Clang 18+ (README do XenonRecomp). No macOS, use o preset com o LLVM em tools/llvm:
#   cmake --preset macos-llvm && cmake --build --preset macos-llvm
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(PPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/recomp/ppc)

# XenonUtils (carregar/descomprimir o XEX). Como não é o projeto de topo, o
# XenonRecomp não adiciona os testes.
add_subdirectory(tools/XenonRecomp EXCLUDE_FROM_ALL)

# ---- Código recompilado (gerado pelo XenonRecomp em recomp/ppc) ----
file(GLOB RAYMAN_PPC_SOURCES CONFIGURE_DEPENDS ${PPC_DIR}/ppc_recomp.*.cpp)

add_library(RaymanRecompLib STATIC
    ${RAYMAN_PPC_SOURCES}
    ${PPC_DIR}/ppc_func_mapping.cpp
)
target_include_directories(RaymanRecompLib PUBLIC
    ${PPC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/thirdparty/simde
)
# Mesmas flags do UnleashedRecompLib. -w: o código gerado produz milhares de avisos inofensivos.
target_compile_options(RaymanRecompLib PRIVATE
    -O2 -fno-strict-aliasing -ffp-model=strict -w
)

# ---- Stubs dos imports (gerados a partir do ppc_func_mapping.cpp) ----
set(IMPORT_STUBS ${CMAKE_CURRENT_BINARY_DIR}/generated/import_stubs.cpp)
add_custom_command(
    OUTPUT ${IMPORT_STUBS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_import_stubs.py ${PPC_DIR}/ppc_func_mapping.cpp ${IMPORT_STUBS}
    DEPENDS ${PPC_DIR}/ppc_func_mapping.cpp ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_import_stubs.py
)

# ---- Runtime ----
add_executable(rayman
    runtime/main.cpp
    runtime/memory.cpp
    runtime/loader.cpp
    runtime/crash_handler.cpp
    runtime/kernel/stub_log.cpp
    runtime/kernel/heap.cpp
    runtime/kernel/page_heap.cpp
    runtime/kernel/memory_imports.cpp
    thirdparty/o1heap/o1heap/o1heap.c
    ${IMPORT_STUBS}
)
target_include_directories(rayman PRIVATE
    runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/o1heap/o1heap
)
target_compile_options(rayman PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-O1 -g -fno-strict-aliasing -ffp-model=strict>)
target_link_libraries(rayman PRIVATE RaymanRecompLib XenonUtils)
if (APPLE)
    # Exporta os símbolos (sub_XXXXXXXX) para o relatório de crash usar dladdr.
    target_link_options(rayman PRIVATE -Wl,-export_dynamic)
endif()
