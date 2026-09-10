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