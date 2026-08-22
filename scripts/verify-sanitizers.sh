#!/usr/bin/env bash
# Build and run the test suite under AddressSanitizer + UndefinedBehaviorSanitizer.
#
# This script implements the sanitizer/static-analysis quality gate required
# by SPEC §11.2.5 (DOD-05).  It builds the project with
# -fsanitize=address,undefined and runs the full CTest suite, failing on any
# sanitizer report (memory errors, undefined behaviour) or test failure.
#
# Usage:  nix-shell --run './scripts/verify-sanitizers.sh'
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${CBX_SANITIZER_BUILD_DIR:-build-sanitizer}
cd -- "$PROJECT_ROOT"

# The project gate is Nix-bound (same rationale as verify-project.sh). The
# authenticated boundary (scripts/nix-gate.sh + scripts/nix-gate-exec.sh)
# replaces the forgeable CBX_VERIFY_IN_NIX_SHELL / IN_NIX_SHELL trust and FAILS
# rather than silently running against undeclared host packages when Nix is
# unavailable.
source "$PROJECT_ROOT/scripts/nix-gate.sh"
if ! nix_gate_require full; then
    exec "$PROJECT_ROOT/scripts/nix-gate-exec.sh" \
        "$PROJECT_ROOT/scripts/verify-sanitizers.sh" "$@"
fi

required=(sdl2 SDL2_ttf SDL2_image libsystemd yaml-0.1 cmocka)
if ! pkg-config --exists "${required[@]}"; then
    echo "verify-sanitizers: required native dependencies are unavailable" >&2
    exit 2
fi

# Fresh build — stale caches may not carry sanitizer flags.
rm -rf "$BUILD_DIR"

echo "=== Building with ASan + UBSan ==="
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCBX_ENABLE_SANITIZERS=ON
cmake --build "$BUILD_DIR" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"

echo "=== Running test suite under sanitizers ==="
# The Nix shell provides sdl2-compat (an SDL2→SDL3 compatibility shim) that
# dlopens libSDL3.so.0 at runtime.  ASan changes the dynamic linker's search
# path, preventing sdl2-compat from finding SDL3 via its RUNPATH.  Extract
# the SDL3 library directory from the sdl2-compat RUNPATH and add it to
# LD_LIBRARY_PATH so dlopen succeeds under sanitizers.
SDL2_LIB_PATH="$(pkg-config --variable=libdir sdl2 2>/dev/null)"
SDL3_LIB_DIR=""
if [[ -n "$SDL2_LIB_PATH" && -f "$SDL2_LIB_PATH/libSDL2-2.0.so.0" ]]; then
    SDL3_LIB_DIR="$(readelf -d "$SDL2_LIB_PATH/libSDL2-2.0.so.0" 2>/dev/null \
        | grep RUNPATH | sed 's/.*\[//' | sed 's/\]//' \
        | tr ':' '\n' | while read -r d; do test -f "$d/libSDL3.so.0" && echo "$d" && break; done)"
fi
if [[ -z "$SDL3_LIB_DIR" ]]; then
    # Fallback: locate via the regular (non-sanitized) binary's linked libs
    SDL3_LIB_DIR="$(ldd build-check/controller-box 2>/dev/null \
        | grep SDL2 | awk '{print $3}' | xargs dirname 2>/dev/null)"
    # If that gives us the sdl2-compat dir, check its RUNPATH for SDL3
    if [[ -n "$SDL3_LIB_DIR" && -f "$SDL3_LIB_DIR/libSDL2-2.0.so.0" ]]; then
        SDL3_LIB_DIR="$(readelf -d "$SDL3_LIB_DIR/libSDL2-2.0.so.0" 2>/dev/null \
            | grep RUNPATH | sed 's/.*\[//' | sed 's/\]//' \
            | tr ':' '\n' | while read -r d; do test -f "$d/libSDL3.so.0" && echo "$d" && break; done)"
    fi
fi
if [[ -n "$SDL3_LIB_DIR" && -f "$SDL3_LIB_DIR/libSDL3.so.0" ]]; then
    export LD_LIBRARY_PATH="${SDL3_LIB_DIR}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    echo "verify-sanitizers: added $SDL3_LIB_DIR to LD_LIBRARY_PATH for SDL3"
else
    echo "verify-sanitizers: WARNING — could not locate libSDL3.so.0; SDL tests may fail" >&2
fi

# ASan options:
#   detect_leaks=1  — enable leak detection (default, but explicit)
#   abort_on_error=1 — abort immediately on first sanitizer error so the
#     process exit code propagates as a test failure.
# UBSan options:
#   print_stacktrace=1 — include backtrace for UB diagnostics.
# LSAN_OPTIONS:
#   Suppressions for known third-party library leaks (harfbuzz font
#   shaping caches, SDL2_ttf/SDL2 global state) that are not our bugs.
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
export LSAN_OPTIONS=suppressions="$PROJECT_ROOT/scripts/lsan-suppressions.txt"

ctest --test-dir "$BUILD_DIR" --output-on-failure --timeout 120

echo "=== Sanitizer gate passed: no ASan/UBSan defects detected ==="