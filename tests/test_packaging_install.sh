#!/usr/bin/env bash
# test_packaging_install.sh — Verify CMake install file layout (Task 41).
#
# Usage: tests/test_packaging_install.sh <install-prefix>
#
# Verifies that `cmake --install` (or `make DESTDIR=<prefix> install`)
# placed every required file in the correct location per SPEC §9.3
# and the Task 41 acceptance criteria.
#
# Exit codes: 0 = all checks pass, 1 = one or more checks failed.

set -eu

if [ "$#" -lt 1 ]; then
    echo "FAIL: usage: $0 <install-prefix>" >&2
    exit 1
fi

PREFIX="$1"
FAILURES=0

check_file() {
    local desc="$1"
    local path="$2"
    if [ -f "$path" ]; then
        echo "OK: $desc — $path"
    else
        echo "FAIL: $desc — missing: $path" >&2
        FAILURES=$((FAILURES + 1))
    fi
}

check_dir() {
    local desc="$1"
    local path="$2"
    if [ -d "$path" ]; then
        echo "OK: $desc — $path"
    else
        echo "FAIL: $desc — missing directory: $path" >&2
        FAILURES=$((FAILURES + 1))
    fi
}

check_contains() {
    local desc="$1"
    local file="$2"
    local pattern="$3"
    if grep -qF -- "$pattern" "$file" 2>/dev/null; then
        echo "OK: $desc"
    else
        echo "FAIL: $desc — '$pattern' not found in $file" >&2
        FAILURES=$((FAILURES + 1))
    fi
}

echo "=== Packaging install layout verification ==="
echo "Prefix: $PREFIX"
echo ""

# --- Binary -----------------------------------------------------------------
check_file "binary"          "${PREFIX}/usr/bin/controller-box"

# --- Data files --------------------------------------------------------------
check_dir  "icons directory" "${PREFIX}/usr/share/controller-box/icons/svg"
check_file "icon mapping"    "${PREFIX}/usr/share/controller-box/controller-icons.yaml"
check_file "Xbox 360 layout" "${PREFIX}/usr/share/controller-box/controller-layouts/xbox-360.json"
check_file "service file"    "${PREFIX}/usr/share/controller-box/controller-box.service"

# --- Desktop entry -----------------------------------------------------------
check_file "desktop entry"   "${PREFIX}/usr/share/applications/controller-box-manager.desktop"

# --- Service file content (SPEC §2.4) --------------------------------------
SVC="${PREFIX}/usr/share/controller-box/controller-box.service"
if [ -f "$SVC" ]; then
    check_contains "service: graphical ordering"  "$SVC" "After=graphical-session.target"
    check_contains "service: graphical lifecycle" "$SVC" "PartOf=graphical-session.target"
    check_contains "service: bounded restart"      "$SVC" "Restart=on-failure"
    check_contains "service: restart backoff"      "$SVC" "RestartSec=2s"
    check_contains "service: ExecStart overlay"     "$SVC" "--overlay-service"
else
    echo "SKIP: service file content checks (file missing)" >&2
    FAILURES=$((FAILURES + 1))
fi

# --- Desktop entry content --------------------------------------------------
DESK="${PREFIX}/usr/share/applications/controller-box-manager.desktop"
if [ -f "$DESK" ]; then
    check_contains "desktop: manager mode"  "$DESK" "--manager"
    check_contains "desktop: Type=Application" "$DESK" "Type=Application"
else
    echo "SKIP: desktop entry content checks (file missing)" >&2
    FAILURES=$((FAILURES + 1))
fi

# --- Icon SVGs present ------------------------------------------------------
SVG_COUNT=$(find "${PREFIX}/usr/share/controller-box/icons/svg" -name '*.svg' 2>/dev/null | wc -l)
if [ "$SVG_COUNT" -gt 0 ]; then
    echo "OK: icon SVGs present ($SVG_COUNT files)"
else
    echo "FAIL: no icon SVGs found" >&2
    FAILURES=$((FAILURES + 1))
fi

echo ""
if [ "$FAILURES" -eq 0 ]; then
    echo "PASS: all packaging install checks passed"
    exit 0
else
    echo "FAIL: $FAILURES check(s) failed" >&2
    exit 1
fi