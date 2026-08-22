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

# Production tool selection/test hooks must FAIL CLOSED outside the explicit
# RALPH_VISUAL_AUDIT_TESTING marker: a stray CBX_ATOMIC_PUBLISH or
# CBX_PRE_PUBLISH_HOOK in a production environment is an injection, not
# something to silently ignore (or to hide behind a tool SKIP). Only under the
# marker is an override honored. This check sits above the display-tool gate so
# a production run carrying an override always refuses (exit 1) rather than
# skipping (77).
if [[ -n "${CBX_ATOMIC_PUBLISH:-}" || -n "${CBX_PRE_PUBLISH_HOOK:-}" ]] \
        && [[ ${RALPH_VISUAL_AUDIT_TESTING:-0} != 1 ]]; then
    echo "visual-capture: test-only publish override present without RALPH_VISUAL_AUDIT_TESTING; refusing to run" >&2
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
# Current user identity: every owned artifact and the output parent must be
# owned by this uid, and only identity-matched owned entries are ever removed
# on failure/signal (a pre-existing/unowned OUTPUT or a path raced in by
# another principal is never deleted or replaced).
ME=$(id -u 2>/dev/null || echo 0)
# Owned capture/receipt temp paths. Written in the same directory as OUTPUT so
# the final publish is an atomic same-filesystem rename; only these exactly-
# owned paths are ever removed on failure/signal, never a pre-existing OUTPUT.
CAPTURE_TMP=""
RECEIPT_TMP=""
# Atomic-publication state machine: tracks how far a publication has progressed
# so the EXIT trap can withdraw exactly the owned artifacts between the two
# publishes and before the final directory fsync (point: no half-published or
# orphaned image/receipt ever survives an error or a terminating signal).
RECEIPT_PUBLISHED=0
IMAGE_PUBLISHED=0
COMMITTED=0

# Remove a path ONLY if it is a current-user-owned regular single-link file.
# Defense-in-depth identity match: a temp/artifact that was raced away or
# replaced by a symlink/hardlink, or that is owned by another principal, is
# never removed. A pre-existing/unowned OUTPUT is therefore never deleted.
rm_owned() { # path
    local p=$1 owner links
    [[ -n "$p" ]] || return 0
    [[ -e "$p" || -L "$p" ]] || return 0
    owner=$(stat -c %u "$p" 2>/dev/null) || return 0
    [[ "$owner" == "$ME" ]] || return 0
    # Regular (incl. empty) file that is not a symlink and has a single link.
    [[ -L "$p" ]] && return 0
    [[ -f "$p" ]] || return 0
    links=$(stat -c %h "$p" 2>/dev/null)
    [[ "$links" == "1" ]] || return 0
    rm -f -- "$p"
}

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
    # Withdraw only identity-matched owned entries. A pre-existing/unowned
    # OUTPUT (or a path raced in by another principal) is never deleted.
    #   * temps not yet published: the capture and receipt temps;
    #   * a published-but-not-committed receipt (between the receipt publish
    #     and the image commit point / final directory fsync);
    #   * a published image and its receipt once the image is at OUTPUT but
    #     before the final directory fsync (the image commit point was reached
    #     but durability was not confirmed).
    rm_owned "$CAPTURE_TMP"
    rm_owned "$RECEIPT_TMP"
    if [[ "$RECEIPT_PUBLISHED" == 1 && "$COMMITTED" == 0 ]]; then
        rm_owned "$OUTPUT.receipt.json"
    fi
    if [[ "$IMAGE_PUBLISHED" == 1 && "$COMMITTED" == 0 ]]; then
        rm_owned "$OUTPUT"
    fi
}
# Run the same owned-temp/process-group cleanup on terminating signals as well
# as normal exit, so an interrupt never strands a partial capture or receipt.
trap cleanup EXIT
for _sig in INT TERM HUP QUIT; do
    trap 'cleanup; exit 130' "$_sig"
done

