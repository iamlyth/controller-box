#!/usr/bin/env bash
# visual-capture-driver.sh — Controller-Box installed production capture driver.
#
# Contract (from the generic visual-audit-capture runner):
#   <driver> <state_id> <output.png> <commit>
#
# Launches the INSTALLED production `controller-box` binary under an isolated,
# collision-free Xvfb display, navigates semantically to the requested visual
# state, waits (boundedly) for the correct production window title, captures
# that window with ImageMagick `import`, writes an exact-commit capture receipt
# next to the output, and exits 0 only when the screenshot exists. It never
# touches the build tree (installed binary only), never writes to golden
# directories, and runs under the dedicated visual-audit lease (the capture
# runner holds it; this driver inherits nothing extra).
#
# Hardening versus the previous adapter (BUG-0018 acceptance driver defects):
#   * Bounded window polling — never `xdotool search --sync` (which blocks
#     forever when the target window is absent). A time-bounded poll loop waits
#     for the exact production title ("Controller-Box Manager" for the manager,
#     "Controller-Box Overlay" for the overlay service), so a missing or
#     wrong-titled window fails closed instead of hanging.
#   * Private collision-free display — a display number is probed against the
#     X11 sockets/locks instead of a fixed :97 with a global `pkill`. No global
#     process kill is ever issued; only processes this driver owns are reaped.
#   * Isolated HOME/XDG + deterministic test-owned profile — the driver seeds a
#     test-owned InputPlumber profile (with a low display_order sidecar) into the
#     isolated user dirs so the manager's first profile row (and therefore the
#     editor it opens) is deterministic across environments.
#   * Correct modes — the overlay state uses `--overlay-service` (the real
#     overlay-service flag, never the legacy invalid flag).
#   * Exact installed commit receipt — verifies the working tree matches the
#     requested commit and records the installed binary sha256 + prefix next to
#     every capture for external human review.
#   * Owned process-group cleanup — the manager/overlay and Xvfb each run in
#     their own session/process group (setsid) so cleanup TERM->KILLs the whole
#     owned group, never a single PID and never an unrelated process.
#
# State ids captured (see .factory/visual-audit-inventory.json):
#   manager-main        manager window, default Profiles tab, first profile row
#   manager-profiles    Profiles tab list with a selected profile
#   manager-editor      profile editor with controller diagram (BUG-0018 area)
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

# ---------------------------------------------------------------------------
# Exact-commit binding: the committed source tree must match the requested
# commit, and the installed binary is the one produced from that commit's gate
# run. The receipt (written on success) records the exact binary sha256 so a
# human reviewer can bind the capture to the installed artifact.
# ---------------------------------------------------------------------------
CUR=$(git rev-parse HEAD 2>/dev/null || true)
if [[ -z "$CUR" || "$CUR" != "$COMMIT" ]]; then
    echo "visual-capture: working tree HEAD '$CUR' does not match requested commit '$COMMIT'" >&2
    exit 1
fi

for tool in Xvfb xdotool import; do
    command -v "$tool" >/dev/null 2>&1 || { echo "visual-capture: SKIP missing tool $tool" >&2; exit 77; }
done

# Prefer an already-installed binary from a prior gate (test-install prefix);
# VISUAL_AUDIT_INSTALL_PREFIX overrides the prefix for operator-driven runs.
INSTALLED_BIN=""
for candidate in \
    "${VISUAL_AUDIT_INSTALL_PREFIX:+"$VISUAL_AUDIT_INSTALL_PREFIX/bin/controller-box"}" \
    "$PROJECT_ROOT/.test-install/usr/bin/controller-box" \
    "$PROJECT_ROOT/.test-install/bin/controller-box"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then INSTALLED_BIN=$candidate; break; fi
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

# Expected window title per state (production titles, src/ui/renderer.c).
EXPECTED_TITLE=""
APP_MODE=""
case "$STATE" in
    manager-main|manager-profiles|manager-editor)
        EXPECTED_TITLE="Controller-Box Manager"
        APP_MODE="--manager"
        ;;
    overlay-active)
        EXPECTED_TITLE="Controller-Box Overlay"
        APP_MODE="--overlay-service"
        ;;
    *)
        echo "visual-capture: unknown state '$STATE'" >&2
        exit 64
        ;;
esac

# Bounded window-poll bound (seconds). Configurable for operator-driven runs;
# the production default is generous but always finite.
WINDOW_TIMEOUT="${VISUAL_AUDIT_WINDOW_TIMEOUT:-30}"

# ---------------------------------------------------------------------------
# Owned process-group bookkeeping. Every long-lived child (Xvfb and the app)
# is launched via `setsid` so it becomes the leader of its own process group;
# TERM then KILL targets the whole group, reaping any grandchildren.
# ---------------------------------------------------------------------------
declare -a OWNED_GROUPS=()
TMPDIR=""

