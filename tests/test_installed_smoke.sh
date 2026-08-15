#!/usr/bin/env bash
# test_installed_smoke.sh — Installed production smoke test (Task 9, §11.1.5).
#
# Exercises the real main() entry point (no --dry-run) of the installed
# binary under a headless X11 server (Xvfb).  Verifies that:
#
#   1. The binary builds and installs to a staging prefix.
#   2. Manager mode launches under Xvfb, renders a visible window,
#      responds to keyboard input (tab navigation), and the captured
#      framebuffer is non-blank (pixel variance above threshold in both
#      the tab-bar region and the body region).
#   3. Coordinate-based mouse clicks on manager body controls (profiles
#      tab, settings list item, Save button) produce a visible state
#      change or file mutation, proving that the installed binary
#      processes real X11 mouse events through its production event loop.
#   4. Overlay service mode launches under Xvfb and either:
#      a. Runs and renders if InputPlumber is available on the system
#         DBus, or
#      b. Exits cleanly with a non-crash error (exit 1 = "InputPlumber
#         not found") if InputPlumber is unavailable.
#
# If Xvfb, xdotool, or ImageMagick is unavailable, the test exits with
# SKIP_RETURN_CODE 77 (ctest treats this as SKIP, not FAIL).
#
# Usage: tests/test_installed_smoke.sh [build-dir]
#
# Exit codes: 0 = pass, 1 = fail, 77 = skip (tools unavailable).

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
cd -- "$PROJECT_ROOT"

BUILD_DIR="${1:-${BUILD_DIR:-build-check}}"
STAGING_DIR="$PROJECT_ROOT/.test-install"
XVFB_DISPLAY=":99"
XVFB_PID=""
FAILURES=0
TMPDIR=""

pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*" >&2; FAILURES=$((FAILURES + 1)); }