# ---------------------------------------------------------------------------
# Atomic publish + durable-fsync helper. Publication uses an atomic no-replace
# primitive (renameat2 RENAME_NOREPLACE, falling back to link()+unlink()) so a
# destination created after any existence check is still refused atomically: no
# TOCTOU and no ordinary overwriting `mv`. fsync failures FAIL CLOSED (nonzero),
# so the crash/power-durability claim is real rather than best-effort: a
# half-published or non-durable receipt/image is never presented as a success.
# ---------------------------------------------------------------------------
ATOMIC="$SCRIPT_DIR/atomic-publish.py"
# A test may inject a mock helper via CBX_ATOMIC_PUBLISH, honored ONLY under the
# RALPH_VISUAL_AUDIT_TESTING marker (the completion gate rejects that marker, so
# it can never weaken the production publish path). The fail-closed check for a
# stray override lives above the display-tool gate.
if [[ ${RALPH_VISUAL_AUDIT_TESTING:-0} == 1 && -n "${CBX_ATOMIC_PUBLISH:-}" ]]; then
    ATOMIC="$CBX_ATOMIC_PUBLISH"
fi
# The publish helper must be a REGULAR file AND executable (a world-writable,
# directory, or non-executable substitute is refused). `test -x` alone would
# accept a directory or a helper that is executable but not a regular file.
if [[ ! -f "$ATOMIC" || ! -x "$ATOMIC" ]]; then
    echo "visual-capture: publish helper '$ATOMIC' is not a regular executable file" >&2
    exit 1
fi

