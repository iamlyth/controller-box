#!/usr/bin/env bash
# test_installed_diagram.sh — Installed production-window controller diagram
# acceptance (BUG-0014, Task 5).
#
# Exercises the real installed `controller-box --manager` binary under a real
# X11 window server (Xvfb), drives it through production pointer dispatch to
# the Profiles tab and then the profile editor, and asserts that the editor's
# controller diagram region contains recognizable diagram content — the
# controller outline SVG (black silhouette on the panel), a slot highlight,
# the profile model label, and the binding list — rather than a blank/flat
# panel, a non-NULL-texture proxy, or a broad pixel-count change.
#
# This is a semantic production-window assertion, not an offscreen pixel
# readback: the installed binary renders to a real window through the real
# SDL event loop, and the assertions require the diagram to be perceptibly
# present.
#
# If Xvfb, xdotool, or ImageMagick is unavailable the test exits with
# SKIP_RETURN_CODE 77 (ctest treats this as SKIP, not FAIL).
#
# Exit codes: 0 = pass, 1 = fail, 77 = skip (tools unavailable).

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
cd -- "$PROJECT_ROOT"

BUILD_DIR="${1:-${BUILD_DIR:-build-check}}"
STAGING_DIR="$PROJECT_ROOT/.test-install-diagram"
# Dedicated custom-prefix build directory (AGENTS.md): the installed
# production-path diagram test must exercise the installed layout, so the
# binary is configured with CMAKE_INSTALL_PREFIX=$STAGING_DIR.  This makes
# ICON_DIR resolve to the installed share tree at runtime instead of falling
# back to SOURCE_ICON_DIR (which would be a production-path bypass).
PREFIX_BUILD_DIR="$PROJECT_ROOT/.build-install-diagram"
XVFB_DISPLAY=":93"
XVFB_PID=""
MANAGER_PID=""
FAILURES=0
TMPDIR=""

pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*" >&2; FAILURES=$((FAILURES + 1)); }

# shellcheck disable=SC2329  # invoked indirectly via `trap cleanup EXIT`
cleanup() {
    if [ -n "$MANAGER_PID" ] && kill -0 "$MANAGER_PID" 2>/dev/null; then
        kill -TERM "$MANAGER_PID" 2>/dev/null || true
        sleep 0.3
        kill -KILL "$MANAGER_PID" 2>/dev/null || true
    fi
    if [ -n "$XVFB_PID" ] && kill -0 "$XVFB_PID" 2>/dev/null; then
        kill -TERM "$XVFB_PID" 2>/dev/null || true
        sleep 0.5
        kill -KILL "$XVFB_PID" 2>/dev/null || true
    fi
    if [ -n "$TMPDIR" ] && [ -d "$TMPDIR" ]; then
        rm -rf "$TMPDIR"
    fi
    rm -rf "$STAGING_DIR" "$PREFIX_BUILD_DIR" 2>/dev/null || true
}
trap cleanup EXIT

# --- Step 0: tools -----------------------------------------------------------
for tool in Xvfb xdotool import convert; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "SKIP: required tool '$tool' is not installed"
        exit 77
    fi
done

# --- Step 1: build + install to staging prefix --------------------------------
# Configure a dedicated build with CMAKE_INSTALL_PREFIX=$STAGING_DIR so the
# installed binary's ICON_DIR points at the installed data directory.  This is
# the only way the installed production path can be exercised without a
# source-tree fallback (BUG-0014, Task 5): installing a default-prefix build
# only relocates files while the binary still resolves ICON_DIR to /usr/share.
rm -rf "$PREFIX_BUILD_DIR" "$STAGING_DIR"
if cmake -S . -B "$PREFIX_BUILD_DIR" \
        -DCMAKE_INSTALL_PREFIX="$STAGING_DIR" \
        -DCMAKE_BUILD_TYPE=Debug >/dev/null 2>&1; then
    :
else
    fail "custom-prefix cmake configure failed"
    exit 1
