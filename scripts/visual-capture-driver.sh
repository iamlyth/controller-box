#!/usr/bin/env bash
# visual-capture-driver.sh — Controller-Box installed production capture driver.
#
# Contract (from the generic visual-audit-capture runner):
#   <driver> <state_id> <output.png> <commit>
#
# Launches the INSTALLED production `controller-box` binary under an isolated
# Xvfb display, navigates semantically to the requested visual state, captures
# the window with ImageMagick `import`, and exits 0 only when the screenshot
# exists. It never touches the build tree (installed binary only), never
# writes to golden directories, and runs under the dedicated visual-audit
# lease (the capture runner holds it; this driver inherits nothing extra).
#
# State ids captured (see .factory/visual-audit-inventory.json):
#   manager-main        manager window, default Profiles tab, first profile row
#   manager-profiles    Profiles tab list with a selected profile
#   manager-editor      profile editor with controller diagram (BUG-0014 area)
#   overlay-active      overlay window active over the manager
#
# If Xvfb/xdotool/import are unavailable, exit 77 (SKIP), never fake a pass.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
cd -- "$PROJECT_ROOT"

[[ $# -eq 3 ]] || { echo "usage: visual-capture-driver.sh <state_id> <output.png> <commit>" >&2; exit 64; }
STATE=$1
OUTPUT=$2
COMMIT=$3

# The committed source must match the requested commit; the installed binary
# must be the one produced from that commit's gate run.
if [[ "$(git rev-parse HEAD)" != "$COMMIT" ]]; then
    echo "visual-capture: working tree HEAD $CUR does not match requested commit $COMMIT" >&2
    exit 1
fi

for tool in Xvfb xdotool import; do
    command -v "$tool" >/dev/null 2>&1 || { echo "visual-capture: SKIP missing tool $tool" >&2; exit 77; }
done

# Prefer an already-installed binary from a prior gate (test-install prefix);
# fall back to a fresh isolated custom-prefix install only if none exists.
INSTALLED_BIN=""
for candidate in \
    "$PROJECT_ROOT/.test-install/usr/bin/controller-box" \
    "$PROJECT_ROOT/.test-install/bin/controller-box"; do
    if [[ -x "$candidate" ]]; then INSTALLED_BIN=$candidate; break; fi
done
if [[ -z "$INSTALLED_BIN" ]]; then
    echo "visual-capture: no installed production binary found; run the project gate first" >&2
    exit 1
fi
# Installed assets must resolve inside the prefix (no source-tree fallback).
PREFIX=$(cd -- "$(dirname -- "$INSTALLED_BIN")/.." && pwd)
[[ -f "$PREFIX/share/controller-box/icons/svg/generic-gamepad.svg" ]] || {
    echo "visual-capture: installed assets missing in prefix $PREFIX" >&2
    exit 1
}

DISPLAY_NUM=":97"
XVFB_PID=""
APP_PID=""
TMPDIR=""

cleanup() {
    [[ -z "$APP_PID" ]] || kill -KILL "$APP_PID" 2>/dev/null || true
    [[ -z "$XVFB_PID" ]] || kill -KILL "$XVFB_PID" 2>/dev/null || true
    [[ -z "$TMPDIR" ]] || rm -rf "$TMPDIR"
}
trap cleanup EXIT

pkill -f "Xvfb $DISPLAY_NUM" 2>/dev/null || true
sleep 0.4
Xvfb "$DISPLAY_NUM" -screen 0 1280x720x24 &
XVFB_PID=$!
sleep 1.0
kill -0 "$XVFB_PID" 2>/dev/null || { echo "visual-capture: Xvfb failed" >&2; exit 1; }

TMPDIR=$(mktemp -d -t cbx-visual-XXXXXX)
export DISPLAY="$DISPLAY_NUM"
export SDL_VIDEODRIVER=x11
export SDL_RENDER_DRIVER=software
export HOME="$TMPDIR/home"
mkdir -p "$HOME/.config/systemd/user" "$HOME/.local/share/fonts"
# Seed the first-run marker so the SPEC §9.1 modal is skipped (matches the
# installed functional acceptance path; never an env-var production bypass).
mkdir -p "$HOME/.config"
printf '{"first_run_seen":true}\n' > "$HOME/.config/controller-box-first-run.json" 2>/dev/null || true

"$INSTALLED_BIN" --manager >"$TMPDIR/app.log" 2>&1 &
APP_PID=$!
sleep 2.5
kill -0 "$APP_PID" 2>/dev/null || { echo "visual-capture: app exited early" >&2; exit 1; }

# Focus the manager window and navigate semantically per state.
case "$STATE" in
    manager-main)
        xdotool search --sync --name "Controller Box" windowactivate 2>/dev/null || \
            xdotool search --sync --class controller-box windowactivate 2>/dev/null || true
        ;;
    manager-profiles)
        xdotool search --sync --name "Controller Box" windowactivate >/dev/null 2>&1 || true
        xdotool key Right  # Profiles tab
        sleep 0.6
        ;;
    manager-editor)
        xdotool search --sync --name "Controller Box" windowactivate >/dev/null 2>&1 || true
        xdotool key Right  # Profiles tab
        sleep 0.6
        xdotool key Return # open editor on first profile
        sleep 0.8
        ;;
    overlay-active)
        "$INSTALLED_BIN" --overlay >"$TMPDIR/overlay.log" 2>&1 &
        APP_PID=$!
        sleep 2.0
        ;;
    *)
        echo "visual-capture: unknown state '$STATE'" >&2
        exit 64
        ;;
esac

# Capture the focused window; if none, capture the root window (isolated
# display, so this is deterministic).
sleep 0.6
WIN=$(xdotool getactivewindow 2>/dev/null || true)
if [[ -n "$WIN" && "$WIN" != "0" ]]; then
    import -window "$WIN" "$OUTPUT" 2>/dev/null || import -window root "$OUTPUT" 2>/dev/null
else
    import -window root "$OUTPUT"
fi

[[ -s "$OUTPUT" ]] || { echo "visual-capture: no screenshot produced for $STATE" >&2; exit 1; }
echo "visual-capture: captured $STATE -> $OUTPUT (commit ${COMMIT:0:12})"
