#============================================================================
# FindSDL2.cmake - Locate SDL2 library
#
# This module defines:
#   SDL2_FOUND        - True if SDL2 was found
#   SDL2_INCLUDE_DIRS - Include directories for SDL2
#   SDL2_LIBRARIES    - Libraries to link against
#   SDL2::SDL2        - Imported target
#
# Search order:
#   1. CMake config package (SDL2Config.cmake — vcpkg, manual SDK)
#   2. pkg-config (Linux)
#   3. sdl2-config script (Linux/macOS)
#   4. Common install paths (manual search)
#============================================================================

# Skip entirely if target already exists
if(TARGET SDL2::SDL2)
    set(SDL2_FOUND TRUE)
    return()
endif()

#---------------------------------------------------------------------------
# Try config mode FIRST (vcpkg, SDL2 >= 2.0.12 with cmake support)
# This is the key fix: we explicitly attempt config mode before falling
# back to manual discovery.
#---------------------------------------------------------------------------
find_package(SDL2 CONFIG QUIET)
if(TARGET SDL2::SDL2)
    set(SDL2_FOUND TRUE)
    # Populate legacy variables for compatibility
    get_target_property(SDL2_INCLUDE_DIRS SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
    set(SDL2_LIBRARIES SDL2::SDL2)
    if(NOT SDL2_INCLUDE_DIRS)
        set(SDL2_INCLUDE_DIRS "")
    endif()
    return()
endif()

#---------------------------------------------------------------------------
# Try pkg-config (most reliable on Linux)
#---------------------------------------------------------------------------
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(_SDL2 QUIET sdl2)
    if(_SDL2_FOUND)
        set(SDL2_INCLUDE_DIRS ${_SDL2_INCLUDE_DIRS})
        set(SDL2_LIBRARIES    ${_SDL2_LIBRARIES})
        set(SDL2_LIBRARY_DIRS ${_SDL2_LIBRARY_DIRS})

        add_library(SDL2::SDL2 INTERFACE IMPORTED)
        set_target_properties(SDL2::SDL2 PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES      "${SDL2_LIBRARIES}"
            INTERFACE_LINK_DIRECTORIES    "${SDL2_LIBRARY_DIRS}"
        )
        if(NOT TARGET SDL2::SDL2main)
            add_library(SDL2::SDL2main INTERFACE IMPORTED)
        endif()

        set(SDL2_FOUND TRUE)
    endif()
endif()

#---------------------------------------------------------------------------
# Try sdl2-config script
#---------------------------------------------------------------------------
if(NOT SDL2_FOUND)
    find_program(SDL2_CONFIG sdl2-config)
    if(SDL2_CONFIG)
        execute_process(COMMAND ${SDL2_CONFIG} --cflags
                        OUTPUT_VARIABLE _SDL2_CFLAGS
                        OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND ${SDL2_CONFIG} --libs
                        OUTPUT_VARIABLE _SDL2_LIBS
                        OUTPUT_STRIP_TRAILING_WHITESPACE)

        string(REGEX MATCHALL "-I[^ ]+" _SDL2_INC_FLAGS "${_SDL2_CFLAGS}")
        set(SDL2_INCLUDE_DIRS "")
        foreach(_flag ${_SDL2_INC_FLAGS})
            string(REGEX REPLACE "^-I" "" _dir "${_flag}")
            list(APPEND SDL2_INCLUDE_DIRS "${_dir}")
        endforeach()

        set(SDL2_LIBRARIES ${_SDL2_LIBS})

        add_library(SDL2::SDL2 INTERFACE IMPORTED)
        set_target_properties(SDL2::SDL2 PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES      "${SDL2_LIBRARIES}"
        )
        if(NOT TARGET SDL2::SDL2main)
            add_library(SDL2::SDL2main INTERFACE IMPORTED)
        endif()

        set(SDL2_FOUND TRUE)
    endif()
endif()

#---------------------------------------------------------------------------
# Manual search in common paths
#---------------------------------------------------------------------------
if(NOT SDL2_FOUND)
    find_path(SDL2_INCLUDE_DIR SDL.h
        PATH_SUFFIXES SDL2 include/SDL2
        PATHS
            /usr/local
            /usr
            /opt/homebrew
            $ENV{SDL2DIR}
    )

    find_library(SDL2_LIBRARY
        NAMES SDL2
        PATH_SUFFIXES lib lib64
        PATHS
            /usr/local
            /usr
            /opt/homebrew
            $ENV{SDL2DIR}
    )

    if(SDL2_INCLUDE_DIR AND SDL2_LIBRARY)
        get_filename_component(_inc_parent "${SDL2_INCLUDE_DIR}" DIRECTORY)
        get_filename_component(_inc_leaf   "${SDL2_INCLUDE_DIR}" NAME)
        if(_inc_leaf STREQUAL "SDL2")
            set(SDL2_INCLUDE_DIRS "${_inc_parent}")
        else()
            set(SDL2_INCLUDE_DIRS "${SDL2_INCLUDE_DIR}")
        endif()

        set(SDL2_LIBRARIES "${SDL2_LIBRARY}")

        add_library(SDL2::SDL2 INTERFACE IMPORTED)
        set_target_properties(SDL2::SDL2 PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES      "${SDL2_LIBRARIES}"
        )
        if(NOT TARGET SDL2::SDL2main)
            add_library(SDL2::SDL2main INTERFACE IMPORTED)
        endif()

        set(SDL2_FOUND TRUE)
    endif()
endif()

#---------------------------------------------------------------------------
# Report result
#---------------------------------------------------------------------------
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SDL2
    REQUIRED_VARS SDL2_LIBRARIES SDL2_INCLUDE_DIRS
)
mark_as_advanced(SDL2_INCLUDE_DIRS SDL2_LIBRARIES)
