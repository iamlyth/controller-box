#!/usr/bin/env bash
# Adversarial gpu-compositor candidate probe checks: the probe must fail
# closed against software renderers, source-tree binaries, missing installed
# assets, a broken compositor, an unusable input route, a missing screenshot,
# and blank/dark/noise diagram regions — and it must accept exactly two kinds
# of evidence: a real VirGL-style renderer AND a diagram region whose pixels
# independently prove a recognizable controller silhouette.  Fixtures are
# generated deterministically (fixed seed) and replay the exact staged facts
# the live probe validates; a fixture that could produce a false pass is
# itself a failure.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

ANALYZER="$PROJECT_ROOT/scripts/gpurunner-probes/analyze-gpu-compositor.py"
PROBE="$PROJECT_ROOT/scripts/probe-gpu-compositor.sh"
SVG="$PROJECT_ROOT/data/icons/svg/generic-gamepad.svg"

command -v nix-shell >/dev/null || {
    echo "test: nix-shell required for the gpu-compositor probe fixtures" >&2
    exit 1
}

must_fail() { # label marker-fragment cmd...
    local label=$1 marker=$2
    shift 2
    set +e
    "$@" >"$tmp/out.log" 2>&1
    local rc=$?
    set -e
    [[ $rc -ne 0 ]] || { echo "test: $label unexpectedly passed" >&2; exit 1; }
    if [[ -n "$marker" ]]; then
        grep -qF "$marker" "$tmp/out.log" || {
            echo "test: $label did not emit marker $marker" >&2
            cat "$tmp/out.log" >&2
            exit 1
        }
    fi
}

must_pass() { # label cmd...
    local label=$1
    shift
    "$@" >"$tmp/out.log" 2>&1 || {
        echo "test: $label unexpectedly failed" >&2
        cat "$tmp/out.log" >&2
        exit 1
    }
}

run_probe() { # fixture_dir
    nix-shell --run "bash '$PROBE' --fixture '$1'"
}

# ---------------------------------------------------------------------------
# Full compositor screenshot (1920x1080) with the manager window at
# (100,80,1280,720) and the 300x300 diagram crop at window+(16,88) i.e.
# (116,168) in output coordinates.
# ---------------------------------------------------------------------------
compose_shot() { # out.png diagram.png|"blank"|"dark"|"noise"
    local out=$1 diagram=$2
    convert -size 1920x1080 xc:"#18181c" "$out"
    convert "$out" -fill "#12121c" -draw "rectangle 100,80 1379,799" "$out"
    case "$diagram" in
        blank)
            convert "$out" -fill "#1e1e2a" -draw "rectangle 116,168 415,467" "$out"
            ;;
        dark)
            convert "$out" -fill "#000000" -draw "rectangle 116,168 415,467" "$out"
            ;;
        noise)
            convert "$out" -fill "#1e1e2a" -draw "rectangle 116,168 415,467" "$out"
            convert "$out" "$tmp/noise.png" -geometry +116+168 -composite "$out"
            ;;
        *)
            convert "$out" -fill "#1e1e2a" -draw "rectangle 116,168 415,467" "$out"
            convert "$out" "$diagram" -geometry +116+168 -composite "$out"
            ;;
    esac
}

# Render the real production asset exactly as a compositor would rasterize it.
nix-shell --run "convert -background '#1e1e2a' -size 300x300 '$SVG' '$tmp/diagram-svg.png'"

