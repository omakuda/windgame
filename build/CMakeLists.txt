#============================================================================
# CMakeLists.txt - Windows x64 / Linux / macOS Build (SDL2)
#
# === QUICK START (Windows + vcpkg) ===
#
#   1. Install vcpkg if you haven't:
#        git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
#        cd C:\vcpkg && bootstrap-vcpkg.bat
#
#   2. Build (vcpkg.json manifest auto-installs SDL2):
#        cd hal_engine\build
#        cmake -B out -G "Visual Studio 17 2022" -A x64 ^
#              -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
#        cmake --build out --config Release
#
# === Other platforms ===
#
#   Linux:  sudo apt install libsdl2-dev libsdl2-mixer-dev
#           cd build && cmake -B out && cmake --build out
#
#   macOS:  brew install sdl2 sdl2_mixer
#           cd build && cmake -B out && cmake --build out
#
# === Manual SDL2 SDK (no vcpkg) ===
#
#   Download SDL2-devel-x.x.x-VC.zip from github.com/libsdl-org/SDL/releases
#   Extract to e.g. C:\SDL2, then:
#        cmake -B out -G "Visual Studio 17 2022" -A x64 ^
#              -DCMAKE_PREFIX_PATH=C:\SDL2
#============================================================================

cmake_minimum_required(VERSION 3.16)
project(game C)

set(CMAKE_C_STANDARD 99)

#---------------------------------------------------------------------------
# Find SDL2
#
# Discovery order:
#   1. vcpkg manifest mode (vcpkg.json in this directory)
#   2. CMake config mode (SDL2Config.cmake from vcpkg/manual install)
#   3. Bundled FindSDL2.cmake (pkg-config / sdl2-config / path search)
#---------------------------------------------------------------------------

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(SDL2 REQUIRED)
find_package(SDL2_mixer QUIET)

#---------------------------------------------------------------------------
# Source files
#---------------------------------------------------------------------------

set(GAME_SOURCES
    ../src/game/main.c
    ../src/game/overworld_events.c
)

set(HAL_SOURCES
    ../src/hal/win64/hal_win64.c
)

#---------------------------------------------------------------------------
# Executable
#---------------------------------------------------------------------------

add_executable(game
    ${GAME_SOURCES}
    ${HAL_SOURCES}
)

target_include_directories(game PRIVATE
    ../src
)

#---------------------------------------------------------------------------
# Link SDL2
#---------------------------------------------------------------------------

if(TARGET SDL2::SDL2)
    target_link_libraries(game PRIVATE SDL2::SDL2)
else()
    target_include_directories(game PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(game PRIVATE ${SDL2_LIBRARIES})
endif()

if(TARGET SDL2_mixer::SDL2_mixer)
    target_link_libraries(game PRIVATE SDL2_mixer::SDL2_mixer)
elseif(SDL2_MIXER_FOUND)
    target_include_directories(game PRIVATE ${SDL2_MIXER_INCLUDE_DIRS})
    target_link_libraries(game PRIVATE ${SDL2_MIXER_LIBRARIES})
endif()

#---------------------------------------------------------------------------
# Build options
#---------------------------------------------------------------------------

target_compile_definitions(game PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
    # Silence MSVC warnings about fopen/sprintf — we need portable C99
    $<$<C_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)

if(MSVC)
    target_compile_options(game PRIVATE /W4)
else()
    target_compile_options(game PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Use console subsystem so our main() is the real entry point.
# SDL2main provides its own main() which conflicts with ours,
# so we deliberately do NOT link SDL2main.
if(WIN32)
    set_target_properties(game PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

#---------------------------------------------------------------------------
# Copy SDL2 DLLs alongside executable (Windows only)
#---------------------------------------------------------------------------

if(WIN32 AND TARGET SDL2::SDL2)
    add_custom_command(TARGET game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2>
            $<TARGET_FILE_DIR:game>
        COMMENT "Copying SDL2.dll"
    )
endif()