fi
if ! cmake --build "$PREFIX_BUILD_DIR" --parallel >/dev/null 2>&1; then
    fail "custom-prefix cmake build failed"
    exit 1
fi
INSTALL_OK=0
for attempt in 1 2 3; do
    if cmake --install "$PREFIX_BUILD_DIR" >/dev/null 2>&1; then
        INSTALL_OK=1
        break
    fi
    echo "WARN: cmake --install attempt $attempt failed; retrying..."
    sleep 1
done
if [ "$INSTALL_OK" -ne 1 ]; then
    fail "cmake --install failed after 3 attempts"
    exit 1
fi
# The custom-prefix install must deliver the diagram asset to the installed
# data layout.  If it is missing, the binary would fall back to the source
# tree, which the acceptance must reject.
INSTALLED_SVG="$STAGING_DIR/share/controller-box/icons/svg/generic-gamepad.svg"
if [ ! -f "$INSTALLED_SVG" ]; then
    fail "installed layout missing controller SVG: $INSTALLED_SVG"
    exit 1
fi
INSTALLED_BIN="$STAGING_DIR/bin/controller-box"
if [ ! -f "$INSTALLED_BIN" ]; then
    INSTALLED_BIN="$STAGING_DIR/usr/bin/controller-box"
fi
if [ ! -f "$INSTALLED_BIN" ]; then
    fail "installed binary not found in staging prefix"
    exit 1
fi

# --- Step 2: start Xvfb ------------------------------------------------------
pkill -f "Xvfb $XVFB_DISPLAY" 2>/dev/null || true
sleep 0.5
Xvfb "$XVFB_DISPLAY" -screen 0 1280x720x24 &
XVFB_PID=$!
sleep 1.0
if ! kill -0 "$XVFB_PID" 2>/dev/null; then
    fail "Xvfb failed to start on $XVFB_DISPLAY"
    exit 1
fi
export DISPLAY="$XVFB_DISPLAY"
export SDL_VIDEODRIVER="x11"
export SDL_RENDER_DRIVER="software"

# --- Step 3: isolated HOME with font + service + a default profile ------------
TMPDIR=$(mktemp -d -t cbx-diagram-XXXXXX)
FONT_HOME="$TMPDIR/home"
mkdir -p "$FONT_HOME/.local/share/fonts"
mkdir -p "$FONT_HOME/.config/systemd/user"
# Pre-create the service file so the first-run dialog (SPEC §9.1) does not
# block interaction in this test.
touch "$FONT_HOME/.config/systemd/user/controller-box.service"
FONT_FOUND=""
for c in \
    "/nix/store/zzs2q7lk5mn6y2rywd3snhak7098zs66-system-path/share/X11/fonts/DejaVuSans.ttf" \
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"; do
    if [ -f "$c" ]; then FONT_FOUND="$c"; break; fi
done
if [ -z "$FONT_FOUND" ]; then
    FONT_FOUND=$(find /nix/store -name "DejaVuSans.ttf" 2>/dev/null | head -1 || true)
fi
if [ -n "$FONT_FOUND" ]; then
    cp "$FONT_FOUND" "$FONT_HOME/.local/share/fonts/DejaVuSans.ttf"
else
    echo "WARN: no DejaVuSans.ttf found; text regions may not assert"
fi

# --- Step 4: launch manager and navigate to the profile editor ---------------
HOME="$FONT_HOME" XDG_CONFIG_HOME="$FONT_HOME/.config" \
XDG_DATA_HOME="$FONT_HOME/.local/share" "$INSTALLED_BIN" --manager 2>"$TMPDIR/mgr.err" &
MANAGER_PID=$!
sleep 2.0

if ! kill -0 "$MANAGER_PID" 2>/dev/null; then
    wait "$MANAGER_PID" 2>/dev/null || true
    fail "manager exited prematurely"
    exit 1
fi

