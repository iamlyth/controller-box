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
#   1. exact-commit build    archives the exact HEAD commit into a temp
#                            source tree, then configures, builds, and
#                            installs the product FRESH under an isolated
#                            prefix outside that source; the ambient PATH is
#                            never trusted (git-archive-failed /
#                            configure-failed / build-failed /
#                            install-failed / installed-launch-rejected /
#                            binary-outside-prefix-rejected /
#                            installed-assets-missing);
#   2. source removal        verifies the installed binary and every required
#                            asset resolve inside the prefix, then deletes
#                            the archived source+build trees before launch so
#                            the compiled-in SOURCE_PROFILE_DIR /
#                            SOURCE_ICON_DIR fallbacks can never satisfy the
#                            runtime asset lookups — the installed prefix
#                            assets must win (source-tree-accessible-rejected /
#                            source-data-path-available);
#   3. isolated launch       runs the installed binary from an isolated CWD
#                            with PATH prefixed to the install bin dir and a
#                            private HOME/XDG tree (never the repository);
#   4. compositor check      starts a private Weston (headless backend,
#                            GL renderer, Xwayland) on a private runtime
#                            dir and compiles+queries the EGL renderer
#                            probe; software rasterizers fail
#                            (software-renderer-rejected /
#                            renderer-unverified) and weston failures fail
#                            (compositor-start-failed);
#   5. input route               Xwayland + xdotool must drive the real UI
#                                (input-route-failed otherwise);
#   6. navigate + capture        opens the profile editor through the real
#                                keyboard and pointer route, captures the
#                                compositor output (weston-screenshooter);
#   7. semantic analysis         analyze-gpu-compositor.py verifies the
#                                diagram region independently of the asset,
#                                texture, or any golden; failure emits
#                                diagram-not-recognizable (the BUG-0014
#                                negative-control marker).
#
# The screenshot SHA-256, the probe log, the verdict JSON, and the
# configure/build/install/Weston/manager logs are retained with their hashes
# under $CBX_GPU_PROBE_ARTIFACTS (safe default under the gitignored
# .factory-state tree) so later live negative-control evidence can bind
# hashes to markers.
#
# Fixture mode:
#   probe-gpu-compositor.sh --fixture DIR
# is adversarial-test-only and never appears in the committed contract argv.
# The fixture dir replays the exact staged facts the live probe validates:
#   renderer            GL_RENDERER string (eg. "llvmpipe ..." or "VirGL ...")
#   binary              installed binary path; "in-source" => source tree
#   assets-ok           yes|no
#   commit              exact 40-hex HEAD the recorded binary was built from
#   built-from-head     yes|no (binary produced by this probe's exact-HEAD
#                       build/install, never an ambient-PATH binary)
#   binary-inside-prefix yes|no (installed binary realpath inside prefix)
#   assets-inside-prefix yes|no (installed assets realpath inside prefix)
#   source-removed      yes|no (archived source+build deleted before launch)
#   launch-cwd-isolated yes|no (launched from an isolated CWD)
#   path-prefixed       yes|no (PATH prefixed with the install bin dir)
#   build-ok            yes|no (exact-HEAD configure+build succeeded)
#   install-ok          yes|no (install to the isolated prefix succeeded)
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

# Caller-controlled artifact retention dir (live mode only). Safe default
# lives under the gitignored .factory-state tree.
ARTIFACTS=${CBX_GPU_PROBE_ARTIFACTS:-}

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
# Live-artifact retention + process cleanup. Runs on EVERY live exit path
# (success and failure) via the EXIT trap so the signer always has the
# configure/build/install/Weston/manager logs, the screenshot, and the
# verdict with their hashes. The retained copies are never deleted.
# ---------------------------------------------------------------------------
retain_live_artifacts() {
    [[ -n "$ARTIFACTS" ]] || return 0
    if ! mkdir -p "$ARTIFACTS"; then
        echo "$PROBE_TAG: cannot create artifact dir $ARTIFACTS" >&2
        return 1
    fi
    local f
    for f in \
        "$tmp/configure.log" "$tmp/build.log" "$tmp/install.log" \
        "$tmp/weston.log" "$tmp/egl-build.log" "$tmp/screenshooter.log" \
        "$tmp/manager.log" "$tmp/diagram.out" "$tmp/screenshot.png" \
        "$tmp/verdict.json"; do
        [[ -f "$f" && ! -L "$f" ]] || continue
        cp -f "$f" "$ARTIFACTS/$(basename "$f")" 2>/dev/null || continue
        echo "$PROBE_TAG: artifact hash $(sha256sum "$ARTIFACTS/$(basename "$f")" | awk '{print $1}') $ARTIFACTS/$(basename "$f")"
    done
    if [[ -f "$tmp/screenshot.png" && ! -L "$tmp/screenshot.png" ]]; then
        sha256sum_file "$tmp/screenshot.png" > "$ARTIFACTS/screenshot.sha256" 2>/dev/null || true
    fi
    echo "$PROBE_TAG: artifacts retained under $ARTIFACTS"
    return 0
}

