#!/usr/bin/env bash
# Full Controller-Box verification used by the maintenance completion gate.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${CBX_VERIFY_BUILD_DIR:-build-maintenance-verify}
cd -- "$PROJECT_ROOT"

# The project gate is Nix-bound even when a host happens to provide similarly
# named development packages. Host package versions and feature defaults are
# not the declared verification environment.
if [[ ${CBX_VERIFY_IN_NIX_SHELL:-0} != 1 && -z ${IN_NIX_SHELL:-} ]] \
        && command -v nix-shell >/dev/null; then
    export CBX_VERIFY_IN_NIX_SHELL=1
    exec nix-shell --run './scripts/verify-project.sh'
fi

required=(sdl2 SDL2_ttf SDL2_image libsystemd yaml-0.1 cmocka)
if ! pkg-config --exists "${required[@]}"; then
    echo "verify-project: required native dependencies are unavailable" >&2
    exit 2
fi

cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD_DIR" --parallel ${CMAKE_BUILD_PARALLEL_LEVEL:-2}
ctest --test-dir "$BUILD_DIR" --output-on-failure
functional_log=$(mktemp)
trap 'rm -f "$functional_log"' EXIT
ctest --test-dir "$BUILD_DIR" --no-tests=error \
    -R '^test_installed_functional$' --output-on-failure | tee "$functional_log"
if grep -Eq 'Skipped|Not Run|0 tests passed' "$functional_log"; then
    echo "verify-project: installed functional acceptance was skipped" >&2
    exit 1
fi
"$PROJECT_ROOT/tests/test_packaging.sh" "$BUILD_DIR"
# Installed production smoke test (Task 9, §11.1.5):
# Exits 77 (skip) if Xvfb/xdotool/ImageMagick are unavailable.
"$PROJECT_ROOT/tests/test_installed_smoke.sh" "$BUILD_DIR" || \
    { rc=$?; if [ "$rc" -ne 77 ]; then echo "verify-project: installed smoke test failed (exit $rc)" >&2; exit 1; fi; }
mkdir -p .factory-state
cat > .factory-state/installed-functional-evidence.env <<EOF
schema=factory-installed-functional/v1
commit=$(git rev-parse HEAD)
test=test_installed_functional
result=PASS
skipped=0
EOF
echo "verify-project: Controller-Box build, tests, functional acceptance, smoke checks, and packaging passed"
