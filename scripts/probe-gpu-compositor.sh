#!/usr/bin/env bash
# probe-gpu-compositor.sh — non-skipping candidate capability probe for
# `gpu-compositor` (Controller-Box; designed for the dedicated unprivileged
# gpurunner class).
#
# Proves, against a real installed product, that a private Weston compositor
# with a hardware/paravirtual GL renderer (VirGL, virtio-gpu, discrete or
# integrated GPU) renders the production manager UI and that the
# profile-editor diagram region shows a recognizable controller silhouette.
#
# Live mode (the committed contract argv) does, in order:
#   1. installed-launch check    only an installed Controller-Box binary with
#                                installed assets is acceptable; source-tree
#                                binaries and source-asset fallbacks fail
#                                (source-tree-binary-rejected /
#                                source-asset-fallback-rejected);
#   2. compositor check          starts a private Weston (headless backend,
#                                GL renderer, Xwayland) on a private runtime
#                                dir and compiles+queries the EGL renderer
#                                probe; software rasterizers fail
#                                (software-renderer-rejected /
#                                renderer-unverified) and weston failures fail
#                                (compositor-start-failed);
#   3. input route               Xwayland + xdotool must drive the real UI
#                                (input-route-failed otherwise);
#   4. navigate + capture        launches the installed manager, opens the
#                                profile editor through the real keyboard and
#                                pointer route, captures the compositor
#                                output (weston-screenshooter);
#   5. semantic analysis         analyze-gpu-compositor.py verifies the
#                                diagram region independently of the asset,
#                                texture, or any golden; failure emits
#                                diagram-not-recognizable (the BUG-0014
#                                negative-control marker).
#
# The screenshot SHA-256, the probe log, and the verdict JSON are preserved
# (under $CBX_GPU_PROBE_ARTIFACTS in live mode, or in the fixture dir) so
# later live negative-control evidence can bind hashes to markers.
#
# Fixture mode:
#   probe-gpu-compositor.sh --fixture DIR
# is adversarial-test-only and never appears in the committed contract argv.
# The fixture dir replays the exact staged facts the live probe validates:
#   renderer            GL_RENDERER string (eg. "llvmpipe ..." or "VirGL ...")
#   binary              installed binary path; "in-source" => source tree
#   assets-ok           yes|no
#   weston-ok           yes|no
#   input-ok            yes|no
#   egl-ok              yes|no (renderer probe availability)
#   geometry.json       {"x":..,"y":..,"w":..,"h":..} window rect in the shot
#   diagram-rect        X,Y,W,H relative to the window (default 16,88,300,300)
#   screenshot.png      compositor-level PNG (fixture- or scenario-specific)
#   screenshot-missing  yes => treat the screenshot as missing
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ANALYZER="$SCRIPT_DIR/gpurunner-probes/analyze-gpu-compositor.py"
EGL_SOURCE="$SCRIPT_DIR/gpurunner-probes/egl_renderer_probe.c"

MARKER="--- gpu-compositor capability contract ---"
PROBE_TAG="gpu-compositor-probe"
WIN_TITLE="Controller-Box"
WIN_W=1280
WIN_H=720
DIAGRAM_RECT="16,88,300,300"
EDIT_CLICK="332,522"
PROFILE_NAME="nes-gamepad"

[[ -f "$ANALYZER" && -f "$EGL_SOURCE" ]] || {
    echo "$PROBE_TAG: analyzer or EGL helper source missing" >&2
    exit 1
}

fail() { # marker reason
    echo "$PROBE_TAG: FAIL marker=$1 ($2)" >&2
    exit 1
}

[[ $(id -u) -ne 0 ]] || fail root-refused "must not run as root"