# Synthetic recognizable controller silhouette: scanline fill of the real
# gamepad outline polygon (data/icons/svg/generic-gamepad.svg), independent
# of ImageMagick's SVG renderer.
python3 - "$tmp/gamepad.ppm" <<'PY'
import sys
pts = [(32,8),(12,16),(6,40),(15,49),(22,45),(24,42),(29,40),(35,40),(40,42),(42,45),(49,49),(58,40),(52,16)]
S = 300/64.0
poly = [(int(round(x*S)), int(round(y*S))) for x, y in pts]
W = H = 300
img = [[0]*W for _ in range(H)]
for y in range(H):
    xs = []
    n = len(poly)
    for i in range(n):
        x1, y1 = poly[i]
        x2, y2 = poly[(i+1) % n]
        if (y1 <= y < y2) or (y2 <= y < y1):
            xs.append(x1 + (y-y1)*(x2-x1)/(y2-y1))
    xs.sort()
    for k in range(0, len(xs)-1, 2):
        a, b = int(xs[k]), int(xs[k+1])
        for x in range(max(0, a), min(W, b)+1):
            img[y][x] = 1
with open(sys.argv[1], "wb") as f:
    f.write(b"P6\n%d %d\n255\n" % (W, H))
    for y in range(H):
        for x in range(W):
            f.write(bytes((0, 0, 0)) if img[y][x] else bytes((30, 30, 42)))
PY

# Broad random pixels in the diagram region (deterministic seed).
python3 - "$tmp/noise.png" <<'PY'
import random, sys
random.seed(7)
w = h = 300
with open(sys.argv[1], "wb") as f:
    f.write(b"P6\n%d %d\n255\n" % (w, h))
    for _ in range(w*h):
        v = random.randint(0, 255)
        f.write(bytes((v, v, v)))
PY
nix-shell --run "convert '$tmp/noise.png' '$tmp/noise.png'"

# ---------------------------------------------------------------------------
# Analyzer unit checks (independent of the probe wrapper).
# ---------------------------------------------------------------------------
must_pass "virgl renderer accepted" python3 "$ANALYZER" renderer --renderer "VirGL 0.7.0 (virtio-gpu)"
must_pass "discrete GPU renderer accepted" python3 "$ANALYZER" renderer --renderer "NVIDIA GeForce RTX 3060 (GL 4.6)"
must_fail "llvmpipe rejected" "software-renderer-rejected" \
    python3 "$ANALYZER" renderer --renderer "llvmpipe (LLVM 15.0.7, 128 bits)"
must_fail "softpipe rejected" "software-renderer-rejected" \
    python3 "$ANALYZER" renderer --renderer "Mesa softpipe"
must_fail "swrast rejected" "software-renderer-rejected" \
    python3 "$ANALYZER" renderer --renderer "Mesa X11 swrast"
must_fail "unknown renderer is unverified (fail closed)" "renderer-unverified" \
    python3 "$ANALYZER" renderer --renderer "Mystery Renderer 9000"
must_fail "empty renderer is unverified" "renderer-unverified" \
    python3 "$ANALYZER" renderer --renderer ""

# ---------------------------------------------------------------------------
# Full-pass fixtures (two independent renderings of a recognizable
# controller silhouette).
# ---------------------------------------------------------------------------
make_base_fixture() { # dir
    local dir=$1
    mkdir -p "$dir"
    printf 'VirGL 0.7.0 (virtio-gpu)\n' > "$dir/renderer"
    printf '/opt/controller-box/bin/controller-box\n' > "$dir/binary"
    printf 'yes\n' > "$dir/assets-ok"
    printf 'yes\n' > "$dir/weston-ok"
    printf 'yes\n' > "$dir/input-ok"
    printf 'yes\n' > "$dir/egl-ok"
    printf '{"x":100,"y":80,"w":1280,"h":720}\n' > "$dir/geometry.json"
    # Exact-commit/install/source-removed facts the live probe now asserts:
    # the recorded binary was built from the exact HEAD, installed into an
    # isolated prefix, and the archived source+build were deleted before the
    # isolated-CWD launch with PATH prefixed to the install bin dir.
    printf '%s\n' "$(git -C "$PROJECT_ROOT" rev-parse HEAD)" > "$dir/commit"
    printf 'yes\n' > "$dir/built-from-head"
    printf 'yes\n' > "$dir/binary-inside-prefix"
    printf 'yes\n' > "$dir/assets-inside-prefix"
    printf 'yes\n' > "$dir/source-removed"
    printf 'yes\n' > "$dir/launch-cwd-isolated"
    printf 'yes\n' > "$dir/path-prefixed"
    printf 'yes\n' > "$dir/build-ok"
    printf 'yes\n' > "$dir/install-ok"
}

