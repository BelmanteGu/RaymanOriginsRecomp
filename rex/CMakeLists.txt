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
    src/android_touch.cpp
    src/native_capture.cpp
)

if(ANDROID)
    # SDLActivity loads the game as a shared library and calls SDL_main.
    add_library(rayman SHARED ${RAYMAN_SOURCES})
elseif(WIN32)
    add_executable(rayman WIN32 ${RAYMAN_SOURCES})
else()
    add_executable(rayman ${RAYMAN_SOURCES})
endif()

if(ANDROID)
    # The SDK installed for Android ships an Android rexglue binary; codegen
    # runs on the build machine, so point the tool at a host SDK.
    set(REXGLUE_HOST_TOOL "" CACHE FILEPATH "Host rexglue binary for codegen when cross-compiling")
    if(REXGLUE_HOST_TOOL)
        set_target_properties(rex::rexglue PROPERTIES
            IMPORTED_LOCATION_RELEASE "${REXGLUE_HOST_TOOL}"
            IMPORTED_LOCATION "${REXGLUE_HOST_TOOL}")
    endif()
endif()

rexglue_setup_target(rayman GPU_PLUGINS xenos)

if(ANDROID)
    target_link_libraries(rayman PRIVATE log)
endif()