cleanup_live() {
    local pid
    for pid in "${weston_pid:-}" "${manager_pid:-}"; do
        [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
    done
    for pid in "${weston_pid:-}" "${manager_pid:-}"; do
        [[ -n "$pid" ]] && wait "$pid" 2>/dev/null || true
    done
    retain_live_artifacts || true
    rm -rf "$tmp"
}

trap 'cleanup_live' EXIT

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

    # 1. Installed-launch facts. The binary must be a real installed
    #    controller-box produced by THIS probe's own exact-HEAD build/install
    #    (never an ambient-PATH binary), the recorded commit must be the exact
    #    HEAD the probe is running against, the binary and its assets must
    #    resolve inside the recorded install prefix, the archived source/build
    #    trees must have been removed before launch, the launch CWD must be
    #    isolated, and PATH must be prefixed with the install bin dir.
    local_bin=$(cat_fixture binary)
    if [[ "$local_bin" == "in-source" ]]; then
        fail source-tree-binary-rejected "fixture: binary resolved inside the source/build tree"
    fi
    [[ -n "$local_bin" ]] || fail installed-launch-rejected "fixture: no installed binary"
    if [[ "$(cat_fixture built-from-head)" != "yes" ]]; then
        fail ambient-path-binary-rejected "fixture: binary was taken from the ambient PATH, not built from the exact HEAD commit"
    fi
    local_commit=$(cat_fixture commit)
    if [[ ! "$local_commit" =~ ^[0-9a-f]{40}$ ]]; then
        fail exact-commit-mismatch "fixture: recorded build commit is not a 40-hex HEAD hash"
    fi
    if head_resolved=$(git -C "$SCRIPT_DIR/.." rev-parse HEAD 2>/dev/null) && \
            [[ "$local_commit" != "$head_resolved" ]]; then
        fail exact-commit-mismatch "fixture: recorded commit $local_commit is not the exact HEAD $head_resolved"
    fi
    if [[ "$(cat_fixture binary-inside-prefix)" != "yes" ]]; then
        fail binary-outside-prefix-rejected "fixture: installed binary does not resolve inside its install prefix"
    fi
    if [[ "$(cat_fixture build-ok)" != "yes" ]]; then
        fail build-failed "fixture: fresh exact-HEAD configure/build failed"
    fi
    if [[ "$(cat_fixture install-ok)" != "yes" ]]; then
        fail install-failed "fixture: fresh exact-HEAD install failed"
    fi
    if [[ "$(cat_fixture assets-inside-prefix)" != "yes" ]]; then
        fail assets-outside-prefix-rejected "fixture: installed assets do not resolve inside the prefix"
    fi
    if [[ "$(cat_fixture assets-ok)" != "yes" ]]; then
        fail source-asset-fallback-rejected "fixture: installed assets unavailable"
    fi
    if [[ "$(cat_fixture source-removed)" != "yes" ]]; then
        fail source-tree-accessible-rejected "fixture: archived source/build tree still present at launch"
    fi
    if [[ "$(cat_fixture launch-cwd-isolated)" != "yes" ]]; then
        fail launch-cwd-source-fallback-rejected "fixture: product launched from a source CWD (source fallback possible)"
    fi
    if [[ "$(cat_fixture path-prefixed)" != "yes" ]]; then
        fail path-not-prefixed-rejected "fixture: PATH was not prefixed with the install bin dir"
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
if [[ -z "$ARTIFACTS" ]]; then
    ARTIFACTS="$SCRIPT_DIR/../.factory-state/artifacts/gpu-compositor/$(date +%Y%m%dT%H%M%S)"
fi
if [[ -n "$ARTIFACTS" ]]; then
    mkdir -p "$ARTIFACTS"
    : > "$ARTIFACTS/probe.log"
fi
log() { # msg
    echo "$PROBE_TAG: $1"
    [[ -z "$ARTIFACTS" ]] || echo "$PROBE_TAG: $1" >> "$ARTIFACTS/probe.log"
}

# --- 1. exact-HEAD build + install under an isolated prefix -----------------
# The installed binary is produced HERE from a fresh git archive of the exact
# HEAD commit; the ambient PATH is never trusted.  The compiled-in
# SOURCE_PROFILE_DIR / SOURCE_ICON_DIR fallbacks point into the archived
# source tree, which is deleted before launch so the installed prefix assets
# must win.
head_commit=$(git -C "$SCRIPT_DIR/.." rev-parse HEAD 2>/dev/null || true)
[[ -n "$head_commit" ]] || fail exact-commit-unresolved "cannot resolve HEAD in $SCRIPT_DIR/.."
mkdir -p "$tmp/source"
if ! git -C "$SCRIPT_DIR/.." archive "$head_commit" | tar -x -C "$tmp/source"; then
    fail git-archive-failed "git archive of exact HEAD $head_commit failed"
fi
log "install: archived exact HEAD $head_commit"

# Runtime files the probe needs AFTER the source tree is deleted are staged
# into the isolated tmp dir first (probe/analyzer/EGL helper sources).
mkdir -p "$tmp/runtime"
cp "$ANALYZER" "$tmp/runtime/analyze-gpu-compositor.py"
cp "$EGL_SOURCE" "$tmp/runtime/egl_renderer_probe.c"
ANALYZER="$tmp/runtime/analyze-gpu-compositor.py"
EGL_SOURCE="$tmp/runtime/egl_renderer_probe.c"

prefix="$tmp/prefix"
if ! cmake -S "$tmp/source" -B "$tmp/build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" >"$tmp/configure.log" 2>&1; then
    cat "$tmp/configure.log" >&2
    fail configure-failed "cmake configure of exact HEAD failed"
fi
if ! cmake --build "$tmp/build" --parallel >"$tmp/build.log" 2>&1; then
    cat "$tmp/build.log" >&2
    fail build-failed "cmake build of exact HEAD failed"
fi
if ! cmake --install "$tmp/build" --prefix "$prefix" >"$tmp/install.log" 2>&1; then
    cat "$tmp/install.log" >&2
    fail install-failed "cmake install to isolated prefix failed"
fi
log "install: exact-HEAD build installed under $prefix"

# Verify the installed binary realpath and every required asset resolve
# inside the prefix (no source/build-tree fallback can satisfy them).
installed_bin="$prefix/bin/controller-box"
[[ -x "$installed_bin" ]] || fail installed-launch-rejected "installed binary missing: $installed_bin"
installed_real=$(readlink -f "$installed_bin")
case "$installed_real" in
    "$prefix"/*) ;;
    *) fail binary-outside-prefix-rejected "installed binary realpath escapes the prefix: $installed_real" ;;
esac
installed_svg=""
asset_ok=1
for asset in \
    "$prefix/share/controller-box/icons/svg/generic-gamepad.svg" \
    "$prefix/share/controller-box/profiles/default.yaml" \
    "$prefix/share/controller-box/controller-icons.yaml"; do
    if [[ -f "$asset" && ! -L "$asset" ]]; then
        asset_real=$(readlink -f "$asset")
        case "$asset_real" in
            "$prefix"/*) ;;
            *) asset_ok=0 ;;
        esac
        if [[ -z "$installed_svg" && "$asset" == *generic-gamepad.svg ]]; then
            installed_svg=$asset
        fi
    else
        asset_ok=0
    fi
    [[ "$asset_ok" -eq 1 ]] || break
done
[[ "$asset_ok" -eq 1 ]] || fail installed-assets-missing "required installed assets missing or outside the prefix"
[[ -n "$installed_svg" ]] || fail installed-assets-missing "installed generic-gamepad.svg not found in prefix"
log "installed-launch: binary=$installed_real assets=$installed_svg prefix=$prefix"

# Delete the archived source+build trees BEFORE launch: the compiled-in
# SOURCE_PROFILE_DIR / SOURCE_ICON_DIR fallbacks must be unreachable so the
# installed prefix assets win.  A leftover source/build tree is a hard fail.
rm -rf "$tmp/source" "$tmp/build"
if [[ -e "$tmp/source" || -e "$tmp/build" ]]; then
    fail source-tree-accessible-rejected "archived source/build tree still present at launch"
fi
# The binary's relative data/profiles fallback must not resolve from the
# isolated launch CWD either (no source-relative data path available).
if [[ -e "$tmp/data/profiles/default.yaml" || -e "$tmp/data/icons/svg/generic-gamepad.svg" ]]; then
    fail source-data-path-available "a source-relative data path is reachable from the launch CWD"
fi

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

# Launch the installed, unmodified manager from an isolated CWD (never the
# repository) with PATH prefixed to the install bin dir, so a relative
# source asset lookup or an ambient-PATH binary can never win.
(
    cd "$tmp" || exit 1
    export PATH="$prefix/bin:$PATH"
    export HOME="$tmp/home"
    export SDL_VIDEODRIVER=x11
    export SDL_AUDIODRIVER=dummy
    exec "$installed_bin" --manager
) >"$tmp/manager.log" 2>&1 &
manager_pid=$!
log "launch: cwd=$tmp path-prefix=$prefix/bin binary=$installed_real"

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

# Screenshot/log hashes and the verdict are retained with the probe log by
# retain_live_artifacts on the EXIT trap (every live exit path).
log "artifact: screenshot sha256=$(sha256sum_file "$shot" | awk '{print $1}') marker=$marker_analyzed"
echo "gpu-compositor-probe: PASS"
log "gpu-compositor-probe: PASS"
