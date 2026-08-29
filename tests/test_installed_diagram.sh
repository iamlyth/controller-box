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

# Resolve the script directory without invoking dirname.  This keeps the
# installed-path acceptance runnable in the minimal verifier environment,
# where coreutils may be unavailable while Bash itself is present.
SCRIPT_PATH=${BASH_SOURCE[0]}
SCRIPT_DIR=${SCRIPT_PATH%/*}
if [ "$SCRIPT_DIR" = "$SCRIPT_PATH" ]; then
    SCRIPT_DIR=.
fi
SCRIPT_DIR=$(cd -- "$SCRIPT_DIR" && pwd)
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
XVFB_DISPLAY=""
XVFB_PID=""
MANAGER_PID=""
MANAGER_WINDOW=""
FAILURES=0
TMPDIR=""

# An installed file can be a regular non-symlink while an ancestor redirects
# the staging tree back into the source checkout. Walk every existing
# component so this acceptance cannot prove a source-tree asset through an
# installed-looking path.
reject_symlink_components() {
    local path="$1"
    local prefix="/"
    local component
    local old_ifs="$IFS"
    local -a components
    IFS=/ read -r -a components <<< "${path#/}"
    IFS="$old_ifs"
    for component in "${components[@]}"; do
        [ -n "$component" ] || continue
        prefix="${prefix}${component}"
        if [ -L "$prefix" ]; then
            fail "installed path contains symlink component: $prefix"
            return 1
        fi
        prefix="${prefix}/"
    done
    return 0
}

pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*" >&2; FAILURES=$((FAILURES + 1)); }

# shellcheck disable=SC2329  # invoked indirectly via `trap cleanup EXIT`
cleanup() {
    if [ -n "$MANAGER_PID" ] && kill -0 "$MANAGER_PID" 2>/dev/null; then
        kill -TERM "$MANAGER_PID" 2>/dev/null || true
        sleep 0.3
        kill -KILL "$MANAGER_PID" 2>/dev/null || true
    fi
    # Reap each owned child after termination.  Besides avoiding zombies in
    # repeated installed-path runs, this ensures a failed semantic assertion
    # cannot leave the manager alive while its temporary HOME is removed.
    if [ -n "$MANAGER_PID" ]; then
        wait "$MANAGER_PID" 2>/dev/null || true
    fi
    if [ -n "$XVFB_PID" ] && kill -0 "$XVFB_PID" 2>/dev/null; then
        kill -TERM "$XVFB_PID" 2>/dev/null || true
        sleep 0.5
        kill -KILL "$XVFB_PID" 2>/dev/null || true
    fi
    if [ -n "$XVFB_PID" ]; then
        wait "$XVFB_PID" 2>/dev/null || true
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
# tree, which the acceptance must reject.  Also reject a redirected staging
# ancestor before inspecting individual files.
if ! reject_symlink_components "$STAGING_DIR"; then
    exit 1
fi
INSTALLED_SVG="$STAGING_DIR/share/controller-box/icons/svg/generic-gamepad.svg"
# Require the asset to be a real installed regular file.  Accepting a symlink
# here could silently reintroduce a source-tree asset and make this test pass
# without exercising the installed layout.
if [ ! -f "$INSTALLED_SVG" ] || [ -L "$INSTALLED_SVG" ]; then
    fail "installed layout missing non-symlink controller SVG: $INSTALLED_SVG"
    exit 1
fi
# The production editor resolves the diagram through the installed icon map;
# require that map to be installed as a real file too, so a source-tree asset
# cannot satisfy this acceptance while the production mapping data is absent.
INSTALLED_ICON_MAP="$STAGING_DIR/share/controller-box/controller-icons.yaml"
if ! reject_symlink_components "$INSTALLED_ICON_MAP"; then
    exit 1
fi
if [ ! -f "$INSTALLED_ICON_MAP" ] || [ -L "$INSTALLED_ICON_MAP" ]; then
    fail "installed layout missing non-symlink controller icon map: $INSTALLED_ICON_MAP"
    exit 1
fi
INSTALLED_BIN="$STAGING_DIR/bin/controller-box"
if [ ! -f "$INSTALLED_BIN" ]; then
    INSTALLED_BIN="$STAGING_DIR/usr/bin/controller-box"
fi
if ! reject_symlink_components "$INSTALLED_BIN"; then
    exit 1
fi
if [ ! -f "$INSTALLED_BIN" ] || [ -L "$INSTALLED_BIN" ] || [ ! -x "$INSTALLED_BIN" ]; then
    fail "installed binary is missing, non-executable, or is not a regular non-symlink file: $INSTALLED_BIN"
    exit 1
fi

# --- Step 2: start Xvfb ------------------------------------------------------
# Pick an unused display instead of killing a process by a global pattern.  A
# fixed display can belong to another test (or another user's session), and a
# global pkill can terminate unrelated work.  Xvfb creates both the socket and
# lock below, so reject either one before starting our owned server.
display_number=90
while [ "$display_number" -le 199 ]; do
    if [ ! -e "/tmp/.X11-unix/X${display_number}" ] &&
       [ ! -e "/tmp/.X${display_number}-lock" ]; then
        XVFB_DISPLAY=":${display_number}"
        break
    fi
    display_number=$((display_number + 1))
done
if [ -z "$XVFB_DISPLAY" ]; then
    fail "no unused X11 display is available"
    exit 1
fi
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
    # Avoid a find/head pipeline here: shell globbing is sufficient for the
    # Nix store fallback and keeps this acceptance test independent of helper
    # subprocess permissions.
    for c in /nix/store/*/share/X11/fonts/DejaVuSans.ttf; do
        if [ -f "$c" ]; then FONT_FOUND="$c"; break; fi
    done
fi
if [ -n "$FONT_FOUND" ]; then
    cp "$FONT_FOUND" "$FONT_HOME/.local/share/fonts/DejaVuSans.ttf"
else
    echo "WARN: no DejaVuSans.ttf found; text regions may not assert"
fi

# --- Step 3b: test-owned profile + sidecar (+ optional staged host) ----------
# (Task 8) The diagram acceptance must not depend on which profile happens to
# sort to the first row.  We create a profile the test itself owns in the
# isolated user profiles dir, and select it by its computed row rather than by
# a hardcoded first-row click.  The sidecar display_order forces the test
# profile into a deterministic position: every other profile (builtin Default,
# and any real host/system InputPlumber profiles in /usr/share/inputplumber)
# has display_order 0, so the test profile (order -50) sorts ahead of all of
# them — and behind any staged competitor at an even lower order.  This is
# deterministic with or without host/system profiles present.
USER_PROFILES_DIR="$FONT_HOME/.local/share/inputplumber/profiles"
mkdir -p "$USER_PROFILES_DIR"
CONFIG_DIR="$FONT_HOME/.config/controller-box"
SIDECAR_DIR="$CONFIG_DIR/profile-metadata"
mkdir -p "$SIDECAR_DIR"

TEST_PROFILE="cbx-diagram-test"
TEST_ORDER=-50
# 4 diagram-button mappings (A/B/X/Y): enough for a recognisable binding list,
# with the first mapping (A) on the diagram so the slot highlight renders.
cat > "$USER_PROFILES_DIR/$TEST_PROFILE.yaml" <<'YAML'
version: 1
kind: DeviceProfile
name: "Diagram-Test-Profile"
description: "Test-owned profile for environment-independent installed diagram acceptance"
mapping:
  - name: "A"
    source_event:
      gamepad:
        button: A
    target_events:
      - gamepad: A
  - name: "B"
    source_event:
      gamepad:
        button: B
    target_events:
      - gamepad: B
  - name: "X"
    source_event:
      gamepad:
        button: X
    target_events:
      - gamepad: X
  - name: "Y"
    source_event:
      gamepad:
        button: Y
    target_events:
      - gamepad: Y
YAML
cat > "$SIDECAR_DIR/$TEST_PROFILE.meta.yaml" <<YAML
display_order: $TEST_ORDER
YAML

# Regression (opt-in via CBX_DIAGRAM_STAGE_HOST=1): stage a profile that sorts
# ahead of the test-owned profile and has zero bindings.  /usr/share/inputplumber
# is unwritable in the sandbox (no root, no sudo), so the competitor is staged
# in the isolated user dir with a lower display_order — this reproduces exactly
# the operator's host-sorts-first failure mechanism (a non-test profile at the
# first row).  If the old first-row-click behaviour selected it, the slot
# highlight (5b) and binding list (5d) assertions would fail, mirroring the
# operator's verify-project exit 8.
STAGED_HOST="${CBX_DIAGRAM_STAGE_HOST:-0}"
if [ "$STAGED_HOST" = "1" ]; then
    HOST_PROFILE="0-host-sorts-first"
    HOST_ORDER=-100
    cat > "$USER_PROFILES_DIR/$HOST_PROFILE.yaml" <<'YAML'
version: 1
kind: DeviceProfile
name: "0-Host-Sorts-First"
description: "Staged host InputPlumber profile that sorts ahead of the test-owned profile"
mapping: []
YAML
    cat > "$SIDECAR_DIR/$HOST_PROFILE.meta.yaml" <<YAML
display_order: $HOST_ORDER
YAML
fi

# Compute the sorted row index of the test-owned profile.  The app sorts
# profiles by display_order (ascending) then display_name.  Every profile other
# than the test's staged ones has display_order 0 (builtin/system profiles
# carry no sidecar here), so the number of profiles with a strictly lower
# display_order is exactly the test profile's row index — environment-
# independent.  Selecting this computed row (and verifying by the resulting
# diagram content, §5b/5d) is the "by name" selection, never a first-row click.
TEST_INDEX=0
for f in "$USER_PROFILES_DIR"/*.yaml; do
    [ -f "$f" ] || continue
    base=$(basename "$f" .yaml)
    order=0
    meta="$SIDECAR_DIR/$base.meta.yaml"
    if [ -f "$meta" ]; then
        # The sidecar has one display_order entry.  Read it directly rather
        # than piping through `head`: this acceptance test must remain usable
        # in the minimal installed verifier environment, where an otherwise
        # available coreutils executable may be denied by the sandbox.
        # Parse the single scalar with Bash so the installed-path acceptance
        # does not depend on a separately executable sed from the host/Nix
        # environment.  Reject malformed values by retaining the neutral
        # order; the test-owned profile still remains selected by its
        # explicitly valid sidecar.
        while IFS= read -r meta_line; do
            if [[ "$meta_line" =~ ^[[:space:]]*display_order:[[:space:]]*(-?[0-9]+)[[:space:]]*$ ]]; then
                order="${BASH_REMATCH[1]}"
                break
            fi
        done < "$meta"
    fi
    if [ "$order" -lt "$TEST_ORDER" ]; then
        TEST_INDEX=$((TEST_INDEX + 1))
    fi
done
echo "  test-owned profile '$TEST_PROFILE' sorted at row $TEST_INDEX"
if [ "$STAGED_HOST" = "1" ] && [ "$TEST_INDEX" -eq 0 ]; then
    fail "staged host profile did not sort ahead of the test profile (regression not reproduced)"
    exit 1
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

# Acquire the exact production manager window before sending coordinates.  A
# root capture can otherwise make an unrelated window look like evidence, and
# an unbounded xdotool --sync search can hang forever when startup fails.  Keep
# this bounded and use the discovered window for the final framebuffer capture.
for ((poll = 0; poll < 50; poll++)); do
    MANAGER_WINDOW=$(xdotool search --name '^Controller-Box Manager$' 2>/dev/null | while IFS= read -r candidate; do
        [ -n "$candidate" ] || continue
        title=$(xdotool getwindowname "$candidate" 2>/dev/null || true)
        if [ "$title" = "Controller-Box Manager" ]; then
            printf '%s\n' "$candidate"
            break
        fi
    done)
    if [ -n "$MANAGER_WINDOW" ]; then
        break
    fi
    sleep 0.1
done
if [ -z "$MANAGER_WINDOW" ]; then
    fail "could not acquire exact Controller-Box Manager window"
    exit 1
fi

# Controllers tab is the default.  Click the Profiles tab (middle of 3 tabs).
xdotool mousemove 550 24 click 1
sleep 0.6
# Select the test-owned profile by its computed row, then the Edit button.
# Profiles panel: list (16,64,1248x420, 32px rows), Edit (212,500,180x44).
# Row i spans y [64+32*i, 96+32*i); use the same 90px relative offset as the
# original first-row click, shifted by the row index.
ROW_Y=$((90 + 32 * TEST_INDEX))
xdotool mousemove 200 "$ROW_Y" click 1
sleep 0.3
xdotool mousemove 302 522 click 1
sleep 0.7

EDITOR_CAPTURE="$TMPDIR/editor.png"
# Re-acquire after navigation: SDL may recreate the production window during
# a mode transition, and using the stale/root handle would weaken the semantic
# assertion.  The bounded search also verifies the exact title.
MANAGER_WINDOW=""
for ((poll = 0; poll < 50; poll++)); do
    MANAGER_WINDOW=$(xdotool search --name '^Controller-Box Manager$' 2>/dev/null | while IFS= read -r candidate; do
        [ -n "$candidate" ] || continue
        title=$(xdotool getwindowname "$candidate" 2>/dev/null || true)
        if [ "$title" = "Controller-Box Manager" ]; then
            printf '%s\n' "$candidate"
            break
        fi
    done)
    if [ -n "$MANAGER_WINDOW" ]; then
        break
    fi
    sleep 0.1
done
if [ -z "$MANAGER_WINDOW" ]; then
    fail "manager window disappeared before editor capture"
    exit 1
fi
import -window "$MANAGER_WINDOW" "$EDITOR_CAPTURE" 2>/dev/null
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
if ! awk -v pixels="${HIGHLIGHT2:-}" 'BEGIN {
        exit !(pixels ~ /^[0-9]+([.][0-9]+)?$/ && (pixels + 0) >= 100)
    }'; then
    fail "controller slot highlight not rendered in diagram region (blue px ${HIGHLIGHT2:-invalid} < 100 or invalid)"
else
    pass "slot highlight rendered in diagram region ($HIGHLIGHT2 px)"
fi

# 5c. Model label / title (profile name text above the diagram). The title
#     shows the loaded profile's name — for a correctly selected test-owned
#     profile this is "Diagram-Test-Profile".  (No OCR in the sandbox, so the
#     assertion is semantic: the title region carries text, and §5b/§5d below
#     prove the loaded profile is the test-owned one, not a first-row host
#     profile with different binding content.)
TITLE_PX=$(convert "$EDITOR_CAPTURE" -crop "$TITLE" +repage -colorspace gray \
        -threshold 60% -format "%[fx:mean*w*h]" info: 2>/dev/null)
echo "    title text pixels: $TITLE_PX"
if ! awk -v pixels="${TITLE_PX:-}" 'BEGIN {
        exit !(pixels ~ /^[0-9]+([.][0-9]+)?$/ && (pixels + 0) >= 50)
    }'; then
    fail "editor model/title label not rendered (${TITLE_PX:-invalid} px)"
else
    pass "editor title/model label rendered ($TITLE_PX px)"
fi

# 5d. Binding list (right panel) must carry text rows.
LIST_PX=$(convert "$EDITOR_CAPTURE" -crop "$LIST" +repage -colorspace gray \
        -threshold 60% -format "%[fx:mean*w*h]" info: 2>/dev/null)
echo "    binding list text pixels: $LIST_PX"
if ! awk -v pixels="${LIST_PX:-}" 'BEGIN {
        exit !(pixels ~ /^[0-9]+([.][0-9]+)?$/ && (pixels + 0) >= 100)
    }'; then
    fail "binding list not rendered (${LIST_PX:-invalid} px)"
else
    pass "binding list rendered ($LIST_PX px)"
fi

if [ "$FAILURES" -ne 0 ]; then
    echo "FAIL: installed-window diagram semantic acceptance failed" >&2
    exit 1
fi

echo "PASS: installed-window controller diagram semantic acceptance"
exit 0
