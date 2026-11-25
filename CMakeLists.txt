cmake_minimum_required(VERSION 3.20)
project(MPXpack LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Statically link the MSVC runtime so mpx_sender.exe/mpx_receiver.exe run on
# any Windows x64 machine with no VC++ Redistributable installed. Must be set
# before any target (including fetched WavPack/GLFW/ImGui) is created, since
# every target in the build has to agree on this or linking fails.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(MSVC)
    add_compile_options(/W4 /wd4996)
else()
    add_compile_options(-Wall -Wextra)
endif()

# --- WavPack: fetch mainline WavPack and build it as a static library. ---
# Force these before FetchContent_MakeAvailable() so WavPack's own
# CMakeLists.txt sees them already set in the cache (it otherwise defaults
# them itself).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WAVPACK_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(WAVPACK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(WAVPACK_ENABLE_DSD OFF CACHE BOOL "" FORCE)
set(WAVPACK_ENABLE_LEGACY OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

include(FetchContent)
# Pinned to the exact upstream master commit this project was developed and
# tested against (post-5.9.0; includes the 2026 decoder hardening fixes).
# A full SHA can't be shallow-cloned, so GIT_SHALLOW is deliberately omitted.
# To use a local checkout instead (e.g. offline), configure with
#   -DFETCHCONTENT_SOURCE_DIR_WAVPACK=<path-to-WavPack-source>
FetchContent_Declare(
    wavpack
    GIT_REPOSITORY https://github.com/dbry/WavPack.git
    GIT_TAG        eccf998c7acce58e18dedd354e6b025728dcf6da
)
FetchContent_MakeAvailable(wavpack)

# Note: the separate wavpack-stream fork is intentionally not used — mainline
# WavPack's OPEN_STREAMING mode is a superset of what that frozen 2018 fork
# offers.

include(cmake/FetchDeps.cmake)

add_subdirectory(src/common)
add_subdirectory(src/sender)
add_subdirectory(src/receiver)

enable_testing()
add_subdirectory(tests)