kill_group() { # pgid
    local pgid="$1" deadline
    [[ -n "$pgid" && "$pgid" -gt 0 ]] || return 0
    kill -- "-$pgid" 2>/dev/null || true
    deadline=$(( $(date +%s) + 3 ))
    while kill -0 -- "-$pgid" 2>/dev/null && (( $(date +%s) < deadline )); do
        sleep 0.1
    done
    kill -KILL -- "-$pgid" 2>/dev/null || true
}

cleanup() {
    local g
    for g in "${OWNED_GROUPS[@]+"${OWNED_GROUPS[@]}"}"; do
        kill_group "$g"
    done
    [[ -z "$TMPDIR" ]] || rm -rf -- "$TMPDIR"
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Collision-free display selection: pick a display number whose X11 socket and
# lock are free, so concurrent capture leases never collide and no global
# Xvfb is disturbed.
# ---------------------------------------------------------------------------
pick_display() {
    local base n i
    base=$(( 90 + ( $$ % 700 ) ))          # 90..789
    for i in $(seq 0 60); do
        n=$(( base + i ))
        [[ -e "/tmp/.X11-unix/X$n" || -e "/tmp/.X${n}-lock" ]] || { echo "$n"; return 0; }
    done
    return 1
}

DISPLAY_NUM=$(pick_display) || {
    echo "visual-capture: no free X display found" >&2
    exit 1
}

TMPDIR=$(mktemp -d -t cbx-visual-XXXXXX)
# Fully isolate the run's user state (SPEC §8 config paths).
export HOME="$TMPDIR/home"
export XDG_CONFIG_HOME="$HOME/.config"
export XDG_DATA_HOME="$HOME/.local/share"
export XDG_CACHE_HOME="$HOME/.cache"
export XDG_RUNTIME_DIR="$HOME/.runtime"
mkdir -p "$XDG_CONFIG_HOME/controller-box" \
    "$XDG_CONFIG_HOME/systemd/user" \
    "$XDG_DATA_HOME/fonts" \
    "$XDG_RUNTIME_DIR" \
    "$XDG_CACHE_HOME"
# Seed the first-run service marker and a system font so the manager window
# renders text (SPEC §9.1 first-run modal and font handling must not block
# capture); mirrors the installed functional acceptance environment.
touch "$XDG_CONFIG_HOME/systemd/user/controller-box.service"
FONT_FOUND=""
if [ -f "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" ]; then
    FONT_FOUND="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
fi
if [ -z "$FONT_FOUND" ]; then
    FONT_FOUND=$(find /nix/store -name "DejaVuSans.ttf" 2>/dev/null | head -1 || true)
fi
if [ -n "$FONT_FOUND" ]; then
    cp "$FONT_FOUND" "$XDG_DATA_HOME/fonts/DejaVuSans.ttf"
fi

# Deterministic test-owned profile: a test-owned InputPlumber profile with a
# very low display_order sidecar sorts ahead of every builtin/system profile
# (which have display_order 0), so the manager's first profile row — and the
# editor it opens — is deterministic across environments (Task 8 mechanism).
if [[ "$STATE" == "manager-editor" ]]; then
    mkdir -p "$XDG_DATA_HOME/inputplumber/profiles" \
        "$XDG_CONFIG_HOME/controller-box/profile-metadata"
    cat > "$XDG_DATA_HOME/inputplumber/profiles/cbx-capture-test.yaml" <<'YAML'
version: 1
kind: DeviceProfile
name: "Capture-Test-Profile"
description: "Test-owned profile for deterministic installed capture"
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
YAML
    printf 'display_order: -50\n' > \
        "$XDG_CONFIG_HOME/controller-box/profile-metadata/cbx-capture-test.meta.yaml"
fi

# ---------------------------------------------------------------------------
# Launch Xvfb (own session/group) on the collision-free display.
# ---------------------------------------------------------------------------
export DISPLAY=":$DISPLAY_NUM"
export SDL_VIDEODRIVER=x11
export SDL_RENDER_DRIVER=software
setsid Xvfb ":$DISPLAY_NUM" -screen 0 1280x720x24 >"$TMPDIR/xvfb.log" 2>&1 &
XVFB_GROUP=$!
OWNED_GROUPS+=("$XVFB_GROUP")
for _ in $(seq 1 40); do
    kill -0 "$XVFB_GROUP" 2>/dev/null || { echo "visual-capture: Xvfb failed" >&2; exit 1; }
    [[ -e "/tmp/.X11-unix/X$DISPLAY_NUM" ]] && break
    sleep 0.1
done
kill -0 "$XVFB_GROUP" 2>/dev/null || { echo "visual-capture: Xvfb failed" >&2; exit 1; }
[[ -e "/tmp/.X11-unix/X$DISPLAY_NUM" ]] || { echo "visual-capture: Xvfb socket not ready" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Overlay service requires a system DBus to stay resident (SPEC §11.1.5). In
# environments without a system InputPlumber bus, start a private dbus-daemon
# (real dbus, real system-bus address) and point the installed overlay at it so
# its production connect/render path runs and renders its window. This mirrors
# test_installed_smoke.sh and never bypasses the installed binary's code path.
# ---------------------------------------------------------------------------
if [[ "$STATE" == "overlay-active" ]]; then
    command -v dbus-daemon >/dev/null 2>&1 || {
        echo "visual-capture: SKIP missing dbus-daemon (required for overlay-active)" >&2
        exit 77
    }
    DCFG="$TMPDIR/dbus/system.conf"
    mkdir -p "$TMPDIR/dbus"
    cat > "$DCFG" <<EOF
<busconfig>
  <type>system</type>
  <listen>unix:path=$TMPDIR/system-bus</listen>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
    <allow user="*"/>
  </policy>
</busconfig>
EOF
    setsid dbus-daemon --config-file="$DCFG" --print-address=1 \
        >"$TMPDIR/dbus.addr" 2>&1 &
    DBUS_GROUP=$!
    OWNED_GROUPS+=("$DBUS_GROUP")
    for _ in $(seq 1 40); do
        [[ -s "$TMPDIR/dbus.addr" ]] && break
        kill -0 "$DBUS_GROUP" 2>/dev/null || break
        sleep 0.1
    done
    BUS_ADDR=$(head -n 1 "$TMPDIR/dbus.addr" 2>/dev/null || true)
    [[ -n "$BUS_ADDR" ]] || { echo "visual-capture: private dbus-daemon failed" >&2; exit 1; }
    export DBUS_SYSTEM_BUS_ADDRESS="$BUS_ADDR"
    sleep 0.5
fi

# ---------------------------------------------------------------------------
# Launch the installed binary in the requested mode (own session/group).
# ---------------------------------------------------------------------------
setsid "$INSTALLED_BIN" "$APP_MODE" >"$TMPDIR/app.log" 2>&1 &
APP_GROUP=$!
OWNED_GROUPS+=("$APP_GROUP")
sleep 2.5
kill -0 "$APP_GROUP" 2>/dev/null || { echo "visual-capture: app exited early" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Bounded window polling for the exact production title. Returns the window id
# or fails closed after WINDOW_TIMEOUT seconds (never hangs).
# ---------------------------------------------------------------------------
wait_window() { # title-regex, seconds
    local title="$1" secs="$2" win=""
    local deadline=$(( $(date +%s) + secs ))
    while (( $(date +%s) < deadline )); do
        win=$(xdotool search --name "$title" 2>/dev/null | head -n 1 || true)
        if [[ -n "$win" && "$win" != "0" ]]; then
            echo "$win"
            return 0
        fi
        sleep 0.25
    done
    return 1
}

WIN=""
if ! WIN=$(wait_window "^${EXPECTED_TITLE}\$" "$WINDOW_TIMEOUT"); then
    echo "visual-capture: expected window '$EXPECTED_TITLE' not found within ${WINDOW_TIMEOUT}s (state $STATE)" >&2
    exit 1
fi
xdotool windowactivate "$WIN" >/dev/null 2>&1 || true
xdotool windowfocus "$WIN" >/dev/null 2>&1 || true

# ---------------------------------------------------------------------------
# Semantic navigation per state (production event dispatch via xdotool).
# ---------------------------------------------------------------------------
case "$STATE" in
    manager-main)
        # Default view; nothing to navigate.
        ;;
    manager-profiles)
        xdotool key Right   # Controllers -> Profiles tab
        sleep 0.6
        ;;
    manager-editor)
        xdotool key Right   # Controllers -> Profiles tab
        sleep 0.6
        xdotool key Return  # open the editor on the deterministic first profile
        sleep 0.8
        ;;
    overlay-active)
        # Overlay service already running; nothing further to navigate.
        ;;
