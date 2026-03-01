#!/usr/bin/env bash
#============================================================================
# build.sh - HAL Engine Build Script (Linux / macOS)
#
# Usage:
#   ./build.sh              Build Release
#   ./build.sh debug        Build Debug
#   ./build.sh clean        Remove build artifacts
#   ./build.sh release      Build Release + package tarball
#   ./build.sh next         Build ZX Spectrum Next (requires z88dk)
#   ./build.sh all          Build all targets + package
#   ./build.sh run          Build Release and launch
#   ./build.sh install-deps Install SDL2 dependencies (apt/brew)
#   ./build.sh help         Show this message
#
# Environment variables (optional):
#   VCPKG_ROOT         Path to vcpkg
#   SDL2_DIR           Manual SDL2 path (if not system-installed)
#   Z88DK_PATH         Path to z88dk installation
#   CMAKE_GENERATOR    Override CMake generator (default: auto)
#   BUILD_JOBS         Parallel jobs (default: nproc)
#============================================================================

set -euo pipefail

PROJECT_NAME="game"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build/out"
NEXT_BUILD_DIR="${SCRIPT_DIR}/build/out_next"
DIST_DIR="${SCRIPT_DIR}/dist"
BUILD_JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

# Colors (if terminal supports it)
if [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
    CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; CYAN=''; BOLD=''; NC=''
fi

info()  { echo -e "${CYAN}[info]${NC}  $*"; }
ok()    { echo -e "${GREEN}[ok]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[warn]${NC}  $*"; }
err()   { echo -e "${RED}[error]${NC} $*" >&2; }
die()   { err "$@"; exit 1; }

# ========================================================================
show_help() {
# ========================================================================
    cat <<'EOF'

  HAL Engine Build Script
  =======================
  ./build.sh              Build Release
  ./build.sh debug        Build Debug
  ./build.sh clean        Remove build artifacts
  ./build.sh release      Build Release + package tarball
  ./build.sh next         Build ZX Spectrum Next (requires z88dk)
  ./build.sh all          Build all targets + package
  ./build.sh run          Build Release and launch
  ./build.sh install-deps Install SDL2 via system package manager
  ./build.sh help         This message

  Optional environment variables:
    VCPKG_ROOT     Path to vcpkg installation
    SDL2_DIR       Path to manually installed SDL2
    Z88DK_PATH     Path to z88dk (for Next builds)
    BUILD_JOBS     Parallel build jobs (default: auto)

EOF
}

# ========================================================================
detect_platform() {
# ========================================================================
    OS="$(uname -s)"
    case "$OS" in
        Linux*)  PLATFORM="linux";;
        Darwin*) PLATFORM="macos";;
        MINGW*|MSYS*|CYGWIN*) PLATFORM="windows";;
        *)       PLATFORM="unknown";;
    esac
    info "Platform: $PLATFORM ($OS)"
}

# ========================================================================
detect_vcpkg() {
# ========================================================================
    CMAKE_EXTRA_ARGS=""
    if [ -n "${VCPKG_ROOT:-}" ] && [ -f "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" ]; then
        info "vcpkg: ${VCPKG_ROOT}"
        CMAKE_EXTRA_ARGS="-DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        return 0
    fi

    # Auto-detect common locations
    local candidates=(
        "$HOME/vcpkg"
        "/opt/vcpkg"
        "$HOME/.local/share/vcpkg"
    )
    for d in "${candidates[@]}"; do
        if [ -f "$d/scripts/buildsystems/vcpkg.cmake" ]; then
            info "vcpkg: found at $d"
            VCPKG_ROOT="$d"
            CMAKE_EXTRA_ARGS="-DCMAKE_TOOLCHAIN_FILE=$d/scripts/buildsystems/vcpkg.cmake"
            return 0
        fi
    done

    # No vcpkg — rely on system SDL2
    if [ -n "${SDL2_DIR:-}" ]; then
        CMAKE_EXTRA_ARGS="-DCMAKE_PREFIX_PATH=${SDL2_DIR}"
        info "SDL2: using SDL2_DIR=${SDL2_DIR}"
    else
        info "vcpkg: not found, using system SDL2"
    fi
}

