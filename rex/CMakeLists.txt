# rayman - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(rayman LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(RAYMAN_SOURCES
    src/main.cpp
    src/hooks.cpp
)

if(WIN32)
    add_executable(rayman WIN32 ${RAYMAN_SOURCES})
else()
    add_executable(rayman ${RAYMAN_SOURCES})
endif()

rexglue_setup_target(rayman)
