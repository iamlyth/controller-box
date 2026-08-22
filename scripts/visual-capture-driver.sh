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

# ---------------------------------------------------------------------------
# Semantic state validation (ImageMagick). The installed manager is driven to a
# requested visual state via deterministic coordinate clicks; before a capture
# is accepted the driver verifies the *content* of the freshly captured frame
# actually matches the requested state, so a wrong-state capture (e.g. the
# profile list masquerading as the editor) fails closed instead of being
# recorded as evidence. Regions and thresholds come from the fixed 1280x720
# manager layout (SPEC §5.1): the controller-diagram region (x16 y40 300x300,
# profile_editor_list.c CBX_PE_*) and the Profiles-tab Create/Edit/Delete
# button row (x16 y500 400x44, profiles_tab.c CBX_PT_LIST_*/CBX_PT_BTN_*).
# ---------------------------------------------------------------------------

# Print the ImageMagick standard-deviation (0..1) of a crop; empty on error.
img_std() { # png x y w h
    convert "$1" -crop "$4x$5+$2+$3" +repage \
        -format '%[fx:standard_deviation]' info: 2>/dev/null || echo ""
}

# Print the mean RGB triple ("r g b", each 0..1) of a crop; empty on error.
img_mean() { # png x y w h
    convert "$1" -crop "$4x$5+$2+$3" +repage \
        -format '%[fx:mean.r] %[fx:mean.g] %[fx:mean.b]' info: 2>/dev/null || echo ""
}

# Is the first manager-profiles list row visibly selected? The profiles list
# (panel y=48 + CBX_PT_LIST_Y=16 => list y=64..484, item_h=32, SPEC §5.3)
# auto-selects row 0 on refresh (cbx_profiles_tab_refresh), which the list
# widget paints with panel_bg_hover {42,42,58} against the unselected
# panel_bg {30,30,42}. Compare background-only right-edge strips of row 0 and
# row 1: a genuine selection leaves a measurable per-channel mean difference
# (>= 0.03 ~ 8/255), while an unselected list has identical row backgrounds.
row_selected() { # png
    local img="$1" r0 r1
    r0=$(img_mean "$img" 1150 70 100 16)
    r1=$(img_mean "$img" 1150 102 100 16)
    [[ -n "$r0" && -n "$r1" ]] || { echo "cannot measure profile list rows" >&2; return 1; }
    awk -v a="$r0" -v b="$r1" 'BEGIN{
        split(a, A, " "); split(b, B, " ");
        dr=A[1]-B[1]; dg=A[2]-B[2]; db=A[3]-B[3];
        if (dr<0) dr=-dr; if (dg<0) dg=-dg; if (db<0) db=-db;
        exit (dr>=0.03 || dg>=0.03 || db>=0.03) ? 0 : 1;
    }'
}