# Pass fixture 1: the production SVG rendered through ImageMagick.
make_base_fixture "$tmp/pass"
compose_shot "$tmp/pass/screenshot.png" "$tmp/diagram-svg.png"
must_pass "production-svg diagram pass" run_probe "$tmp/pass"
grep -q "gpu-compositor-probe: PASS" "$tmp/out.log"
grep -qF "controller-recognized" "$tmp/pass/verdict.json"
[[ -s "$tmp/pass/screenshot.sha256" ]] || { echo "test: screenshot.sha256 missing" >&2; exit 1; }
grep -q "diagram-not-recognizable" "$tmp/pass/verdict.json" && {
    echo "test: pass fixture verdict is negative" >&2
    exit 1
}
python3 - "$tmp/pass/screenshot.png" "$tmp/pass/screenshot.sha256" <<'PY'
import hashlib, sys
actual = hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest()
recorded = open(sys.argv[2], encoding="utf-8").read().split()[0]
assert actual == recorded, (actual, recorded)
PY

# Pass fixture 2: synthetic polygon silhouette (independent of the SVG asset).
make_base_fixture "$tmp/synthetic-pass"
compose_shot "$tmp/synthetic-pass/screenshot.png" "$tmp/gamepad.ppm"
must_pass "synthetic controller silhouette pass" run_probe "$tmp/synthetic-pass"
grep -qF "controller-recognized" "$tmp/synthetic-pass/verdict.json"

# ---------------------------------------------------------------------------
# Adversarial fixtures: every negative path must fail closed with its exact
# marker (deterministic, no false pass).
# ---------------------------------------------------------------------------
make_base_fixture "$tmp/software-renderer"
printf 'llvmpipe (LLVM 15.0.7, 128 bits)\n' > "$tmp/software-renderer/renderer"
compose_shot "$tmp/software-renderer/screenshot.png" "$tmp/diagram-svg.png"
must_fail "software renderer rejection" "software-renderer-rejected" \
    run_probe "$tmp/software-renderer"

make_base_fixture "$tmp/unknown-renderer"
printf 'Mystery Renderer 9000\n' > "$tmp/unknown-renderer/renderer"
compose_shot "$tmp/unknown-renderer/screenshot.png" "$tmp/diagram-svg.png"
must_fail "unknown renderer fails closed" "renderer-unverified" \
    run_probe "$tmp/unknown-renderer"

make_base_fixture "$tmp/source-tree-binary"
printf 'in-source\n' > "$tmp/source-tree-binary/binary"
compose_shot "$tmp/source-tree-binary/screenshot.png" "$tmp/diagram-svg.png"
must_fail "source/build-tree binary rejection" "source-tree-binary-rejected" \
    run_probe "$tmp/source-tree-binary"

make_base_fixture "$tmp/source-assets"
printf 'no\n' > "$tmp/source-assets/assets-ok"
compose_shot "$tmp/source-assets/screenshot.png" "$tmp/diagram-svg.png"
must_fail "source-asset fallback rejection" "source-asset-fallback-rejected" \
    run_probe "$tmp/source-assets"

make_base_fixture "$tmp/no-binary"
rm -f "$tmp/no-binary/binary"
compose_shot "$tmp/no-binary/screenshot.png" "$tmp/diagram-svg.png"
must_fail "missing installed binary" "installed-launch-rejected" \
    run_probe "$tmp/no-binary"

