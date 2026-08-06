#!/usr/bin/env bash
# Full Controller-Box verification used by the maintenance completion gate.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${CBX_VERIFY_BUILD_DIR:-build-maintenance-verify}
cd -- "$PROJECT_ROOT"

required=(sdl2 SDL2_ttf SDL2_image libsystemd yaml-0.1 cmocka)
if ! pkg-config --exists "${required[@]}"; then
    if [[ ${CBX_VERIFY_IN_NIX_SHELL:-0} != 1 ]] && command -v nix-shell >/dev/null; then
        export CBX_VERIFY_IN_NIX_SHELL=1
        exec nix-shell --run './scripts/verify-project.sh'
    fi
    echo "verify-project: required native dependencies are unavailable" >&2
    exit 2
fi

cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure
"$PROJECT_ROOT/tests/test-packaging.sh" "$BUILD_DIR"
echo "verify-project: Controller-Box build, tests, smoke checks, and packaging passed"