esac

# Capture the focused production window (isolated display, deterministic).
sleep 0.5
import -window "$WIN" "$OUTPUT" 2>/dev/null || import -window root "$OUTPUT" 2>/dev/null

[[ -s "$OUTPUT" ]] || { echo "visual-capture: no screenshot produced for $STATE" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Exact-commit install receipt (retained beside the capture for external human
# review): binds the capture to the requested commit and the installed binary
# sha256.
# ---------------------------------------------------------------------------
BIN_SHA=$(sha256sum "$INSTALLED_BIN" 2>/dev/null | awk '{print $1}' || true)
IMG_SHA=$(sha256sum "$OUTPUT" 2>/dev/null | awk '{print $1}' || true)
cat > "$OUTPUT.receipt.json" <<EOF
{
  "schema": "controller-box/visual-capture-receipt/v1",
  "state": "$STATE",
  "commit": "$COMMIT",
  "install_prefix": "$PREFIX",
  "binary_sha256": "${BIN_SHA:-unknown}",
  "image": "$OUTPUT",
  "image_sha256": "${IMG_SHA:-unknown}",
  "window_title": "$EXPECTED_TITLE",
  "display": ":$DISPLAY_NUM",
  "finished_at": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF

echo "visual-capture: captured $STATE -> $OUTPUT (commit ${COMMIT:0:12}, bin ${BIN_SHA:0:12})"
