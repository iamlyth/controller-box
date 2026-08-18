#!/usr/bin/env bash
# test_packaging.sh — End-to-end packaging integration test (Task 43).
#
# Verifies the full packaging pipeline:
#   1. CMake configure + build (if build dir not supplied)
#   2. `cmake --install` to a temporary DESTDIR staging area
#   3. File layout verification (delegates to test_packaging_install.sh)
#   4. Binary runs: `controller-box --version` prints the build version
#   5. Binary runs: `--dry-run` in both modes (overlay + manager) succeeds
#   6. Version string matches the CMake project(VERSION ...)
#   7. (Optional) Flatpak build if flatpak-builder is available
#
# Usage: tests/test_packaging.sh [build-dir]
#   build-dir: existing CMake build directory (default: ./build)
#
# Environment:
#   CBX_BUILD_CMD: command to wrap the build (e.g. "nix-shell --run")
#                  for environments requiring dependency isolation.
#
# Exit codes: 0 = all checks pass, 1 = one or more checks failed.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
cd -- "$PROJECT_ROOT"

BUILD_DIR="${1:-${BUILD_DIR:-build}}"
FAILURES=0
TMPDIR=""

pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*" >&2; FAILURES=$((FAILURES + 1)); }

# shellcheck disable=SC2329  # invoked via trap EXIT
cleanup() {
    if [ -n "$TMPDIR" ] && [ -d "$TMPDIR" ]; then
        rm -rf "$TMPDIR"
    fi
}
trap cleanup EXIT

# --- Helper: wrap a command with nix-shell if needed ------------------------
run_cmd() {
    if [ -n "${CBX_BUILD_CMD:-}" ]; then
        eval "$CBX_BUILD_CMD \"${*}\""
    else
        "$@"
    fi
}

# --- Step 1: Ensure build exists --------------------------------------------
echo "=== Task 43: Packaging integration test ==="
echo ""

if [ ! -d "$BUILD_DIR" ]; then
    echo "Build directory '$BUILD_DIR' not found; running cmake configure + build..."
    run_cmd cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
fi

if [ ! -f "$BUILD_DIR/controller-box" ]; then
    echo "Binary not found in '$BUILD_DIR'; building..."
    run_cmd cmake --build "$BUILD_DIR"
fi

if [ ! -f "$BUILD_DIR/controller-box" ]; then
    fail "binary not built: $BUILD_DIR/controller-box"
    echo ""
    echo "FAIL: $FAILURES check(s) failed" >&2
    exit 1
fi

pass "binary built: $BUILD_DIR/controller-box"

# --- Step 2: Install to temporary DESTDIR staging area ----------------------
TMPDIR=$(mktemp -d -t cbx-packaging-XXXXXX)
STAGING="$TMPDIR/install"

echo "Installing to staging: $STAGING"
DESTDIR="$STAGING" cmake --install "$BUILD_DIR" 2>/dev/null || {
    # cmake --install with DESTDIR works from the build dir
    # Try alternative if first attempt fails
    cmake --install "$BUILD_DIR" --prefix "$STAGING" 2>/dev/null || true
}

# Determine the install prefix layout
if [ -f "$STAGING/usr/bin/controller-box" ]; then
    INSTALL_PREFIX="$STAGING"
elif [ -f "$STAGING/bin/controller-box" ]; then
    INSTALL_PREFIX="$STAGING"
else
    fail "no installed binary found in staging area"
    echo ""
    echo "FAIL: $FAILURES check(s) failed" >&2
    exit 1
fi

pass "installed to staging: $INSTALL_PREFIX"

# --- Step 3: File layout verification ---------------------------------------
# Delegate to the existing install layout checker.
INSTALL_CHECKER="$SCRIPT_DIR/test_packaging_install.sh"
if [ -x "$INSTALL_CHECKER" ] || [ -f "$INSTALL_CHECKER" ]; then
    echo ""
    echo "--- File layout verification ---"
    if bash "$INSTALL_CHECKER" "$INSTALL_PREFIX"; then
        pass "file layout checks passed"
    else
        fail "file layout checks failed"
    fi
else
    fail "install layout checker not found: $INSTALL_CHECKER"
fi

# --- Step 4: Binary runs — --version ---------------------------------------
echo ""
echo "--- Binary execution tests ---"

INSTALLED_BIN="$INSTALL_PREFIX/usr/bin/controller-box"
if [ ! -f "$INSTALLED_BIN" ]; then
    INSTALLED_BIN="$INSTALL_PREFIX/bin/controller-box"
fi

VERSION_OUTPUT=$("$INSTALLED_BIN" --version 2>&1) || true
if echo "$VERSION_OUTPUT" | grep -qE '^controller-box [0-9]+\.[0-9]+\.[0-9]+'; then
    pass "--version: $VERSION_OUTPUT"
else
    fail "--version output unexpected: '$VERSION_OUTPUT'"
fi