# ========================================================================
install_deps() {
# ========================================================================
    detect_platform
    case "$PLATFORM" in
        linux)
            if command -v apt-get &>/dev/null; then
                info "Installing via apt..."
                sudo apt-get update
                sudo apt-get install -y build-essential cmake libsdl2-dev libsdl2-mixer-dev
            elif command -v dnf &>/dev/null; then
                info "Installing via dnf..."
                sudo dnf install -y gcc cmake SDL2-devel SDL2_mixer-devel
            elif command -v pacman &>/dev/null; then
                info "Installing via pacman..."
                sudo pacman -S --noconfirm base-devel cmake sdl2 sdl2_mixer
            else
                die "Unsupported package manager. Install cmake, SDL2, SDL2_mixer manually."
            fi
            ;;
        macos)
            if ! command -v brew &>/dev/null; then
                die "Homebrew not found. Install from https://brew.sh"
            fi
            info "Installing via brew..."
            brew install cmake sdl2 sdl2_mixer
            ;;
        *)
            die "install-deps not supported on $PLATFORM. Install SDL2 manually."
            ;;
    esac
    ok "Dependencies installed."
}

# ========================================================================
check_cmake() {
# ========================================================================
    if ! command -v cmake &>/dev/null; then
        die "cmake not found. Run './build.sh install-deps' or install manually."
    fi
    local ver
    ver="$(cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+')"
    info "cmake: $ver"
}

# ========================================================================
configure() {
# ========================================================================
    check_cmake
    detect_vcpkg

    local generator_args=""
    if [ -n "${CMAKE_GENERATOR:-}" ]; then
        generator_args="-G ${CMAKE_GENERATOR}"
    elif command -v ninja &>/dev/null; then
        generator_args="-G Ninja"
        info "Generator: Ninja"
    else
        info "Generator: Unix Makefiles"
    fi

    info "Configuring..."
    cmake -B "${BUILD_DIR}" ${generator_args} ${CMAKE_EXTRA_ARGS} \
          -S "${SCRIPT_DIR}/build"

    ok "Configure complete."
}

# ========================================================================
build_release() {
# ========================================================================
    echo
    echo -e "${BOLD}===== Building Release =====${NC}"
    configure
    cmake --build "${BUILD_DIR}" --config Release -j "${BUILD_JOBS}"

    local exe="${BUILD_DIR}/${PROJECT_NAME}"
    if [ -f "$exe" ]; then
        ok "Built: $exe"
    else
        # Multi-config generator (MSVC, Xcode)
        exe="${BUILD_DIR}/Release/${PROJECT_NAME}"
        [ -f "$exe" ] && ok "Built: $exe"
    fi
}

# ========================================================================
build_debug() {
# ========================================================================
    echo
    echo -e "${BOLD}===== Building Debug =====${NC}"
    configure
    cmake --build "${BUILD_DIR}" --config Debug -j "${BUILD_JOBS}"
    ok "Debug build complete."
}

# ========================================================================
build_next() {
# ========================================================================
    echo
    echo -e "${BOLD}===== Building ZX Spectrum Next =====${NC}"

    local zcc="zcc"
    if [ -n "${Z88DK_PATH:-}" ]; then
        zcc="${Z88DK_PATH}/bin/zcc"
        export PATH="${Z88DK_PATH}/bin:$PATH"
        # z88dk needs ZCCCFG
        if [ -d "${Z88DK_PATH}/lib/config" ]; then
            export ZCCCFG="${Z88DK_PATH}/lib/config"
        fi
    fi
    if ! command -v "$zcc" &>/dev/null; then
        die "z88dk not found. Set Z88DK_PATH or add z88dk/bin to PATH."
    fi
    info "z88dk: $(command -v "$zcc")"

    mkdir -p "${NEXT_BUILD_DIR}"

    # Makefile.next is designed to be run from its own directory
    # but we want to build out-of-tree, so copy and build there
    cp "${SCRIPT_DIR}/build/Makefile.next" "${NEXT_BUILD_DIR}/"
    pushd "${NEXT_BUILD_DIR}" > /dev/null
    make -f Makefile.next clean 2>/dev/null || true
    make -f Makefile.next
    popd > /dev/null

    if [ -f "${NEXT_BUILD_DIR}/game.nex" ]; then
        ok "Built: ${NEXT_BUILD_DIR}/game.nex"
    else
        die "Next build produced no output."
    fi
}