# ---------------------------------------------------------------------------
# Exact-commit / install / source-removed fact rejection (the live probe now
# builds+installs the exact HEAD itself; these facts must all hold or the
# probe fails closed BEFORE any renderer/compositor evidence is read).
# ---------------------------------------------------------------------------
# Ambient PATH binary: the recorded binary was NOT produced by this probe's
# own exact-HEAD build/install.
make_base_fixture "$tmp/ambient-path"
printf 'no\n' > "$tmp/ambient-path/built-from-head"
compose_shot "$tmp/ambient-path/screenshot.png" "$tmp/diagram-svg.png"
must_fail "ambient PATH binary rejected" "ambient-path-binary-rejected" \
    run_probe "$tmp/ambient-path"

# Recorded build commit is not the exact HEAD the probe is running against.
make_base_fixture "$tmp/wrong-commit"
printf '%040d\n' 0 > "$tmp/wrong-commit/commit"
compose_shot "$tmp/wrong-commit/screenshot.png" "$tmp/diagram-svg.png"
must_fail "recorded commit not exact HEAD rejected" "exact-commit-mismatch" \
    run_probe "$tmp/wrong-commit"

# The archived source/build tree was still present at launch: the compiled-in
# SOURCE_PROFILE_DIR / SOURCE_ICON_DIR fallbacks would still be reachable.
make_base_fixture "$tmp/source-accessible"
printf 'no\n' > "$tmp/source-accessible/source-removed"
compose_shot "$tmp/source-accessible/screenshot.png" "$tmp/diagram-svg.png"
must_fail "source tree still accessible rejected" "source-tree-accessible-rejected" \
    run_probe "$tmp/source-accessible"

# Installed binary does not resolve inside its install prefix (wrong prefix).
make_base_fixture "$tmp/wrong-prefix"
printf 'no\n' > "$tmp/wrong-prefix/binary-inside-prefix"
compose_shot "$tmp/wrong-prefix/screenshot.png" "$tmp/diagram-svg.png"
must_fail "binary outside its prefix rejected" "binary-outside-prefix-rejected" \
    run_probe "$tmp/wrong-prefix"

# Installed assets do not resolve inside the prefix (missing/wrong assets).
make_base_fixture "$tmp/assets-outside"
printf 'no\n' > "$tmp/assets-outside/assets-inside-prefix"
compose_shot "$tmp/assets-outside/screenshot.png" "$tmp/diagram-svg.png"
must_fail "assets outside prefix rejected" "assets-outside-prefix-rejected" \
    run_probe "$tmp/assets-outside"

# Fresh exact-HEAD configure/build failure.
make_base_fixture "$tmp/build-failed"
printf 'no\n' > "$tmp/build-failed/build-ok"
compose_shot "$tmp/build-failed/screenshot.png" "$tmp/diagram-svg.png"
must_fail "exact-HEAD build failure rejected" "build-failed" \
    run_probe "$tmp/build-failed"

# Fresh exact-HEAD install failure.
make_base_fixture "$tmp/install-failed"
printf 'no\n' > "$tmp/install-failed/install-ok"
compose_shot "$tmp/install-failed/screenshot.png" "$tmp/diagram-svg.png"
must_fail "exact-HEAD install failure rejected" "install-failed" \
    run_probe "$tmp/install-failed"

# Launch CWD not isolated: relative source data lookups could resolve.
make_base_fixture "$tmp/cwd-not-isolated"
printf 'no\n' > "$tmp/cwd-not-isolated/launch-cwd-isolated"
compose_shot "$tmp/cwd-not-isolated/screenshot.png" "$tmp/diagram-svg.png"
must_fail "source CWD launch rejected" "launch-cwd-source-fallback-rejected" \
    run_probe "$tmp/cwd-not-isolated"

# PATH not prefixed with the install bin dir.
make_base_fixture "$tmp/path-not-prefixed"
printf 'no\n' > "$tmp/path-not-prefixed/path-prefixed"
compose_shot "$tmp/path-not-prefixed/screenshot.png" "$tmp/diagram-svg.png"
must_fail "PATH not prefixed rejected" "path-not-prefixed-rejected" \
    run_probe "$tmp/path-not-prefixed"

