#!/usr/bin/env bash
# verify.sh — project verification entry point.
#
# Builds the project and runs the full CTest suite.  Tests that require
# hardware not available on the current machine skip (exit 77).
#
# The factory harness calls this as the per-runner verify_command and as
# the overall verification.command.  Runner capabilities in
# .factory/environment.toml determine which runner executes which tasks;
# this script runs identically on all runners — the tests themselves
# decide what to skip based on the environment.
set -euo pipefail

cd -- "$(dirname -- "$0")/.."

BUILD_DIR=${CBX_BUILD_DIR:-build}

cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD_DIR" -j"$(nproc)"
ctest --test-dir "$BUILD_DIR" --output-on-failure --timeout 120