# ========================================================================
package_release() {
# ========================================================================
    echo
    info "Packaging..."
    mkdir -p "${DIST_DIR}"

    local exe=""
    for f in "${BUILD_DIR}/${PROJECT_NAME}" "${BUILD_DIR}/Release/${PROJECT_NAME}" \
             "${BUILD_DIR}/${PROJECT_NAME}.exe" "${BUILD_DIR}/Release/${PROJECT_NAME}.exe"; do
        [ -f "$f" ] && exe="$f" && break
    done
    if [ -z "$exe" ]; then
        warn "No executable found to package."
        return 1
    fi

    local stage="${DIST_DIR}/hal_engine_${PLATFORM}"
    rm -rf "$stage"
    mkdir -p "$stage"
    cp "$exe" "$stage/"
    [ -f "${SCRIPT_DIR}/README.md" ] && cp "${SCRIPT_DIR}/README.md" "$stage/"

    # Copy SDL2 dylib on macOS if present
    if [ "$PLATFORM" = "macos" ]; then
        local dylib
        for dylib in "${BUILD_DIR}"/libSDL2*.dylib; do
            [ -f "$dylib" ] && cp "$dylib" "$stage/" && break
        done
    fi

    local archive="hal_engine_${PLATFORM}.tar.gz"
    tar -czf "${DIST_DIR}/${archive}" -C "${DIST_DIR}" "hal_engine_${PLATFORM}"
    ok "${DIST_DIR}/${archive}"
}

# ========================================================================
package_next() {
# ========================================================================
    if [ ! -f "${NEXT_BUILD_DIR}/game.nex" ]; then
        warn "No Next build to package."
        return 1
    fi
    mkdir -p "${DIST_DIR}"
    local stage="${DIST_DIR}/hal_engine_next"
    rm -rf "$stage"
    mkdir -p "$stage"
    cp "${NEXT_BUILD_DIR}/game.nex" "$stage/"
    [ -f "${SCRIPT_DIR}/README.md" ] && cp "${SCRIPT_DIR}/README.md" "$stage/"
    tar -czf "${DIST_DIR}/hal_engine_next.tar.gz" -C "${DIST_DIR}" "hal_engine_next"
    ok "${DIST_DIR}/hal_engine_next.tar.gz"
}

# ========================================================================
run_game() {
# ========================================================================
    build_release
    echo
    info "Launching..."
    local exe=""
    for f in "${BUILD_DIR}/${PROJECT_NAME}" "${BUILD_DIR}/Release/${PROJECT_NAME}"; do
        [ -f "$f" ] && exe="$f" && break
    done
    if [ -z "$exe" ]; then
        die "Cannot find ${PROJECT_NAME} executable."
    fi
    exec "$exe"
}

# ========================================================================
do_clean() {
# ========================================================================
    echo
    echo -e "${BOLD}===== Cleaning =====${NC}"
    [ -d "${BUILD_DIR}" ]      && rm -rf "${BUILD_DIR}"      && ok "Removed ${BUILD_DIR}"
    [ -d "${NEXT_BUILD_DIR}" ] && rm -rf "${NEXT_BUILD_DIR}" && ok "Removed ${NEXT_BUILD_DIR}"
    [ -d "${DIST_DIR}" ]       && rm -rf "${DIST_DIR}"       && ok "Removed ${DIST_DIR}"
    ok "Clean complete."
}

# ========================================================================
# Main dispatch
# ========================================================================
detect_platform

CMD="${1:-release_build}"
case "$CMD" in
    help|-h|--help)   show_help ;;
    clean)            do_clean ;;
    debug)            build_debug ;;
    release)          build_release; package_release ;;
    next)             build_next ;;
    all)              build_release; build_next || true; package_release; package_next || true ;;
    run)              run_game ;;
    install-deps)     install_deps ;;
    *)                build_release ;;
esac