make_base_fixture "$tmp/weston-down"
printf 'no\n' > "$tmp/weston-down/weston-ok"
compose_shot "$tmp/weston-down/screenshot.png" "$tmp/diagram-svg.png"
must_fail "compositor start failure" "compositor-start-failed" \
    run_probe "$tmp/weston-down"

make_base_fixture "$tmp/egl-down"
printf 'no\n' > "$tmp/egl-down/egl-ok"
compose_shot "$tmp/egl-down/screenshot.png" "$tmp/diagram-svg.png"
must_fail "EGL renderer unestablished" "renderer-unverified" \
    run_probe "$tmp/egl-down"

make_base_fixture "$tmp/input-down"
printf 'no\n' > "$tmp/input-down/input-ok"
compose_shot "$tmp/input-down/screenshot.png" "$tmp/diagram-svg.png"
must_fail "input route failure" "input-route-failed" \
    run_probe "$tmp/input-down"

make_base_fixture "$tmp/missing-shot"
printf 'yes\n' > "$tmp/missing-shot/screenshot-missing"
must_fail "missing compositor screenshot" "screenshot-missing" \
    run_probe "$tmp/missing-shot"

# BUG-0014 negative control: a blank diagram region (the current production
# defect) must produce exactly the diagram-not-recognizable marker.
make_base_fixture "$tmp/blank"
compose_shot "$tmp/blank/screenshot.png" blank
must_fail "blank diagram (BUG-0014 negative control)" "diagram-not-recognizable" \
    run_probe "$tmp/blank"

make_base_fixture "$tmp/dark"
compose_shot "$tmp/dark/screenshot.png" dark
must_fail "dark diagram rejection" "diagram-not-recognizable" \
    run_probe "$tmp/dark"

make_base_fixture "$tmp/noise"
compose_shot "$tmp/noise/screenshot.png" noise
must_fail "broad random pixels rejection" "diagram-not-recognizable" \
    run_probe "$tmp/noise"

# A thin diagonal stroke is not a controller silhouette: passes edge-style
# density but fails the shape/region assertions.
make_base_fixture "$tmp/line"
convert -size 300x300 xc:"#1e1e2a" "$tmp/line.png"
convert "$tmp/line.png" -stroke "#000000" -strokewidth 4 \
    -draw "line 20,280 280,20" "$tmp/line.png"
compose_shot "$tmp/line/screenshot.png" "$tmp/line.png"
must_fail "thin stroke is not a controller silhouette" "diagram-not-recognizable" \
    run_probe "$tmp/line"

# A lone centered blob (no side grips) is not a controller silhouette.
make_base_fixture "$tmp/blob"
convert -size 300x300 xc:"#1e1e2a" "$tmp/blob.png"
convert "$tmp/blob.png" -fill "#000000" -draw "ellipse 150,133 120,80 0,360" "$tmp/blob.png"
compose_shot "$tmp/blob/screenshot.png" "$tmp/blob.png"
must_fail "lone blob without grips rejection" "diagram-not-recognizable" \
    run_probe "$tmp/blob"

# Diagram crop outside the output is geometry-unexpected.
make_base_fixture "$tmp/geometry-bad"
printf '{"x":1900,"y":80,"w":1280,"h":720}\n' > "$tmp/geometry-bad/geometry.json"
compose_shot "$tmp/geometry-bad/screenshot.png" "$tmp/diagram-svg.png"
must_fail "window geometry outside output" "window-geometry-unexpected" \
    run_probe "$tmp/geometry-bad"

# The probe wrapper refuses unknown arguments and an unsafe fixture dir.
must_fail "probe refuses unknown arguments" "usage" bash "$PROBE" --bogus
must_fail "probe refuses a missing fixture dir" "fixture-invalid" \
    bash "$PROBE" --fixture "$tmp/does-not-exist"

echo "test: gpu-compositor probe adversarial checks passed"
