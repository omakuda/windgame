#============================================================================
# FindSDL2_mixer.cmake - Locate SDL2_mixer library
#============================================================================

if(TARGET SDL2_mixer::SDL2_mixer)
    set(SDL2_MIXER_FOUND TRUE)
    return()
endif()

# Try config mode first (vcpkg)
find_package(SDL2_mixer CONFIG QUIET)
if(TARGET SDL2_mixer::SDL2_mixer)
    set(SDL2_MIXER_FOUND TRUE)
    return()
endif()

# Try pkg-config
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(_SDL2MIX QUIET SDL2_mixer)
    if(_SDL2MIX_FOUND)
        set(SDL2_MIXER_INCLUDE_DIRS ${_SDL2MIX_INCLUDE_DIRS})
        set(SDL2_MIXER_LIBRARIES    ${_SDL2MIX_LIBRARIES})
        add_library(SDL2_mixer::SDL2_mixer INTERFACE IMPORTED)
        set_target_properties(SDL2_mixer::SDL2_mixer PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SDL2_MIXER_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES      "${SDL2_MIXER_LIBRARIES}"
        )
        set(SDL2_MIXER_FOUND TRUE)
    endif()
endif()

# Manual search
if(NOT SDL2_MIXER_FOUND)
    find_path(SDL2_MIXER_INCLUDE_DIR SDL_mixer.h
        PATH_SUFFIXES SDL2 include/SDL2
        PATHS /usr/local /usr /opt/homebrew $ENV{SDL2DIR}
    )
    find_library(SDL2_MIXER_LIBRARY
        NAMES SDL2_mixer
        PATH_SUFFIXES lib lib64
        PATHS /usr/local /usr /opt/homebrew $ENV{SDL2DIR}
    )
    if(SDL2_MIXER_INCLUDE_DIR AND SDL2_MIXER_LIBRARY)
        set(SDL2_MIXER_INCLUDE_DIRS "${SDL2_MIXER_INCLUDE_DIR}")
        set(SDL2_MIXER_LIBRARIES    "${SDL2_MIXER_LIBRARY}")
        add_library(SDL2_mixer::SDL2_mixer INTERFACE IMPORTED)
        set_target_properties(SDL2_mixer::SDL2_mixer PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SDL2_MIXER_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES      "${SDL2_MIXER_LIBRARIES}"
        )
        set(SDL2_MIXER_FOUND TRUE)
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SDL2_mixer
    REQUIRED_VARS SDL2_MIXER_LIBRARIES SDL2_MIXER_INCLUDE_DIRS
)
mark_as_advanced(SDL2_MIXER_INCLUDE_DIRS SDL2_MIXER_LIBRARIES)