# Validate that a captured frame semantically matches the requested state.
# Returns 0 on match, 1 (after an explanatory message) on a wrong state.
validate_state() { # state png
    local state="$1" img="$2" diag btn
    diag=$(img_std "$img" 16 40 300 300)
    btn=$(img_std "$img" 16 500 400 44)
    [[ -n "$diag" && -n "$btn" ]] || {
        echo "visual-capture: cannot measure $state frame content ($img)" >&2
        return 1
    }
    case "$state" in
    manager-editor)
        # Editor: the controller diagram must be present (high variance) and
        # the profile-list edit-button row must be hidden.
        if awk "BEGIN{exit !($diag >= 0.13)}"; then :; else
            echo "visual-capture: state validation FAILED ($state): controller diagram absent (diagram std=$diag, expect >=0.13)" >&2
            return 1
        fi
        if awk "BEGIN{exit !($btn <= 0.09)}"; then :; else
            echo "visual-capture: state validation FAILED ($state): profile-list buttons still visible (btn-row std=$btn, expect <=0.09)" >&2
            return 1
        fi
        ;;
    manager-profiles)
        # Profiles tab: the Create/Edit/Delete button row must be visible.
        if awk "BEGIN{exit !($btn >= 0.13)}"; then :; else
            echo "visual-capture: state validation FAILED ($state): Create/Edit/Delete button row absent (btn std=$btn, expect >=0.13)" >&2
            return 1
        fi
        # And a profile row must be visibly selected (the inventory requires a
        # selected row, not merely the button row). A profile list with no
        # highlighted row is a wrong-state/false-positive capture.
        if row_selected "$img"; then :; else
            echo "visual-capture: state validation FAILED ($state): no selected profile row visible (unselected list captured)" >&2
            return 1
        fi
        ;;
    overlay-active)
        # Overlay active state requires real icon/status content rendered by
        # the installed overlay. A uniform/blank frame means the overlay never
        # reached an active state (it is created hidden and only shown when an
        # active gamepad stimulus activates it via the real system service).
        # Failing closed here honestly blocks the state when that required
        # service stimulus is unavailable, instead of recording a false-positive
        # black frame as evidence.
        full=$(img_std "$img" 0 0 1280 720)
        [[ -n "$full" ]] || { echo "visual-capture: cannot measure $state frame content ($img)" >&2; return 1; }
        if awk "BEGIN{exit !($full >= 0.02)}"; then :; else
            echo "visual-capture: state validation FAILED ($state): overlay frame is blank/uniform (std=$full); the overlay never rendered active icon/status content (requires an active gamepad stimulus via the real system service)" >&2
            return 1
        fi
        ;;
    manager-main)
        # Default Controllers view: neither the profile-list button row nor the
        # editor diagram may be present (guards against a wrong-state capture).
        if awk "BEGIN{exit !($btn <= 0.09)}"; then :; else
            echo "visual-capture: state validation FAILED ($state): profile-list button row visible (btn std=$btn, expect <=0.09)" >&2
            return 1
        fi
        if awk "BEGIN{exit !($diag <= 0.09)}"; then :; else
            echo "visual-capture: state validation FAILED ($state): editor diagram visible (diag std=$diag, expect <=0.09)" >&2
            return 1
        fi
        ;;
    esac
    return 0
}

# Test-only validation hook (gated behind RALPH_VISUAL_AUDIT_TESTING): feed a
# previously captured PNG and validate it against the requested state without
# re-navigating. Lets the installed-adapter regression prove the validator
# rejects a wrong-state frame (the BUG-0018 wrong-state capture) directly. The
# production completion gate rejects this marker, so it never weakens the
# production capture path.
if [[ ${RALPH_VISUAL_AUDIT_TESTING:-0} == 1 && -n "${CBX_VISUAL_VALIDATE_ONLY:-}" ]]; then
    if validate_state "$STATE" "$CBX_VISUAL_VALIDATE_ONLY"; then
        echo "visual-capture: [test] state '$STATE' validated against $CBX_VISUAL_VALIDATE_ONLY"
        exit 0
    fi
    echo "visual-capture: [test] state '$STATE' rejected for $CBX_VISUAL_VALIDATE_ONLY" >&2
    exit 1
fi

# Full production capture needs a real X11/display toolchain. This gate sits
# AFTER the test-only validation hook so the fail-closed validator (which needs
# only ImageMagick `convert`) can be exercised by non-skipping negative
# regressions without a display server or installed binary.
for tool in Xvfb xdotool import convert; do
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
# Owned capture/receipt temp paths. Written in the same directory as OUTPUT so
# the final publish is an atomic same-filesystem rename; only these exactly-
# owned paths are ever removed on failure/signal, never a pre-existing OUTPUT.
CAPTURE_TMP=""
RECEIPT_TMP=""

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
    # Remove only owned capture/receipt temps. A pre-existing/unowned OUTPUT is
    # never deleted; a rejected or interrupted capture leaves no partial bytes
    # at the requested output path and no orphaned receipt sidecar.
    [[ -z "$CAPTURE_TMP" ]] || rm -f -- "$CAPTURE_TMP"
    [[ -z "$RECEIPT_TMP" ]] || rm -f -- "$RECEIPT_TMP"
}
# Run the same owned-temp/process-group cleanup on terminating signals as well
# as normal exit, so an interrupt never strands a partial capture or receipt.
trap cleanup EXIT
for _sig in INT TERM HUP QUIT; do
    trap 'cleanup; exit 130' "$_sig"