publish_no_replace() { # src dst  -> 0 ok, 2 exists, 3 error
    "$ATOMIC" publish "$1" "$2"
}
fsync_path() { # path  -> 0 ok, 3 error (fail-closed durability)
    "$ATOMIC" fsync "$1"
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
# ---------------------------------------------------------------------------
# Owned, canonical output parent + single-link owned temps.
#
# The output parent and every ancestor must be a NON-symlink path component
# (a symlinked parent lets an attacker redirect the atomic rename to an
# attacker-chosen directory), the immediate parent must be owned by the
# current user and must not be writable by group/other (a group/world-writable
# output dir lets another principal swap an owned temp or the final artifact),
# and every temp is created there and must remain a current-user-owned regular
# file with a single link. Together these close the "owned temp" identity gap
# the atomic no-replace rename depends on.
# ---------------------------------------------------------------------------
# Reject the output parent if any path component is a symlink. Also handles a
# relative OUTPUT (dirname bottoming out at "." or "..") by terminating the
# walk there instead of spinning forever.
reject_symlinked_path() { # dir
    local d=$1 prev=""
    while [[ -n "$d" && "$d" != "/" && "$d" != "." && "$d" != ".." ]]; do
        if [[ -L "$d" ]]; then
            echo "visual-capture: output parent/ancestor is a symlink: $d" >&2
            return 1
        fi
        prev="$d"
        d=$(dirname -- "$d")
        [[ "$d" == "$prev" ]] && break
    done
    return 0
}

# Validate the output parent is a non-symlink, current-user-owned directory
# that is not writable by group/other; echoes the canonical path on success.
verify_output_parent() { # dir
    local dir=$1 perms owner type
    [[ -d "$dir" ]] || { echo "visual-capture: output directory '$dir' does not exist" >&2; return 1; }
    reject_symlinked_path "$dir" || return 1
    owner=$(stat -c %u "$dir" 2>/dev/null) || { echo "visual-capture: cannot stat output directory '$dir'" >&2; return 1; }
    [[ "$owner" == "$ME" ]] || { echo "visual-capture: output directory '$dir' is not owned by current user" >&2; return 1; }
    type=$(stat -c %F "$dir" 2>/dev/null)
    [[ "$type" == "directory" ]] || { echo "visual-capture: output parent '$dir' is not a directory" >&2; return 1; }
    perms=$(stat -c %a "$dir" 2>/dev/null)
    if (( (8#$perms & 8#022) != 0 )); then
        echo "visual-capture: output directory '$dir' is writable by group/other (perms $perms)" >&2
        return 1
    fi
    echo "$dir"
}

# Assert a temp is still a current-user-owned regular single-link file. `-f` is
# true for both empty and non-empty regular files (the capture temp is allocated
# empty by mktemp and only later filled by `import`); the `-L` guard rejects a
# symlink (which `-f` would otherwise follow).
assert_safe_temp() { # path label
    local p=$1 label=$2 owner links
    owner=$(stat -c %u "$p" 2>/dev/null) || { echo "visual-capture: cannot stat $label temp $p" >&2; return 1; }
    [[ "$owner" == "$ME" ]] || { echo "visual-capture: $label temp $p is not owned by current user" >&2; return 1; }
    [[ -L "$p" ]] && { echo "visual-capture: $label temp $p is a symlink" >&2; return 1; }
    [[ -f "$p" ]] || { echo "visual-capture: $label temp $p is not a regular file" >&2; return 1; }
    links=$(stat -c %h "$p" 2>/dev/null)
    [[ "$links" == "1" ]] || { echo "visual-capture: $label temp $p has link count $links (not a single-link owned temp)" >&2; return 1; }
    return 0
}

# The output parent is validated here; temps are allocated in this same owned
# directory so the final publish is an atomic same-filesystem rename. dirfd-
# relative operations are approximated by allocating every temp in the
# validated OUTPUT_DIR and by never accepting a path from outside it; full
# dirfd-relative syscalls are impractical in a shell driver, so the identity
# checks (owned, regular, single-link, non-symlink parents) carry the security
# guarantee instead.
OUTPUT_DIR=$(verify_output_parent "$(dirname -- "$OUTPUT")") || exit 1
CAPTURE_TMP=$(mktemp "$OUTPUT_DIR/.cbx-capture.XXXXXX" 2>/dev/null) \
    || { echo "visual-capture: cannot allocate capture temp in $OUTPUT_DIR" >&2; exit 1; }
assert_safe_temp "$CAPTURE_TMP" capture || exit 1

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
# Durability before hashing: the capture temp bytes are fsynced FIRST so the
# image is on stable storage before it is referenced by a committed receipt,
# then the stable regular inode is hashed (CAPTURE_TMP is the same inode that
# is atomically renamed to OUTPUT, so its sha256 is the committed image hash).
# Both hashes are REQUIRED to be valid 64-hex (the receipt serializer enforces
# it); a failed hash is a fail-closed error, never a silent "unknown".
if ! fsync_path "$CAPTURE_TMP"; then
    echo "visual-capture: fsync failed on capture temp; capture withheld" >&2
    exit 1
fi
BIN_SHA=$(sha256sum "$INSTALLED_BIN" 2>/dev/null | awk '{print $1}') || true
IMG_SHA=$(sha256sum "$CAPTURE_TMP" 2>/dev/null | awk '{print $1}') || true
RECEIPT_TMP=$(mktemp "$OUTPUT_DIR/.cbx-receipt.XXXXXX" 2>/dev/null) \
    || { echo "visual-capture: cannot allocate receipt temp in $OUTPUT_DIR" >&2; exit 1; }
assert_safe_temp "$RECEIPT_TMP" receipt || exit 1
# Build the receipt through the atomic-publish JSON serializer (proper escaping
# of any quotes/backslashes/tabs/newlines in paths/titles; strict 64-hex hash
# validation). A malformed hash or missing field fails closed here.
if ! "$ATOMIC" receipt "$RECEIPT_TMP" \
        "schema=controller-box/visual-capture-receipt/v1" \
        "state=$STATE" \
        "commit=$COMMIT" \
        "install_prefix=$PREFIX" \
        "binary_sha256=${BIN_SHA:-}" \
        "image=$OUTPUT" \
        "image_sha256=${IMG_SHA:-}" \
        "window_title=$EXPECTED_TITLE" \
        "display=:$DISPLAY_NUM" \
        "finished_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"; then
    echo "visual-capture: failed to build receipt; capture withheld" >&2
    exit 1
fi
# Test-only pre-publish injection (gated behind RALPH_VISUAL_AUDIT_TESTING,
# rejected by the completion gate): a regression may source a hook that creates
# OUTPUT/receipt in the exact TOCTOU window between validation and the atomic
# no-replace publish, deterministically proving the primitive (not an existence
# pre-check) refuses a concurrently-created/unowned destination and preserves
# its sentinel with no image-without-receipt.
if [[ ${RALPH_VISUAL_AUDIT_TESTING:-0} == 1 && -n "${CBX_PRE_PUBLISH_HOOK:-}" ]]; then
    # shellcheck disable=SC1090  # path supplied only by a test-only hook
    . "$CBX_PRE_PUBLISH_HOOK"
fi

# Durable fail-closed publication. Two guarantees make an image visible at
# OUTPUT only with its matching validated receipt:
#
#  1. Atomic no-replace publish (renameat2 RENAME_NOREPLACE / link+unlink): the
#     no-overwrite guarantee is enforced in the SAME syscall as the publish, so
#     a destination that appears between any existence check and the publish
#     (TOCTOU) is still refused. No pre-existing/unowned/symlinked/hardlinked
#     path is ever overwritten, and no ordinary `mv` is used.
#  2. Commit-point ordering + fail-closed durability: the receipt is fsynced
#     and published FIRST, then its directory entry is fsynced; only then is
#     the image renamed into place (the commit point) and the directory fsynced.
#     Every fsync failure FAILS CLOSED: the driver withdraws its owned artifacts
#     and exits non-zero, so a half-published or non-durable artifact is never
#     presented as a successful capture.
#
# The RECEIPT_PUBLISHED/IMAGE_PUBLISHED/COMMITTED barriers (set after each
# publish and after the final directory fsync) drive the EXIT-trap cleanup so
# only identity-matched owned artifacts between the two publishes and before
# the final fsync are ever withdrawn.
#
# Flush the receipt bytes to stable storage BEFORE publishing them.
if ! fsync_path "$RECEIPT_TMP"; then
    echo "visual-capture: fsync failed on receipt temp; capture withheld" >&2
    exit 1
fi

# Publish the receipt first (atomic no-replace; refuses a pre-existing/unowned/
# symlinked/hardlinked receipt).
rc=0
publish_no_replace "$RECEIPT_TMP" "$OUTPUT.receipt.json" || rc=$?
if [[ $rc -eq 2 ]]; then
    echo "visual-capture: refusing to overwrite pre-existing receipt '$OUTPUT.receipt.json'" >&2
    exit 1
elif [[ $rc -ne 0 ]]; then
    echo "visual-capture: failed to publish receipt for $STATE; capture withheld" >&2
    exit 1
fi
RECEIPT_TMP=""
# Barrier (signal point) after the receipt publish. There is an inherent few-
# statement window between the syscall returning 0 and this assignment: a
# terminating signal landing exactly there could withdraw the counterpart and
# orphan the just-published receipt. The window is sub-millisecond and the
# EXIT-trap identity checks make any such orphan removable by a human, but it
# is acknowledged here rather than silently ignored. 13f(5) exercises the
# pre-publish signal path; the post-publish window is bounded and accepted.
RECEIPT_PUBLISHED=1
# Make the published receipt entry durable before the image commit point. On a
# failure the EXIT trap withdraws the owned receipt (identity-matched) so a
# failed/doubtful publish leaves neither OUTPUT nor an orphaned, possibly
# non-durable receipt.
if ! fsync_path "$OUTPUT_DIR"; then
    echo "visual-capture: fsync failed on receipt directory entry; capture withheld" >&2
    exit 1
fi

# Commit point: only now is the image visible at OUTPUT (atomic no-replace).
rc=0
publish_no_replace "$CAPTURE_TMP" "$OUTPUT" || rc=$?
if [[ $rc -eq 2 ]]; then
    # A pre-existing/unowned OUTPUT appeared (the TOCTOU window is closed by the
    # no-replace primitive). The EXIT trap withdraws the owned receipt (identity-
    # matched) so no image is ever published without its receipt and the
    # pre-existing OUTPUT/sentinel is preserved.
    echo "visual-capture: refusing to overwrite pre-existing OUTPUT '$OUTPUT'" >&2
    exit 1
elif [[ $rc -ne 0 ]]; then
    echo "visual-capture: failed to publish capture for $STATE; no artifact recorded" >&2
    exit 1
fi
CAPTURE_TMP=""
# Barrier (signal point) after the image commit-point publish (see the note on
# the receipt barrier for the acknowledged sub-millisecond signal window).
IMAGE_PUBLISHED=1
# Make the image commit-point entry durable. If this fsync fails the image is
# already atomically at OUTPUT; the EXIT trap withdraws the owned image and its
# receipt (identity-matched) to keep a clean fail-closed state.
if ! fsync_path "$OUTPUT_DIR"; then
    echo "visual-capture: fsync failed on capture directory entry; capture withdrawn" >&2
    exit 1
fi
COMMITTED=1

echo "visual-capture: captured $STATE -> $OUTPUT (commit ${COMMIT:0:12}, bin ${BIN_SHA:0:12})"
