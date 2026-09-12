#!/usr/bin/env bash
# verify.sh — project verification entry point.
#
# Builds the project and runs the full CTest suite. Tests that require
# hardware not available on the current machine skip (exit 77).
#
# The factory harness calls this as the per-runner verify_command and as
# the overall verification.command. Runner capabilities in
# .factory/environment.toml determine which runner executes which tasks;
# this script runs identically on all runners — the tests themselves
# decide what to skip based on the environment.
set -euo pipefail

cd -- "$(dirname -- "$0")/.."

BUILD_DIR=${CBX_BUILD_DIR:-build}
PROJECT_ROOT=$(cd -- "$PWD" && pwd -P)

# Self-heal a stale CMake cache. CMake records the source root as an
# INTERNAL CACHE variable (CMAKE_HOME_DIRECTORY) at first configure; once the
# project is checked out (or moved) to a different absolute path, calling
# `cmake -B build` against the old cache aborts with a source/binary-dir
# mismatch instead of reconfiguring. The canonical build/ tree is a build
# artifact, so when its cache pins a home directory other than the real
# project root we simply drop the tree and let the configure step rebuild it
# from scratch. This makes the configure+build+test sequence reproducible from
# a clean checkout regardless of prior build history.
if [ "$BUILD_DIR" = "build" ] && \
   [ -f "$BUILD_DIR/CMakeCache.txt" ] && \
   ! grep -q "^CMAKE_HOME_DIRECTORY:INTERNAL=$PROJECT_ROOT\$" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
    echo "verify.sh: canonical $BUILD_DIR cache pinned to a stale source root; rebuilding tree" >&2
    cmake -E remove_directory "$BUILD_DIR"
fi

# Use nix-shell if available (provides all build dependencies via shell.nix).
# Otherwise assume system-installed packages.
if command -v nix-shell >/dev/null 2>&1 && [ -f shell.nix ]; then
    exec nix-shell --run \
        "cmake -B '$BUILD_DIR' -DCMAKE_BUILD_TYPE=Debug && \
         cmake --build '$BUILD_DIR' -j\$(nproc) && \
         ctest --test-dir '$BUILD_DIR' --output-on-failure --timeout 120"
fi

# Fallback: system packages
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD_DIR" -j"$(nproc)"
ctest --test-dir "$BUILD_DIR" --output-on-failure --timeout 120