done

# Best-effort durable fsync of a single file (and, for the directory variant,
# the directory that holds the renames). Used to make the atomic publish
# crash/power-look durable: the receipt and the final OUTPUT rename are flushed
# to stable storage before/after they land. A failure to fsync is not fatal to
# the driver's correctness contract (which is about atomic visibility), so it
# is best-effort and never aborts the operation.
fsync_path() { # path
    python3 -c 'import os,sys; fd=os.open(sys.argv[1], os.O_RDONLY); os.fsync(fd); os.close(fd)' \
        "$1" 2>/dev/null || true
}

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
# Semantic navigation per state (deterministic coordinate clicks through the
# production X11 event path). The manager window fills the isolated 1280x720
# screen, so absolute coordinates in the window map to fixed layout positions:
#   * Profiles tab: x=640, y=24   (tab bar 0..48, 3 tabs; Profiles is middle)
#   * Edit Profile: x=302, y=522 (profiles_tab layout, CBX_PT_BTN_*)
# Keyboard navigation (Right/Return) was abandoned because the profile list is
# not activated from the tab-bar focus, so Return never opens the editor and
# the capture silently recorded the wrong (profile-list) state (BUG-0018).
# ---------------------------------------------------------------------------
case "$STATE" in
    manager-main)
        # Default view is the Controllers tab (SPEC §5.1 initial tab); nothing
        # to navigate.
        ;;
    manager-profiles)
        xdotool mousemove 640 24 click 1   # Profiles tab
        sleep 0.8
        # Deterministically select the first profile row so a selected/highlighted
        # row is genuinely rendered (cbx_profiles_tab_refresh auto-selects row 0,
        # and the click keeps focus on the list so the highlight is visible).
        xdotool mousemove 640 80 click 1   # first profile row
        sleep 0.8
        ;;
    manager-editor)
        xdotool mousemove 640 24 click 1   # Profiles tab
        sleep 0.8
        xdotool mousemove 640 80 click 1   # select first profile row
        sleep 0.8
        xdotool mousemove 302 522 click 1  # Edit Profile (deterministic first row)
        sleep 1.0
        ;;
    overlay-active)
        # Overlay service already running; nothing further to navigate.
        ;;
esac

# Reacquire the exact production window after navigation (the window id may
# have changed or focus shifted); never capture a stale/replaced handle.
if ! WIN=$(wait_window "^${EXPECTED_TITLE}$" "$WINDOW_TIMEOUT"); then
    echo "visual-capture: window '$EXPECTED_TITLE' not re-found after navigation (state $STATE)" >&2
    exit 1
fi
xdotool windowactivate "$WIN" >/dev/null 2>&1 || true
xdotool windowfocus "$WIN" >/dev/null 2>&1 || true

# ---------------------------------------------------------------------------
# Atomic capture publication. The frame is captured to an exclusively-owned
# temporary file in the SAME directory as OUTPUT (so the final publish is an
# atomic same-filesystem rename), semantically validated there, and renamed to
# OUTPUT only after validation succeeds. On any failure or signal the owned
# temp (and receipt temp) are removed and a pre-existing/unowned OUTPUT is
# never deleted or replaced by unvalidated bytes (BUG-0018 fail-closed driver
# contract: a rejected capture must not leave a partial artifact at the
# requested output path).
# ---------------------------------------------------------------------------
OUTPUT_DIR=$(dirname -- "$OUTPUT")
if [[ ! -d "$OUTPUT_DIR" ]]; then
    echo "visual-capture: output directory '$OUTPUT_DIR' does not exist" >&2
    exit 1
fi
CAPTURE_TMP=$(mktemp "$OUTPUT_DIR/.cbx-capture.XXXXXX" 2>/dev/null) \
    || { echo "visual-capture: cannot allocate capture temp in $OUTPUT_DIR" >&2; exit 1; }