# --- Step 5: --version works in both modes -----------------------------------
# --version is checked before mode dispatch, so it exits before mode
# selection regardless of any mode flag. Verify it works alongside both.
for mode_flag in "--overlay-service" "--manager"; do
    out=$("$INSTALLED_BIN" "$mode_flag" --version 2>&1) || true
    if echo "$out" | grep -qE '^controller-box [0-9]+\.[0-9]+\.[0-9]+'; then
        pass "--version with $mode_flag"
    else
        fail "--version with $mode_flag: unexpected output '$out'"
    fi
done

# --- Step 6: --dry-run in both modes ----------------------------------------
# These should print the mode banner and exit 0.
for mode_flag in "--overlay-service" "--manager"; do
    out=$("$INSTALLED_BIN" "$mode_flag" --dry-run 2>&1) || true
    if echo "$out" | grep -q "mode (dry-run)"; then
        pass "$mode_flag --dry-run"
    else
        fail "$mode_flag --dry-run: unexpected output '$out'"
    fi
done

# --- Step 7: Version matches CMake project(VERSION ...) ---------------------
CMAKE_VERSION=$(grep -oP 'VERSION\s+\K[0-9]+\.[0-9]+\.[0-9]+' "$PROJECT_ROOT/CMakeLists.txt" | head -1 || echo "")
BINARY_VERSION=$(echo "$VERSION_OUTPUT" | grep -oP 'controller-box \K[0-9]+\.[0-9]+\.[0-9]+' || echo "")

if [ -n "$CMAKE_VERSION" ] && [ "$CMAKE_VERSION" = "$BINARY_VERSION" ]; then
    pass "version match: CMake=$CMAKE_VERSION binary=$BINARY_VERSION"
elif [ -n "$CMAKE_VERSION" ]; then
    fail "version mismatch: CMake=$CMAKE_VERSION binary=$BINARY_VERSION"
else
    fail "could not extract version from CMakeLists.txt"
fi

# --- Step 8: Build from source tarball (simulated) --------------------------
# Verify that a clean configure+build from a tarball-like layout works.
# We use the existing source tree as a proxy (CMake source tree == tarball).
echo ""
echo "--- Clean configure test ---"
CLEAN_BUILD="$TMPDIR/clean-build"
if run_cmd cmake -B "$CLEAN_BUILD" -DCMAKE_BUILD_TYPE=Release "$PROJECT_ROOT" 2>/dev/null; then
    if run_cmd cmake --build "$CLEAN_BUILD" 2>/dev/null; then
        if [ -f "$CLEAN_BUILD/controller-box" ]; then
            CLEAN_VERSION=$("$CLEAN_BUILD/controller-box" --version 2>&1) || true
            if echo "$CLEAN_VERSION" | grep -qE '^controller-box [0-9]+\.[0-9]+\.[0-9]+'; then
                pass "clean build --version: $CLEAN_VERSION"
            else
                fail "clean build --version unexpected: '$CLEAN_VERSION'"
            fi
        else
            fail "clean build did not produce binary"
        fi
    else
        fail "clean build failed (cmake --build)"
    fi
else
    fail "clean configure failed"
fi

# --- Step 9: Flatpak build (optional locally, mandatory for runner contract) -
# flatpak-builder is an optional dependency. When unavailable, this step is
# a documented optional gate, not a silent skip. See docs/PACKAGING.md for
# installation instructions.
echo ""
echo "--- Flatpak build (optional locally; mandatory runner contract) ---"
REQUIRE_FLATPAK=${CBX_REQUIRE_FLATPAK:-0}
if [[ "$REQUIRE_FLATPAK" != 0 && "$REQUIRE_FLATPAK" != 1 ]]; then
    fail "CBX_REQUIRE_FLATPAK must be 0 or 1"
    REQUIRE_FLATPAK=1
fi
if [[ "$REQUIRE_FLATPAK" == 0 ]]; then
    echo "OPTIONAL: Flatpak execution is deferred to the isolated installed-package runner contract."
elif command -v flatpak-builder >/dev/null 2>&1; then
    FLATPAK_MANIFEST="$PROJECT_ROOT/packaging/org.shadowblip.ControllerBox.yaml"
    FLATPAK_BUILD="$TMPDIR/flatpak-build"
    FLATPAK_STATE="$TMPDIR/flatpak-state"

    echo "flatpak-builder found; running required clean build gate..."
    if flatpak-builder --user --install --force-clean \
            --state-dir="$FLATPAK_STATE" \
            "$FLATPAK_BUILD" "$FLATPAK_MANIFEST" 2>&1; then
        FLATPAK_OUT=$(flatpak run org.shadowblip.ControllerBox --version 2>&1) || true
        if echo "$FLATPAK_OUT" | grep -qE '^controller-box [0-9]+\.[0-9]+\.[0-9]+'; then
            pass "flatpak --version: $FLATPAK_OUT"
        else
            fail "flatpak --version unexpected: '$FLATPAK_OUT'"
        fi
    else
        fail "required Flatpak build failed"
    fi
else
    fail "required flatpak-builder is unavailable"
fi

# --- Summary ---------------------------------------------------------------
echo ""
if [ "$FAILURES" -eq 0 ]; then
    echo "PASS: all packaging integration checks passed"
    exit 0
else
    echo "FAIL: $FAILURES check(s) failed" >&2
    exit 1
fi