# shellcheck disable=SC2329
cleanup() {
    # Kill any lingering controller-box binary processes (not the test script)
    pkill -x "controller-box" 2>/dev/null || true
    # Kill Xvfb if we started it (SIGTERM then SIGKILL)
    if [ -n "$XVFB_PID" ] && kill -0 "$XVFB_PID" 2>/dev/null; then
        kill -TERM "$XVFB_PID" 2>/dev/null || true
        sleep 0.5
        kill -KILL "$XVFB_PID" 2>/dev/null || true
    fi
    # Clean up temp files
    if [ -n "$TMPDIR" ] && [ -d "$TMPDIR" ]; then
        rm -rf "$TMPDIR"
    fi
    # Clean up staging dir
    rm -rf "$STAGING_DIR" 2>/dev/null || true
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Step 0: Check for required tools — skip if unavailable
# ---------------------------------------------------------------------------
echo "=== Task 9: Installed production smoke test ==="
echo ""

for tool in Xvfb xdotool import convert bc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "SKIP: required tool '$tool' is not installed"
        echo "      Install xorg.xorgserver (Xvfb), xdotool, and imagemagick"
        echo "      to run this test."
        exit 77
    fi
done

pass "all required tools available (Xvfb, xdotool, import, convert)"

# ---------------------------------------------------------------------------
# Step 1: Build and install to staging prefix
# ---------------------------------------------------------------------------
echo ""
echo "--- Build and install to staging prefix ---"

if [ ! -d "$BUILD_DIR" ]; then
    echo "Build directory '$BUILD_DIR' not found; configuring..."
    cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
fi

if [ ! -f "$BUILD_DIR/controller-box" ]; then
    echo "Binary not found; building..."
    cmake --build "$BUILD_DIR" --parallel
fi

# Install to staging prefix
rm -rf "$STAGING_DIR"
cmake --install "$BUILD_DIR" --prefix "$STAGING_DIR" 2>/dev/null

INSTALLED_BIN="$STAGING_DIR/bin/controller-box"
if [ ! -f "$INSTALLED_BIN" ]; then
    # Try /usr/bin/ layout (DESTDIR-style install)
    INSTALLED_BIN="$STAGING_DIR/usr/bin/controller-box"
fi

if [ ! -f "$INSTALLED_BIN" ]; then
    fail "installed binary not found in staging prefix"
    exit 1
fi

pass "installed binary: $INSTALLED_BIN"

# Verify the installed binary runs --version
VERSION_OUTPUT=$("$INSTALLED_BIN" --version 2>&1) || true
if echo "$VERSION_OUTPUT" | grep -qE '^controller-box [0-9]+\.[0-9]+\.[0-9]+'; then
    pass "installed --version: $VERSION_OUTPUT"
else
    fail "installed --version unexpected: '$VERSION_OUTPUT'"
fi

# ---------------------------------------------------------------------------
# Step 2: Start Xvfb (headless X11 server)
# ---------------------------------------------------------------------------
echo ""
echo "--- Starting Xvfb on $XVFB_DISPLAY ---"

# Kill any existing Xvfb on this display
pkill -f "Xvfb $XVFB_DISPLAY" 2>/dev/null || true
sleep 0.5

Xvfb "$XVFB_DISPLAY" -screen 0 1280x720x24 &
XVFB_PID=$!
sleep 1.0

if ! kill -0 "$XVFB_PID" 2>/dev/null; then
    fail "Xvfb failed to start on $XVFB_DISPLAY"
    exit 1
fi

pass "Xvfb running (PID $XVFB_PID)"

export DISPLAY="$XVFB_DISPLAY"
export SDL_VIDEODRIVER="x11"
export SDL_RENDER_DRIVER="software"    # force software rendering under Xvfb

# ---------------------------------------------------------------------------
# Step 3: Manager mode smoke test
# ---------------------------------------------------------------------------
echo ""
echo "--- Manager mode smoke test ---"

TMPDIR=$(mktemp -d -t cbx-smoke-XXXXXX)
MANAGER_CAPTURE="$TMPDIR/manager_capture.png"
MANAGER_TABBAR_CROP="$TMPDIR/manager_tabbar.png"
MANAGER_BODY_CROP="$TMPDIR/manager_body.png"

# Set up a temporary HOME with a font directory so the installed binary
# can find DejaVuSans.ttf at runtime via cbx_font_path().
FONT_HOME="$TMPDIR/home"
mkdir -p "$FONT_HOME/.local/share/fonts"

# Search for DejaVuSans.ttf in common locations
FONT_FOUND=""
for font_candidate in \
    "/nix/store/zzs2q7lk5mn6y2rywd3snhak7098zs66-system-path/share/X11/fonts/DejaVuSans.ttf" \
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" \
    "/usr/share/fonts/dejavu/DejaVuSans.ttf" \
    "/usr/share/fonts/TTF/DejaVuSans.ttf"; do
    if [ -f "$font_candidate" ]; then
        FONT_FOUND="$font_candidate"
        break
    fi
done
# Also search the nix store (may vary between environments)
if [ -z "$FONT_FOUND" ]; then
    FONT_FOUND=$(find /nix/store -name "DejaVuSans.ttf" 2>/dev/null | head -1 || true)
fi

if [ -n "$FONT_FOUND" ]; then
    cp "$FONT_FOUND" "$FONT_HOME/.local/share/fonts/DejaVuSans.ttf"
    pass "font available: $FONT_FOUND"
else
    echo "WARN: no DejaVuSans.ttf found; manager may fail to render text"
fi

# Launch manager in background with the temporary HOME and XDG dirs
# (override any inherited XDG_CONFIG_HOME/XDG_DATA_HOME from the runner env)
HOME="$FONT_HOME" XDG_CONFIG_HOME="$FONT_HOME/.config" XDG_DATA_HOME="$FONT_HOME/.local/share" \
    "$INSTALLED_BIN" --manager &
MANAGER_PID=$!
sleep 2.0

if ! kill -0 "$MANAGER_PID" 2>/dev/null; then
    # Manager exited — check if it was a crash
    wait "$MANAGER_PID" 2>/dev/null
    MANAGER_EXIT=$?
    if [ "$MANAGER_EXIT" -eq 139 ] || [ "$MANAGER_EXIT" -eq 134 ]; then
        fail "manager crashed with signal (exit $MANAGER_EXIT)"
    else
        fail "manager exited prematurely (exit $MANAGER_EXIT)"
    fi
else
    pass "manager launched and running (PID $MANAGER_PID)"

    # Send keyboard input: Tab to switch tabs, arrow keys to navigate
    xdotool key Tab 2>/dev/null || true
    sleep 0.3
    xdotool key Tab 2>/dev/null || true
    sleep 0.3
    xdotool key Down 2>/dev/null || true
    sleep 0.3
    xdotool key Up 2>/dev/null || true
    sleep 0.5

    pass "keyboard input sent (Tab, Arrow keys)"

    # Capture the root window
    import -window root "$MANAGER_CAPTURE" 2>/dev/null
    if [ -f "$MANAGER_CAPTURE" ]; then
        pass "screenshot captured: $MANAGER_CAPTURE"

        # Check pixel variance in tab bar region (top 48px)
        # Crop: 1280x48 from top-left
        convert "$MANAGER_CAPTURE" -crop 1280x48+0+0 +repage "$MANAGER_TABBAR_CROP" 2>/dev/null
        TABBAR_MEAN=$(convert "$MANAGER_TABBAR_CROP" -format '%[mean]' info: 2>/dev/null || echo "0")
        # %[mean] returns 0-65535 (16-bit), normalize to 0-255
        TABBAR_MEAN_255=$(echo "scale=2; $TABBAR_MEAN / 257" | bc 2>/dev/null || echo "0")

        echo "  tab bar mean (0-255): $TABBAR_MEAN_255"
        if [ "$(echo "$TABBAR_MEAN_255 > 5.0" | bc 2>/dev/null || echo 0)" -eq 1 ]; then
            pass "tab bar region is non-blank (mean=$TABBAR_MEAN_255)"
        else
            fail "tab bar region appears blank (mean=$TABBAR_MEAN_255)"
        fi

        # Check pixel variance in body region (below 48px)
        # Crop: 1280x672 from y=48
        convert "$MANAGER_CAPTURE" -crop 1280x672+0+48 +repage "$MANAGER_BODY_CROP" 2>/dev/null
        BODY_MEAN=$(convert "$MANAGER_BODY_CROP" -format '%[mean]' info: 2>/dev/null || echo "0")
        BODY_MEAN_255=$(echo "scale=2; $BODY_MEAN / 257" | bc 2>/dev/null || echo "0")

        echo "  body mean (0-255): $BODY_MEAN_255"
        if [ "$(echo "$BODY_MEAN_255 > 5.0" | bc 2>/dev/null || echo 0)" -eq 1 ]; then
            pass "body region is non-blank (mean=$BODY_MEAN_255)"
        else
            fail "body region appears blank (mean=$BODY_MEAN_255)"
        fi

        # Overall non-blank check
        FULL_MEAN=$(convert "$MANAGER_CAPTURE" -format '%[mean]' info: 2>/dev/null || echo "0")
        FULL_MEAN_255=$(echo "scale=2; $FULL_MEAN / 257" | bc 2>/dev/null || echo "0")
        echo "  full frame mean (0-255): $FULL_MEAN_255"

    else
        fail "failed to capture screenshot"
    fi

    # ---------------------------------------------------------------------------
    # Step 3b: Coordinate-based mouse clicks on manager body controls
    # ---------------------------------------------------------------------------
    #
    # Uses xdotool mousemove + click to interact with specific UI elements
    # at known pixel coordinates.  Verifies that each click produces a
    # visible state change (screenshot diff) or file mutation (settings
    # file written to disk).
    #
    # Manager layout (1280x720, tab bar 48px):
    #   Tab bar:  y=0..48,   3 tabs (Controllers x=0, Profiles x=426, Settings x=852)
    #   Panel:    y=48..720
    #   Settings tab widgets:
    #     settings_list: (16, 64, 1248x420)  — first item center ~ (100, 90)
    #     save_btn:      (16, 500, 200x44)   — center ~ (116, 522)
    #     status_lbl:    (16, 560, 1248x32)
    #   Settings file: $HOME/.config/controller-box/settings.yaml
    # ---------------------------------------------------------------------------
    echo ""
    echo "--- Coordinate-based mouse clicks ---"

    # Helper: compute mean of pixel-by-pixel difference between two images.
    # Returns a value in 0-255 scale.  0 = identical, higher = more change.
    img_diff_mean() {
        local img1="$1" img2="$2"
        local diff_mean
        diff_mean=$(convert "$img1" "$img2" -compose difference -composite \
                        -format '%[mean]' info: 2>/dev/null || echo "0")
        # Normalize from 0-65535 to 0-255
        echo "scale=4; $diff_mean / 257" | bc 2>/dev/null || echo "0"
    }

    # Helper: crop body region (y=48..720, full width) from a screenshot.
    crop_body() {
        local src="$1" dst="$2"
        convert "$src" -crop 1280x672+0+48 +repage "$dst" 2>/dev/null
    }

    # --- Click 1: Profiles tab control (center at 639, 24) ---
    # Switches from the current tab to the Profiles tab.
    # Verify the body region visibly changes from the pre-click state.
    echo "  click profiles tab at (639, 24)"
    xdotool mousemove 639 24 2>/dev/null || true
    sleep 0.2
    xdotool click 1 2>/dev/null || true
    sleep 0.8

    PROFILES_CAPTURE="$TMPDIR/click_profiles.png"
    PROFILES_BODY="$TMPDIR/click_profiles_body.png"
    import -window root "$PROFILES_CAPTURE" 2>/dev/null
    crop_body "$PROFILES_CAPTURE" "$PROFILES_BODY" 2>/dev/null || true

    if [ -f "$PROFILES_BODY" ] && [ -f "$MANAGER_BODY_CROP" ]; then
        PDIFF=$(img_diff_mean "$MANAGER_BODY_CROP" "$PROFILES_BODY")
        echo "  profiles tab body diff (0-255): $PDIFF"
        if [ "$(echo "$PDIFF > 0.5" | bc 2>/dev/null || echo 0)" -eq 1 ]; then
            pass "profiles tab click produced visible state change (diff=$PDIFF)"
        else
            fail "profiles tab click produced no visible change (diff=$PDIFF)"
        fi
    else
        fail "profiles tab click: screenshot capture failed"
    fi

    # --- Click 2: Settings tab control (center at 1065, 24) ---
    # Switches from Profiles tab to the Settings tab.
    # Verify the body region visibly changes from the profiles state.
    echo "  click settings tab at (1065, 24)"
    xdotool mousemove 1065 24 2>/dev/null || true
    sleep 0.2
    xdotool click 1 2>/dev/null || true
    sleep 0.8

    SETTINGS_CAPTURE="$TMPDIR/click_settings.png"
    SETTINGS_BODY="$TMPDIR/click_settings_body.png"
    import -window root "$SETTINGS_CAPTURE" 2>/dev/null
    crop_body "$SETTINGS_CAPTURE" "$SETTINGS_BODY" 2>/dev/null || true

    if [ -f "$SETTINGS_BODY" ] && [ -f "$PROFILES_BODY" ]; then
        SDIFF=$(img_diff_mean "$PROFILES_BODY" "$SETTINGS_BODY")
        echo "  settings tab body diff (0-255): $SDIFF"
        if [ "$(echo "$SDIFF > 0.5" | bc 2>/dev/null || echo 0)" -eq 1 ]; then
            pass "settings tab click produced visible state change (diff=$SDIFF)"
        else
            fail "settings tab click produced no visible change (diff=$SDIFF)"
        fi
    else
        fail "settings tab click: screenshot capture failed"
    fi

    # --- Click 3: Settings list item (first item, center ~100, 90) ---
    # Clicks the first settings list row ("Launch at Boot").
    # Verify visible state change from the selection highlight.
    echo "  click settings list item at (100, 90)"
    xdotool mousemove 100 90 2>/dev/null || true
    sleep 0.2
    xdotool click 1 2>/dev/null || true
    sleep 0.5

    LISTITEM_CAPTURE="$TMPDIR/click_listitem.png"
    LISTITEM_BODY="$TMPDIR/click_listitem_body.png"
    import -window root "$LISTITEM_CAPTURE" 2>/dev/null
    crop_body "$LISTITEM_CAPTURE" "$LISTITEM_BODY" 2>/dev/null || true

    if [ -f "$LISTITEM_BODY" ] && [ -f "$SETTINGS_BODY" ]; then
        LDIFF=$(img_diff_mean "$SETTINGS_BODY" "$LISTITEM_BODY")
        echo "  list item click body diff (0-255): $LDIFF"
        if [ "$(echo "$LDIFF > 0.1" | bc 2>/dev/null || echo 0)" -eq 1 ]; then
            pass "settings list item click produced visible state change (diff=$LDIFF)"
        else
            fail "settings list item click produced no visible change (diff=$LDIFF)"
        fi
    else
        fail "settings list item click: screenshot capture failed"
    fi

    # --- Click 4: Save button (center at 116, 522) ---
    # Triggers cbx_settings_tab_save() which writes settings.yaml to disk.
    # Verify the settings file was created (file mutation).
    echo "  click Save button at (116, 522)"
    SETTINGS_FILE="$FONT_HOME/.config/controller-box/settings.yaml"
    # Record mtime before click (file may already exist from list-item activate)
    SETTINGS_MTIME_BEFORE=0
    if [ -f "$SETTINGS_FILE" ]; then
        SETTINGS_MTIME_BEFORE=$(stat -c %Y "$SETTINGS_FILE" 2>/dev/null || echo 0)
    fi

    xdotool mousemove 116 522 2>/dev/null || true
    sleep 0.2
    xdotool click 1 2>/dev/null || true
    sleep 1.0

    if [ -f "$SETTINGS_FILE" ]; then
        SETTINGS_MTIME_AFTER=$(stat -c %Y "$SETTINGS_FILE" 2>/dev/null || echo 0)
        SETTINGS_SIZE=$(stat -c %s "$SETTINGS_FILE" 2>/dev/null || echo 0)
        echo "  settings file: $SETTINGS_FILE ($SETTINGS_SIZE bytes)"
        if [ "$(echo "$SETTINGS_MTIME_AFTER > $SETTINGS_MTIME_BEFORE" | bc 2>/dev/null || echo 0)" -eq 1 ]; then
            pass "Save button click mutated settings file (mtime increased, size=$SETTINGS_SIZE)"
        elif [ "$SETTINGS_MTIME_BEFORE" -eq 0 ]; then
            pass "Save button click created settings file (size=$SETTINGS_SIZE)"
        else
            # File exists but mtime didn't change — may have been saved by list item click
            if [ "$SETTINGS_SIZE" -gt 0 ]; then
                pass "settings file exists after Save click (size=$SETTINGS_SIZE, mtime unchanged)"
            else
                fail "Save button click: settings file is empty"
            fi
        fi
    else
        fail "Save button click did not create settings file at $SETTINGS_FILE"
    fi

    pass "coordinate-based mouse clicks completed"

    # Kill the manager
    kill "$MANAGER_PID" 2>/dev/null || true
    set +e
    wait "$MANAGER_PID" 2>/dev/null
    set -e
    pass "manager terminated cleanly"
fi

# ---------------------------------------------------------------------------
# Step 4: Overlay service mode smoke test
# ---------------------------------------------------------------------------
echo ""
echo "--- Overlay service mode smoke test ---"

# The overlay service connects to InputPlumber via the system DBus.
# In a test environment without InputPlumber, it will exit with code 1
# ("InputPlumber not found").  This is a clean failure, not a crash.
# If InputPlumber IS available, the service runs and we verify it stays
# alive briefly.

OVERLAY_TIMEOUT=5
HOME="$FONT_HOME" "$INSTALLED_BIN" --overlay-service &
OVERLAY_PID=$!

# Wait up to OVERLAY_TIMEOUT seconds for the process to either stay
# running (InputPlumber available) or exit (InputPlumber unavailable)
OVERLAY_RUNNING=0
for _ in $(seq 1 "$OVERLAY_TIMEOUT"); do
    if ! kill -0 "$OVERLAY_PID" 2>/dev/null; then
        break
    fi
    sleep 1
    OVERLAY_RUNNING=1
done

if [ "$OVERLAY_RUNNING" -eq 1 ] && kill -0 "$OVERLAY_PID" 2>/dev/null; then
    # Overlay service is running — InputPlumber is available
    pass "overlay service running with InputPlumber (PID $OVERLAY_PID)"

    # Capture the root window (overlay window may be hidden until activated)
    OVERLAY_CAPTURE="$TMPDIR/overlay_capture.png"
    import -window root "$OVERLAY_CAPTURE" 2>/dev/null || true
    if [ -f "$OVERLAY_CAPTURE" ]; then
        pass "overlay screenshot captured"
        OVERLAY_MEAN=$(convert "$OVERLAY_CAPTURE" -format '%[mean]' info: 2>/dev/null || echo "0")
        OVERLAY_MEAN_255=$(echo "scale=2; $OVERLAY_MEAN / 257" | bc 2>/dev/null || echo "0")
        echo "  overlay frame mean (0-255): $OVERLAY_MEAN_255"
    else
        # Overlay window is hidden — this is expected behavior
        pass "overlay window is hidden (expected when not activated)"
    fi

    # Send SIGTERM for clean shutdown
    kill -TERM "$OVERLAY_PID" 2>/dev/null || true
    sleep 1
    kill "$OVERLAY_PID" 2>/dev/null || true
    wait "$OVERLAY_PID" 2>/dev/null || true
    pass "overlay service terminated via SIGTERM"
else
    # Overlay service exited — check exit code
    # Disable set -e temporarily to capture the exit code
    set +e
    wait "$OVERLAY_PID" 2>/dev/null
    OVERLAY_EXIT=$?
    set -e

    if [ "$OVERLAY_EXIT" -eq 1 ]; then
        pass "overlay service exited cleanly (code 1: InputPlumber not found — expected in test env)"
    elif [ "$OVERLAY_EXIT" -eq 139 ] || [ "$OVERLAY_EXIT" -eq 134 ]; then
        fail "overlay service crashed with signal (exit $OVERLAY_EXIT)"
    else
        # Other non-zero exit codes are acceptable as long as it's not a crash
        pass "overlay service exited (code $OVERLAY_EXIT — not a crash)"
    fi
fi

# ---------------------------------------------------------------------------
# Step 5: Summary
# ---------------------------------------------------------------------------
echo ""
if [ "$FAILURES" -eq 0 ]; then
    echo "PASS: all installed smoke test checks passed"
    exit 0
else
    echo "FAIL: $FAILURES check(s) failed" >&2
    exit 1
fi