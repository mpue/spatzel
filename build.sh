#!/usr/bin/env bash
#
# macOS build script for fitzel — configures and builds Debug and/or Release.
#
# On macOS the default generator (Ninja or Unix Makefiles) is single-config, so
# each configuration gets its own build tree (build/Debug, build/Release) and
# the build type is fixed at configure time. A multi-config generator (Xcode,
# "Ninja Multi-Config") is handled too: one tree serves every configuration and
# only --config differs per build.
#
# Usage:
#   ./build.sh                    # Debug + Release
#   ./build.sh --config Release   # Release only
#   ./build.sh --clean            # wipe the build tree(s) first
#   ./build.sh --no-vulkan        # OpenGL backend only
#   ./build.sh --generator Xcode  # multi-config Xcode project
#   ./build.sh --help
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
CONFIG=Both                 # Debug | Release | Both
BUILD_DIR=build
GENERATOR=''                # empty: Ninja if present, else Unix Makefiles
JOBS=0                      # 0 lets the generator pick
CMAKE_BIN=''                # explicit path to cmake, else looked up
GLSL_COMPILER=''            # explicit glslc/glslangValidator, else CMake finds it
CLEAN=0
NO_VULKAN=0
NO_OPENGL=0

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
# Pretty output — colour only when writing to a terminal.
# ---------------------------------------------------------------------------
if [[ -t 1 ]]; then
    C_CYAN=$'\033[36m'; C_GRAY=$'\033[90m'; C_YELLOW=$'\033[33m'
    C_GREEN=$'\033[32m'; C_RED=$'\033[31m'; C_RESET=$'\033[0m'
else
    C_CYAN=''; C_GRAY=''; C_YELLOW=''; C_GREEN=''; C_RED=''; C_RESET=''
fi

info()  { printf '%s==> %s%s\n' "$C_CYAN"   "$1" "$C_RESET"; }
warn()  { printf '%sWarning: %s%s\n' "$C_YELLOW" "$1" "$C_RESET" >&2; }
die()   { printf '%sError: %s%s\n'   "$C_RED"    "$1" "$C_RESET" >&2; exit 1; }

usage() {
    # Print the header comment block (the lines between the shebang and `set`),
    # stripping the leading "# ". Uses awk for BSD/GNU portability.
    awk 'NR==1{next} /^set -euo/{exit} {sub(/^# ?/,""); print}' "${BASH_SOURCE[0]}"
    exit 0
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--config)        CONFIG="${2:?--config needs a value}"; shift 2 ;;
        -b|--build-dir)     BUILD_DIR="${2:?--build-dir needs a value}"; shift 2 ;;
        -g|--generator)     GENERATOR="${2:?--generator needs a value}"; shift 2 ;;
        -j|--jobs)          JOBS="${2:?--jobs needs a value}"; shift 2 ;;
        --cmake)            CMAKE_BIN="${2:?--cmake needs a value}"; shift 2 ;;
        --glsl-compiler)    GLSL_COMPILER="${2:?--glsl-compiler needs a value}"; shift 2 ;;
        --clean)            CLEAN=1; shift ;;
        --no-vulkan)        NO_VULKAN=1; shift ;;
        --no-opengl)        NO_OPENGL=1; shift ;;
        -h|--help)          usage ;;
        *)                  die "unknown argument '$1' (try --help)" ;;
    esac
done

case "$CONFIG" in
    Debug|Release|Both) ;;
    debug)   CONFIG=Debug ;;
    release) CONFIG=Release ;;
    both)    CONFIG=Both ;;
    *) die "--config must be Debug, Release or Both (got '$CONFIG')" ;;
esac