# Capture the focused production window (isolated display, deterministic). The
# `png:` prefix forces ImageMagick output format regardless of the temp name.
sleep 0.5
import -window "$WIN" "png:$CAPTURE_TMP" 2>/dev/null \
    || import -window root "png:$CAPTURE_TMP" 2>/dev/null

[[ -s "$CAPTURE_TMP" ]] || { echo "visual-capture: no screenshot produced for $STATE" >&2; exit 1; }

# Semantic validation: the freshly captured frame must actually match the
# requested state. Fail closed (never record a wrong-state capture as evidence)
# unless the expected content is present.
if ! validate_state "$STATE" "$CAPTURE_TMP"; then
    echo "visual-capture: wrong-state capture for $STATE; refusing to record evidence" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Exact-commit install receipt (retained beside the capture for external human
# review): binds the capture to the requested commit and the installed binary
# sha256. Also built on an owned temp and renamed last, so a partial receipt
# never accompanies a failed/absent capture.
# ---------------------------------------------------------------------------
BIN_SHA=$(sha256sum "$INSTALLED_BIN" 2>/dev/null | awk '{print $1}' || true)
IMG_SHA=$(sha256sum "$CAPTURE_TMP" 2>/dev/null | awk '{print $1}' || true)
RECEIPT_TMP=$(mktemp "$OUTPUT_DIR/.cbx-receipt.XXXXXX" 2>/dev/null) \
    || { echo "visual-capture: cannot allocate receipt temp in $OUTPUT_DIR" >&2; exit 1; }
cat > "$RECEIPT_TMP" <<EOF
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
# Flush the receipt bytes to stable storage before the atomic renames so the
# published sidecar is durable, not just visible.
fsync_path "$RECEIPT_TMP"

# Fail-closed publication. Two guarantees make an image visible at OUTPUT only
# with its matching validated receipt:
#
#  1. Refuse unowned destinations: OUTPUT and its receipt sidecar must not
#     already exist (including via a symlink, a dangling symlink, or a hardlink
#     to an unowned file). A plain `mv -f` would silently replace (or follow
#     and clobber) a path we do not own; refusing is the only fail-safe.
#  2. Commit-point ordering: the receipt is renamed into place FIRST, and the
#     image rename is the LAST (commit-point) rename. So whenever OUTPUT exists
#     after the driver, its validated receipt is already in place beside it. If
#     the receipt publish fails we abort before touching OUTPUT; if the image
#     publish fails we remove the just-published (owned) receipt so a failed
#     publish leaves neither OUTPUT nor an orphaned receipt.
# Both renames are same-filesystem and atomic; each is followed by a directory
# fsync so a crash/power loss cannot leave a half-published directory entry.
if [[ -e "$OUTPUT" || -L "$OUTPUT" ]]; then
    echo "visual-capture: refusing to overwrite pre-existing OUTPUT '$OUTPUT'" >&2
    exit 1
fi
if [[ -e "$OUTPUT.receipt.json" || -L "$OUTPUT.receipt.json" ]]; then
    echo "visual-capture: refusing to overwrite pre-existing receipt '$OUTPUT.receipt.json'" >&2
    exit 1
fi

if ! mv -- "$RECEIPT_TMP" "$OUTPUT.receipt.json" 2>/dev/null; then
    echo "visual-capture: failed to publish receipt for $STATE; capture withheld" >&2
    exit 1
fi
RECEIPT_TMP=""
fsync_path "$OUTPUT_DIR"

# Commit point: only now is the image visible at OUTPUT.
if ! mv -- "$CAPTURE_TMP" "$OUTPUT" 2>/dev/null; then
    # We own the receipt we just published; remove it so a failed publish
    # leaves neither OUTPUT nor a stray receipt sidecar.
    rm -f -- "$OUTPUT.receipt.json"
    echo "visual-capture: failed to publish capture for $STATE; no artifact recorded" >&2
    exit 1
fi
CAPTURE_TMP=""
fsync_path "$OUTPUT_DIR"

echo "visual-capture: captured $STATE -> $OUTPUT (commit ${COMMIT:0:12}, bin ${BIN_SHA:0:12})"