# Controllers tab is the default.  Click the Profiles tab (middle of 3 tabs).
xdotool mousemove 550 24 click 1
sleep 0.6
# Click the first profile row in the list to select it, then the Edit button.
# Profiles panel: list (16,64,1248x420), Edit button (212,500,180x44).
xdotool mousemove 200 90 click 1
sleep 0.3
xdotool mousemove 302 522 click 1
sleep 0.7

EDITOR_CAPTURE="$TMPDIR/editor.png"
import -window root "$EDITOR_CAPTURE" 2>/dev/null
if [ ! -f "$EDITOR_CAPTURE" ]; then
    fail "failed to capture the editor window"
    exit 1
fi

# --- Step 5: semantic diagram assertions (installed production window) --------
# Diagram widget: (16, 88, 300x300).  Panel: (0,48,1280,672).
DIAG="300x300+16+88"
TITLE="300x40+16+56"
LIST="580x420+346+108"

# 5a. Controller outline SVG must be present: black silhouette pixels on the
#     dark panel.  A blank/flat diagram has none.  The generic-gamepad.svg is
#     filled #000 and rasterised via the production path (ABGR8888).
BLACK=$(convert "$EDITOR_CAPTURE" -crop "$DIAG" +repage -format "%c" \
        histogram:info:- 2>/dev/null | awk -F'[(,)]' '$2==0&&$3==0&&$4==0{s+=$1} END{print s+0}')
echo "  diagram outline (black) pixels: $BLACK"
if [ "$BLACK" -lt 5000 ]; then
    fail "controller diagram outline not rendered (black pixels $BLACK < 5000)"
else
    pass "controller outline rendered in diagram region ($BLACK black pixels)"
fi

# 5b. Slot highlight (focus-colored highlight on a mapped button). The
# highlight is theme.focus (100,180,255) blended over the panel, appearing as a
# muted blue. Count pixels that are clearly blue-dominant and not panel/black.
# Use a colour-range fuzz match (an HSL saturation channel mean is not a
# reliable pixel count).
HIGHLIGHT2=$(convert "$EDITOR_CAPTURE" -crop "$DIAG" +repage \
        -fuzz 25% -fill white -opaque "srgb(79,136,192)" -fill black +opaque white \
        -colorspace gray -format "%[fx:mean*w*h]" info: 2>/dev/null)
echo "    diagram highlight pixels: $HIGHLIGHT2"
if [ "${HIGHLIGHT2:-0}" -lt 100 ]; then
    fail "controller slot highlight not rendered in diagram region (blue px $HIGHLIGHT2 < 100)"
else
    pass "slot highlight rendered in diagram region ($HIGHLIGHT2 px)"
fi

# 5c. Model label / title (profile name text above the diagram).
TITLE_PX=$(convert "$EDITOR_CAPTURE" -crop "$TITLE" +repage -colorspace gray \
        -threshold 60% -format "%[fx:mean*w*h]" info: 2>/dev/null)
echo "    title text pixels: $TITLE_PX"
if [ "${TITLE_PX:-0}" -lt 50 ]; then
    fail "editor model/title label not rendered ($TITLE_PX px)"
else
    pass "editor title/model label rendered ($TITLE_PX px)"
fi

# 5d. Binding list (right panel) must carry text rows.
LIST_PX=$(convert "$EDITOR_CAPTURE" -crop "$LIST" +repage -colorspace gray \
        -threshold 60% -format "%[fx:mean*w*h]" info: 2>/dev/null)
echo "    binding list text pixels: $LIST_PX"
if [ "${LIST_PX:-0}" -lt 100 ]; then
    fail "binding list not rendered ($LIST_PX px)"
else
    pass "binding list rendered ($LIST_PX px)"
fi

if [ "$FAILURES" -ne 0 ]; then
    echo "FAIL: installed-window diagram semantic acceptance failed" >&2
    exit 1
fi

echo "PASS: installed-window controller diagram semantic acceptance"
exit 0
