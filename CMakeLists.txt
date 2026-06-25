# Standalone threepp WebGL spike -- a 1:1 C++ port of an esim-frontend style
# "Simulate" tab editor (1000 device grid, click-select + gizmo move, two-click
# wiring), built for the browser via emscripten. Proves the C++/WebGL stack
# reproduces the three.js clone, including on-demand rendering.
#
# Configure with emcmake (see scripts/build.sh / `pixi run build`).

cmake_minimum_required(VERSION 3.24)
project(threecpp_spike CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(THREEPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/threepp")
if (NOT EXISTS "${THREEPP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
            "threepp submodule is missing. Run:\n"
            "    git submodule update --init --recursive\n"
            "(or `pixi run build`, which does it for you).")
endif ()

# Build only the threepp library: no examples/tests (those FetchContent a data
# repo + Catch2 over the network) and no audio backend.
set(THREEPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(THREEPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(THREEPP_WITH_AUDIO OFF CACHE BOOL "" FORCE)
add_subdirectory(${THREEPP_DIR} threepp_build)

add_executable(spike src/main.cpp)
target_link_libraries(spike PRIVATE threepp)

if (NOT DEFINED EMSCRIPTEN)
    message(FATAL_ERROR "This spike targets the browser. Configure with emcmake "
            "(scripts/build.sh / `pixi run build`).")
endif ()

# Production WebGL link flags. The WebGL backend mirrors threepp's own
# examples/AddExample.cmake (OpenGL branch); the rest is tuned for a shipping
# demo build:
#   -O3 --closure 1   full Binaryen wasm-opt + closure-compiled JS glue
#   (no -sASSERTIONS / -sGL_DEBUG)   drop dev-only runtime checks
#   -sENVIRONMENT=web   web only, smaller glue
# The shell file injects the stats.js panel, the software-cursor red dot, and the
# canvas-fill resize hook (web/shell.html).
set(LINK_FLAGS "--bind -sUSE_GLFW=3 -sALLOW_MEMORY_GROWTH -sWASM=1 -sENVIRONMENT=web \
-sNO_DISABLE_EXCEPTION_CATCHING -sEXPORTED_RUNTIME_METHODS=[requestFullscreen] \
-sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sFULL_ES3 \
-O3 --closure=1 \
--shell-file \"${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html\"")

# -O3 / -DNDEBUG for the C++ compile come from CMAKE_BUILD_TYPE=Release (build.sh).
set_target_properties(spike PROPERTIES SUFFIX ".html" LINK_FLAGS "${LINK_FLAGS}")