[[ "$BUILD_DIR" = /* ]] || BUILD_DIR="$ROOT/$BUILD_DIR"

# ---------------------------------------------------------------------------
# Locate cmake — PATH first, then the usual macOS install locations. CMake is
# frequently installed via the CMake.app bundle or Homebrew without ending up
# on PATH.
# ---------------------------------------------------------------------------
resolve_cmake() {
    if [[ -n "$CMAKE_BIN" ]]; then
        [[ -x "$CMAKE_BIN" ]] || die "cmake not found or not executable at '$CMAKE_BIN'."
        printf '%s\n' "$CMAKE_BIN"; return
    fi
    if command -v cmake >/dev/null 2>&1; then
        command -v cmake; return
    fi
    local c
    for c in \
        /Applications/CMake.app/Contents/bin/cmake \
        /opt/homebrew/bin/cmake \
        /usr/local/bin/cmake
    do
        [[ -x "$c" ]] && { printf '%s\n' "$c"; return; }
    done
    die "cmake was not found. Install it (brew install cmake), pass --cmake <path>, or add it to PATH."
}

CMAKE="$(resolve_cmake)"
printf '%sUsing %s%s\n' "$C_GRAY" "$CMAKE" "$C_RESET"

# ---------------------------------------------------------------------------
# Generator — Ninja when available (much faster incremental builds), otherwise
# Unix Makefiles. Both are single-config. A generator name containing
# "Multi-Config" or equal to "Xcode" is multi-config and takes the one-tree path.
# ---------------------------------------------------------------------------
if [[ -z "$GENERATOR" ]]; then
    if command -v ninja >/dev/null 2>&1; then
        GENERATOR='Ninja'
    else
        GENERATOR='Unix Makefiles'
    fi
fi

MULTI_CONFIG=0
if [[ "$GENERATOR" == "Xcode" || "$GENERATOR" == *"Multi-Config"* ]]; then
    MULTI_CONFIG=1
fi

# ---------------------------------------------------------------------------
# FetchContent clones dependencies with git. On volumes that do not record
# ownership git can refuse to touch a fresh clone with "dubious ownership".
# Relax it for the child processes we spawn only — this never touches the
# user's global git config.
# ---------------------------------------------------------------------------
if [[ -z "${GIT_CONFIG_COUNT:-}" ]]; then
    export GIT_CONFIG_COUNT=1
    export GIT_CONFIG_KEY_0=safe.directory
    export GIT_CONFIG_VALUE_0='*'
fi

# ---------------------------------------------------------------------------
# Backend flags shared by every configure invocation.
# ---------------------------------------------------------------------------
backend_flags=()
backend_flags+=("-DFITZEL_WITH_VULKAN:BOOL=$([[ $NO_VULKAN -eq 1 ]] && echo OFF || echo ON)")
backend_flags+=("-DFITZEL_WITH_OPENGL:BOOL=$([[ $NO_OPENGL -eq 1 ]] && echo OFF || echo ON)")

if [[ -n "$GLSL_COMPILER" ]]; then
    [[ -x "$GLSL_COMPILER" ]] || die "GLSL compiler not found or not executable at '$GLSL_COMPILER'."
    backend_flags+=("-DFITZEL_GLSL_COMPILER:FILEPATH=$GLSL_COMPILER")
elif [[ $NO_VULKAN -eq 0 || $NO_OPENGL -eq 0 ]]; then
    # Both backends need a GLSL compiler at the shader step. On macOS glslc ships
    # with the Vulkan SDK (which also provides MoltenVK, needed to run the Vulkan
    # backend). Warn before CMake spends minutes cloning dependencies first.
    if ! command -v glslc >/dev/null 2>&1 \
       && ! command -v glslangValidator >/dev/null 2>&1 \
       && [[ -z "${VULKAN_SDK:-}" || ! -d "${VULKAN_SDK:-/nonexistent}" ]]; then
        warn "no glslc/glslangValidator on PATH and VULKAN_SDK is unset."
        warn "Configure will fail at the shader step. Install the Vulkan SDK (which"
        warn "also provides MoltenVK for the Vulkan backend), or pass --glsl-compiler <path>."
    fi
fi

run_cmake() {
    local label="$1"; shift
    printf '\n'
    info "$label"
    printf '%s    cmake %s%s\n' "$C_GRAY" "$*" "$C_RESET"
    "$CMAKE" "$@" || die "$label failed (exit code $?)."
}

# The parallel level: honour --jobs, else let the generator decide.
build_parallel_args=()
if [[ "$JOBS" -gt 0 ]]; then
    build_parallel_args+=(--parallel "$JOBS")
else
    build_parallel_args+=(--parallel)
fi

# ---------------------------------------------------------------------------
# Configure + build one configuration in its own tree (single-config path).
# ---------------------------------------------------------------------------
build_single_config() {
    local cfg="$1"
    local tree="$BUILD_DIR/$cfg"

    [[ $CLEAN -eq 1 && -d "$tree" ]] && { info "Removing $tree"; rm -rf "$tree"; }

    run_cmake "Configure $cfg" \
        -S "$ROOT" -B "$tree" -G "$GENERATOR" \
        "-DCMAKE_BUILD_TYPE=$cfg" \
        "${backend_flags[@]}"

    run_cmake "Build $cfg" --build "$tree" "${build_parallel_args[@]}"
}

# ---------------------------------------------------------------------------
# Configure once, build each configuration (multi-config path).
# ---------------------------------------------------------------------------
build_multi_config() {
    local -a configs=("$@")

    [[ $CLEAN -eq 1 && -d "$BUILD_DIR" ]] && { info "Removing $BUILD_DIR"; rm -rf "$BUILD_DIR"; }

    run_cmake "Configure" \
        -S "$ROOT" -B "$BUILD_DIR" -G "$GENERATOR" \
        "${backend_flags[@]}"

    local cfg
    for cfg in "${configs[@]}"; do
        run_cmake "Build $cfg" --build "$BUILD_DIR" --config "$cfg" "${build_parallel_args[@]}"
    done
}

# ---------------------------------------------------------------------------
# Drive it
# ---------------------------------------------------------------------------
if [[ "$CONFIG" == "Both" ]]; then
    configs=(Debug Release)
else
    configs=("$CONFIG")
fi

printf '%sGenerator: %s%s\n' "$C_GRAY" "$GENERATOR" "$C_RESET"

if [[ $MULTI_CONFIG -eq 1 ]]; then
    build_multi_config "${configs[@]}"
else
    for cfg in "${configs[@]}"; do
        build_single_config "$cfg"
    done
fi

# ---------------------------------------------------------------------------
# Report the binaries produced.
# ---------------------------------------------------------------------------
printf '\n%sBuild finished.%s\n' "$C_GREEN" "$C_RESET"
for cfg in "${configs[@]}"; do
    if [[ $MULTI_CONFIG -eq 1 ]]; then
        bin_dir="$BUILD_DIR/bin/$cfg"
        [[ -d "$bin_dir" ]] || bin_dir="$BUILD_DIR/bin"
    else
        bin_dir="$BUILD_DIR/$cfg/bin"
    fi
    [[ -d "$bin_dir" ]] || continue
    # Report the executable(s): Mach-O files or .app bundles under bin/.
    while IFS= read -r exe; do
        printf '  %-8s %s\n' "$cfg" "$exe"
    done < <(find "$bin_dir" -maxdepth 1 \( -type f -perm -u+x -o -name '*.app' \) 2>/dev/null | sort)
done