FIXTURE=""
if [[ $# -eq 2 && "$1" == "--fixture" ]]; then
    FIXTURE=$2
elif [[ $# -ne 0 ]]; then
    fail usage "usage: $0 [--fixture DIR]"
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/cbx-gpu-probe.XXXXXX")
chmod 700 "$tmp"
trap 'rm -rf "$tmp"' EXIT

echo "$MARKER"

sha256sum_file() { # path -> prints "hash  path" ("" when unreadable)
    local path=$1
    if [[ -f "$path" && ! -L "$path" ]]; then
        sha256sum "$path" 2>/dev/null || true
    fi
}

# Runs the renderer sub-analysis and fails with the analyzer's exact marker
# (software-renderer-rejected / renderer-unverified).  Returns only on pass.
check_renderer() { # renderer-string
    local renderer=$1
    local verdict=$tmp/renderer-verdict.json
    local rc marker_analyzed=""
    set +e
    python3 "$ANALYZER" renderer --renderer "$renderer" --out "$verdict" \
        >"$tmp/renderer.out" 2>&1
    rc=$?
    set -e
    if [[ $rc -ne 0 ]]; then
        marker_analyzed="renderer-unverified"
        if [[ -f "$verdict" ]]; then
            marker_analyzed=$(python3 - "$verdict" <<'PY'
import json, sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["marker"])
PY
)
        fi
        fail "$marker_analyzed" "renderer is not a hardware/paravirtual GL stack: $renderer"
    fi
}

# ---------------------------------------------------------------------------
# Fixture mode (adversarial test only)
# ---------------------------------------------------------------------------
if [[ -n "$FIXTURE" ]]; then
    [[ -d "$FIXTURE" && ! -L "$FIXTURE" ]] || fail fixture-invalid "fixture dir missing or unsafe"
    cat_fixture() { # name -> content or ""
        local file=$FIXTURE/$1
        if [[ -f "$file" && ! -L "$file" && -r "$file" ]]; then
            cat "$file"
        fi
    }

    # 1. Installed-launch facts.
    local_bin=$(cat_fixture binary)
    if [[ "$local_bin" == "in-source" ]]; then
        fail source-tree-binary-rejected "fixture: binary resolved inside the source/build tree"
    fi
    [[ -n "$local_bin" ]] || fail installed-launch-rejected "fixture: no installed binary"
    if [[ "$(cat_fixture assets-ok)" != "yes" ]]; then
        fail source-asset-fallback-rejected "fixture: installed assets unavailable"
    fi

    # 2. Compositor + renderer facts.
    if [[ "$(cat_fixture weston-ok)" != "yes" ]]; then
        fail compositor-start-failed "fixture: private Weston failed to start"
    fi
    if [[ "$(cat_fixture egl-ok)" != "yes" ]]; then
        fail renderer-unverified "fixture: EGL renderer probe could not establish a renderer"
    fi
    renderer=$(cat_fixture renderer)
    [[ -n "$renderer" ]] || fail renderer-unverified "fixture: no renderer string"
    check_renderer "$renderer"

    # 3. Input route.
    if [[ "$(cat_fixture input-ok)" != "yes" ]]; then
        fail input-route-failed "fixture: Xwayland/xdotool route unavailable"
    fi

    # 4. Screenshot.
    local_shot=$FIXTURE/screenshot.png
    if [[ "$(cat_fixture screenshot-missing)" == "yes" || ! -f "$local_shot" || -L "$local_shot" ]]; then
        fail screenshot-missing "fixture: no compositor-level screenshot"
    fi

    # 5. Window geometry + diagram analysis.
    local_geom=$FIXTURE/geometry.json
    local_diag=$FIXTURE/diagram-rect
    if [[ -f "$local_geom" && ! -L "$local_geom" ]]; then
        geom=$(python3 - "$local_geom" <<'PY'
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
print(f"{data['x']},{data['y']},{data['w']},{data['h']}")
PY
)
    else
        geom="100,80,1280,720"
    fi
    if [[ -f "$local_diag" && ! -L "$local_diag" ]]; then
        diag=$(cat "$local_diag")
    else
        diag="$DIAGRAM_RECT"
    fi

    verdict=$tmp/fixture-verdict.json
    marker_analyzed=""
    set +e
    python3 "$ANALYZER" diagram --screenshot "$local_shot" --geometry "$geom" \
        --diagram "$diag" --out "$verdict" --expect-controller \
        >"$tmp/diagram.out" 2>&1
    rc=$?
    set -e
    if [[ -f "$verdict" ]]; then
        marker_analyzed=$(python3 - "$verdict" <<'PY'
import json, sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["marker"])
PY
)
    fi
    if [[ $rc -ne 0 ]]; then
        [[ -n "$marker_analyzed" ]] || marker_analyzed="diagram-not-recognizable"
        fail "$marker_analyzed" "fixture: diagram analysis did not recognize a controller silhouette"
    fi
    echo "$PROBE_TAG: diagram analysis marker=$marker_analyzed"

    # Preserve hashes/markers for the fixture (owned by the test).
    sha256sum_file "$local_shot" > "$FIXTURE/screenshot.sha256" 2>/dev/null || true
    cp "$verdict" "$FIXTURE/verdict.json" 2>/dev/null || true
    echo "gpu-compositor-probe: PASS (fixture)"
    exit 0
fi

# ---------------------------------------------------------------------------
# Live mode
# ---------------------------------------------------------------------------
REPO_ROOT=$(git rev-parse --show-toplevel 2>/dev/null || realpath "$SCRIPT_DIR/..")

ARTIFACTS=${CBX_GPU_PROBE_ARTIFACTS:-}
if [[ -n "$ARTIFACTS" ]]; then
    mkdir -p "$ARTIFACTS"
    : > "$ARTIFACTS/probe.log"
fi
log() { # msg
    echo "$PROBE_TAG: $1"
    [[ -z "$ARTIFACTS" ]] || echo "$PROBE_TAG: $1" >> "$ARTIFACTS/probe.log"
}

# --- 1. installed-launch check --------------------------------------------
installed_bin=""
while IFS= read -r candidate; do
    [[ -n "$candidate" ]] || continue
    if [[ -f "$candidate" && -x "$candidate" ]]; then
        installed_bin=$(realpath "$candidate")
        break
    fi
done < <(command -v controller-box 2>/dev/null || true)
[[ -n "$installed_bin" ]] || fail installed-launch-rejected "no installed controller-box binary in PATH"
case "$installed_bin" in
    "$REPO_ROOT"|"$REPO_ROOT"/*)
        fail source-tree-binary-rejected "installed controller-box resolves inside the source/build tree: $installed_bin"
        ;;
esac
prefix=$(dirname "$(dirname "$installed_bin")")
installed_svg=""
for cand in \
    "$prefix/share/controller-box/icons/svg/generic-gamepad.svg" \
    "/usr/share/controller-box/icons/svg/generic-gamepad.svg" \
    "/usr/local/share/controller-box/icons/svg/generic-gamepad.svg"; do
    if [[ -f "$cand" && -r "$cand" ]]; then
        installed_svg=$cand
        break
    fi
done
[[ -n "$installed_svg" ]] || fail source-asset-fallback-rejected "installed controller-box assets (generic-gamepad.svg) not found"
log "installed-launch: binary=$installed_bin assets=$installed_svg"

# --- 2. private Weston compositor with GL/VirGL ----------------------------
for tool in weston weston-screenshooter Xwayland xdotool convert identify cc; do
    command -v "$tool" >/dev/null 2>&1 || fail compositor-start-failed "required tool missing: $tool"
done

egl_helper=$tmp/egl_renderer_probe
set +e
if command -v pkg-config >/dev/null 2>&1 && \
        pkg-config --exists egl glesv2 wayland-client 2>/dev/null; then
    # shellcheck disable=SC2046  # pkg-config output is intentionally split into flags
    cc -O2 -o "$egl_helper" "$EGL_SOURCE" \
        $(pkg-config --cflags --libs egl glesv2 wayland-client) 2>>"$tmp/egl-build.log"
else
    cc -O2 -o "$egl_helper" "$EGL_SOURCE" -lEGL -lGLESv2 -lwayland-client 2>>"$tmp/egl-build.log"
fi
set -e
[[ -x "$egl_helper" ]] || fail renderer-unverified "EGL renderer probe failed to build: $(cat "$tmp/egl-build.log" 2>/dev/null || true)"

export XDG_RUNTIME_DIR="$tmp/runtime"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
export WAYLAND_DISPLAY="cbx-gpu-weston"
export XDG_CONFIG_HOME="$tmp/config"
mkdir -p "$XDG_CONFIG_HOME"

cat > "$XDG_CONFIG_HOME/weston.ini" <<INI
[core]
shell=desktop-shell.so
xwayland=true
[keyboard]
[shell]
background-color=0xff18181c
INI

weston --backend=headless-backend.so --renderer=gl --socket="$WAYLAND_DISPLAY" \
    >"$tmp/weston.log" 2>&1 &
weston_pid=$!

compositor_ready=false
for _ in $(seq 1 60); do
    if [[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]] && kill -0 "$weston_pid" 2>/dev/null; then
        compositor_ready=true
        break
    fi
    sleep 0.25
done
if [[ "$compositor_ready" != "true" ]]; then
    kill "$weston_pid" 2>/dev/null || true
    wait "$weston_pid" 2>/dev/null || true
    fail compositor-start-failed "private Weston did not become ready"
fi
log "compositor: private Weston ready (socket=$WAYLAND_DISPLAY)"

# Xwayland: weston spawns it from the xwayland module; wait for its display.
x_display=""
for _ in $(seq 1 60); do
    for sock in /tmp/.X11-unix/X*; do
        [[ -S "$sock" ]] || continue
        x_display=":${sock##*/X}"
        break
    done
    [[ -n "$x_display" ]] && break
    sleep 0.25
done
[[ -n "$x_display" ]] || fail input-route-failed "Xwayland display did not appear"
export DISPLAY="$x_display"
log "input-route: Xwayland on $DISPLAY"

# EGL renderer: the compositor itself must be on GL/VirGL, not a software
# rasterizer.  The EGL helper queries GL_RENDERER through the private socket.
set +e
egl_out=$("$egl_helper" 2>&1)
egl_rc=$?
set -e
if [[ $egl_rc -ne 0 ]]; then
    fail renderer-unverified "EGL renderer probe failed on the private compositor"
fi
renderer_line=$(printf '%s\n' "$egl_out" | sed -n 's/^GL_RENDERER=//p' | head -n 1)
[[ -n "$renderer_line" ]] || fail renderer-unverified "no GL_RENDERER reported"
check_renderer "$renderer_line"
log "renderer: accepted GL_RENDERER=$renderer_line"

# --- 3. input route --------------------------------------------------------
xdotool getdisplaygeometry >/dev/null 2>&1 \
    || fail input-route-failed "xdotool cannot reach the Xwayland display"

# --- 4. installed launch + navigation + capture -----------------------------
# The manager must find a profile to edit; provision one in a private data
# home (mirrors tests/test_manager_visual.c vis_open_editor).
export XDG_DATA_HOME="$tmp/data"
profile_dir="$XDG_DATA_HOME/inputplumber/profiles"
mkdir -p "$profile_dir"
cat > "$profile_dir/$PROFILE_NAME.yml" <<PROFILE
name: nes-gamepad
schema_version: 1
bindings:
  - name: A
    type: button
    source: "0:16"
  - name: B
    type: button
    source: "0:15"
  - name: X
    type: button
    source: "0:13"
  - name: Y
    type: button
    source: "0:14"
  - name: Start
    type: button
    source: "0:6"
  - name: Select
    type: button
    source: "0:5"
PROFILE

export SDL_VIDEODRIVER=x11
export SDL_AUDIODRIVER=dummy
"$installed_bin" --manager >"$tmp/manager.log" 2>&1 &
manager_pid=$!
trap 'kill "$manager_pid" 2>/dev/null || true; kill "$weston_pid" 2>/dev/null || true; wait "$manager_pid" 2>/dev/null || true; wait "$weston_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

window_id=""
for _ in $(seq 1 80); do
    window_id=$(xdotool search --name "$WIN_TITLE" 2>/dev/null | head -n 1 || true)
    [[ -n "$window_id" ]] && break
    sleep 0.25
done
[[ -n "$window_id" ]] || fail installed-launch-rejected "manager window did not appear (see $tmp/manager.log)"

# The window must have the fixed production geometry (1280x720, non-resizable).
win_geom=$(xdotool getwindowgeometry --shell "$window_id" 2>/dev/null || true)
win_x=$(printf '%s\n' "$win_geom" | sed -n 's/^X=//p')
win_y=$(printf '%s\n' "$win_geom" | sed -n 's/^Y=//p')
win_w=$(printf '%s\n' "$win_geom" | sed -n 's/^WIDTH=//p')
win_h=$(printf '%s\n' "$win_geom" | sed -n 's/^HEIGHT=//p')
[[ "$win_w" == "$WIN_W" && "$win_h" == "$WIN_H" ]] \
    || fail window-geometry-unexpected "manager window is ${win_w}x${win_h}, expected ${WIN_W}x${WIN_H}"

# Navigate: Right opens the Profiles tab; the Edit button opens the editor.
xdotool windowactivate --sync "$window_id" >/dev/null 2>&1 || true
xdotool key Right
sleep 0.5
edit_x=$((win_x + ${EDIT_CLICK%,*}))
edit_y=$((win_y + ${EDIT_CLICK#*,}))
xdotool mousemove --sync "$edit_x" "$edit_y"
xdotool click 1
sleep 1.0

# Capture the compositor output (includes the Xwayland surface).
shot=$tmp/controller-box-compositor.png
weston-screenshooter "$shot" >"$tmp/screenshooter.log" 2>&1 \
    || fail screenshot-missing "weston-screenshooter failed"
[[ -f "$shot" && -s "$shot" ]] || fail screenshot-missing "compositor screenshot is empty"

# --- 5. semantic diagram analysis -------------------------------------------
verdict=$tmp/verdict.json
marker_analyzed=""
set +e
python3 "$ANALYZER" diagram --screenshot "$shot" \
    --geometry "$win_x,$win_y,$win_w,$win_h" --diagram "$DIAGRAM_RECT" \
    --out "$verdict" --expect-controller >"$tmp/diagram.out" 2>&1
analyzer_rc=$?
set -e
if [[ -f "$verdict" ]]; then
    marker_analyzed=$(python3 - "$verdict" <<'PY'
import json, sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["marker"])
PY
)
fi
if [[ $analyzer_rc -ne 0 ]]; then
    [[ -n "$marker_analyzed" ]] || marker_analyzed="diagram-not-recognizable"
    fail "$marker_analyzed" "diagram region does not show a recognizable controller silhouette (BUG-0014 negative control)"
fi
log "diagram analysis marker=$marker_analyzed"

# Preserve screenshot/log hashes and the verdict for later evidence binding.
if [[ -n "$ARTIFACTS" ]]; then
    sha256sum_file "$shot" > "$ARTIFACTS/screenshot.sha256"
    cp "$verdict" "$ARTIFACTS/verdict.json"
    cp "$tmp/manager.log" "$ARTIFACTS/manager.log"
fi
log "artifact: screenshot sha256=$(sha256sum_file "$shot" | awk '{print $1}') marker=$marker_analyzed"
echo "gpu-compositor-probe: PASS"
log "gpu-compositor-probe: PASS"
