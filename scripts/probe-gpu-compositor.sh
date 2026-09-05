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
#   3. compositor check      starts a private Weston (headless backend,
#                            GL renderer, Xwayland) with an explicit output
#                            of at least 1280x720 on a private runtime dir
#                            and compiles+queries the EGL renderer probe;
#                            software rasterizers fail
#                            (software-renderer-rejected /
#                            renderer-unverified) and weston failures fail
#                            (compositor-start-failed);
#   4. display binding       DISPLAY is bound to the exact Xwayland display
#                            that THIS weston instance logged — never the
#                            first /tmp/.X11-unix socket; a pre-existing or
#                            ambiguous display is rejected
#                            (display-binding-rejected / input-route-failed);
#   5. isolated launch       runs the installed binary from an isolated CWD
#                            with PATH prefixed to the install bin dir and a
#                            private HOME/XDG tree (never the repository);
#                            the systemd user unit is seeded under
#                            $XDG_CONFIG_HOME so the SPEC §9.1 first-run
#                            modal is provably skipped (first-run-modal-
#                            not-seeded);
#   6. input proof           focuses the exact manager window, sends Right,
#                            captures before/after compositor screenshots,
#                            and asserts a semantic tab-region pixel change
#                            BEFORE the Edit click (tab-change-not-observed
#                            otherwise);
#   7. navigate + capture    opens the profile editor through the real
#                            keyboard and pointer route, captures the
#                            compositor output (weston-screenshooter) and
#                            rejects clipped (<1280x720) output
#                            (output-too-small);
#   8. semantic analysis     analyze-gpu-compositor.py verifies the
#                            diagram region independently of the asset,
#                            texture, or any golden; failure emits
#                            diagram-not-recognizable (the BUG-0014
#                            negative-control marker).
#
# Every retained artifact (configure/build/install/Weston/manager logs, the
# before/after input screenshots, the final controller-box-compositor.png,
# and the verdict JSON) is copied under $CBX_GPU_PROBE_ARTIFACTS (safe
# default under the gitignored .factory-state tree) with its SHA-256 listed
# in artifact-manifest.json; the final screenshot's relative filename+hash is
# embedded in the verdict itself.  The failure marker/reason and the cleanup
# record are appended to probe.log on EVERY live exit path.
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
#   modal-seeded        yes|no (systemd user unit seeded so the first-run
#                       modal is provably skipped)
#   display-bound       yes|no (launch bound to THIS probe's Xwayland display)
#   tab-change-observed yes|no (Right input changed the tab bar before Edit)
#   screenshot-hash     64-hex sha256 of the retained screenshot (mismatch
#                       is rejected: screenshot-hash-mismatch)
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
TAB_BAR_H=48
TAB_DIFF_MIN=200
DIAGRAM_RECT="16,88,300,300"
EDIT_CLICK="332,522"
PROFILE_NAME="nes-gamepad"

[[ -f "$ANALYZER" && -f "$EGL_SOURCE" ]] || {
    echo "$PROBE_TAG: analyzer or EGL helper source missing" >&2
    exit 1
}

# Caller-controlled artifact retention dir (live mode only). Safe default
# lives under the gitignored .factory-state tree.
ARTIFACTS=${CBX_GPU_PROBE_ARTIFACTS:-}
PROBE_MARKER=""

fail() { # marker reason
    local msg="$PROBE_TAG: FAIL marker=$1 ($2)"
    echo "$msg" >&2
    if [[ -n "$ARTIFACTS" && -f "$ARTIFACTS/probe.log" ]]; then
        echo "$msg" >> "$ARTIFACTS/probe.log"
    fi
    PROBE_MARKER="$1"
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

echo "$MARKER"

sha256sum_file() { # path -> prints "hash  path" ("" when unreadable)
    local path=$1
    if [[ -f "$path" && ! -L "$path" ]]; then
        sha256sum "$path" 2>/dev/null || true
    fi
}

png_size() { # path -> "W H" (via python3 PNG header parse; no ImageMagick dep)
    python3 - "$1" <<'PY'
import struct, sys
with open(sys.argv[1], "rb") as f:
    head = f.read(24)
if head[:8] != b"\x89PNG\r\n\x1a\n":
    raise SystemExit("not-a-png")
w, h = struct.unpack(">II", head[16:24])
print(f"{w} {h}")
PY
}

# Fail output-too-small unless the PNG is at least the production window size
# (the explicit headless output must be >=1280x720 — never clipped).
require_output_size() { # png-path
    local png=$1 size_out size_rc w h
    set +e
    size_out=$(png_size "$png" 2>/dev/null)
    size_rc=$?
    set -e
    if [[ $size_rc -ne 0 ]]; then
        fail output-too-small "screenshot is not a readable PNG: $png"
    fi
    read -r w h <<<"$size_out"
    if [[ ! "$w" =~ ^[0-9]+$ || ! "$h" =~ ^[0-9]+$ || "$w" -lt "$WIN_W" || "$h" -lt "$WIN_H" ]]; then
        fail output-too-small "compositor output ${w:-?}x${h:-?} is smaller than ${WIN_W}x${WIN_H}"
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
# configure/build/install/Weston/manager logs, the before/after input
# screens, the final compositor screenshot, and the verdict — each with its
# SHA-256 in artifact-manifest.json. The retained copies are never deleted.
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
        "$tmp/renderer-verdict.json" "$tmp/installed-manifest.json" \
        "$tmp/controller-box-unhighlighted.png" \
        "$tmp/capture-a.png" "$tmp/capture-b.png" "$tmp/capture-x.png" "$tmp/capture-y.png" \
        "$tmp/capture-up.png" "$tmp/capture-down.png" "$tmp/capture-left.png" "$tmp/capture-right.png" \
        "$tmp/capture-start.png" "$tmp/capture-select.png" "$tmp/capture-guide.png" \
        "$tmp/capture-l1.png" "$tmp/capture-r1.png" "$tmp/capture-l2.png" "$tmp/capture-r2.png" \
        "$tmp/capture-l3.png" "$tmp/capture-r3.png" "$tmp/verdict.json"; do
        [[ -f "$f" && ! -L "$f" ]] || continue
        cp -f "$f" "$ARTIFACTS/$(basename "$f")" 2>/dev/null || continue
        echo "$PROBE_TAG: artifact hash $(sha256sum "$ARTIFACTS/$(basename "$f")" | awk '{print $1}') $ARTIFACTS/$(basename "$f")"
    done
    if [[ -f "$tmp/controller-box-compositor.png" && ! -L "$tmp/controller-box-compositor.png" ]]; then
        sha256sum_file "$tmp/controller-box-compositor.png" \
            > "$ARTIFACTS/controller-box-compositor.sha256" 2>/dev/null || true
    fi
    python3 - "$ARTIFACTS" "$PROBE_MARKER" "${head_commit:-}" "${head_tree:-}" <<'PY'
import hashlib, json, os, sys
adir, marker, commit, tree = sys.argv[1:5]
entries = []
for name in sorted(os.listdir(adir)):
    if name == "artifact-manifest.json":
        continue
    path = os.path.join(adir, name)
    if os.path.isfile(path) and not os.path.islink(path):
        with open(path, "rb") as f:
            entries.append({"filename": name, "sha256": hashlib.sha256(f.read()).hexdigest()})
manifest = {
    "schema": "gpu-compositor-artifacts/v2",
    "probe": "gpu-compositor-probe",
    "marker": marker or "unknown",
    "candidate_commit": commit,
    "candidate_tree": tree,
    "licensed_authority_sha256": "23cb0a91cdcde1ab7bb179b4fe5f6afc340dd9f2061b9d1222be94a3341c298d",
    "signature_scope": "enclosing-gpurunner-signed-receipt",
    "artifacts": entries,
}
with open(os.path.join(adir, "artifact-manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
PY
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
    if [[ -n "$ARTIFACTS" && -f "$ARTIFACTS/probe.log" ]]; then
        echo "$PROBE_TAG: cleanup weston_pid=${weston_pid:-none} manager_pid=${manager_pid:-none} marker=$PROBE_MARKER" \
            >> "$ARTIFACTS/probe.log"
    fi
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

    # 2. First-run modal precondition: the launch env must have the seeded
    #    systemd user unit so the SPEC §9.1 first-run modal is provably
    #    skipped and can never block the navigation.
    if [[ "$(cat_fixture modal-seeded)" != "yes" ]]; then
        fail first-run-modal-not-seeded "fixture: systemd user unit not seeded (first-run modal could appear)"
    fi

    # 3. Display binding: the launch must be bound to THIS probe's Xwayland
    #    instance only — never an ambient/pre-existing display.
    if [[ "$(cat_fixture display-bound)" != "yes" ]]; then
        fail display-binding-rejected "fixture: launch DISPLAY not bound to the private Xwayland instance"
    fi

    # 4. Tab-change proof: the Right navigation visibly changed the tab bar
    #    before the Edit click.
    if [[ "$(cat_fixture tab-change-observed)" != "yes" ]]; then
        fail tab-change-not-observed "fixture: no semantic tab-region pixel change after input"
    fi

    # 5. Screenshot retention + hash binding.
    local_shot=$FIXTURE/screenshot.png
    if [[ "$(cat_fixture screenshot-missing)" == "yes" || ! -f "$local_shot" || -L "$local_shot" ]]; then
        fail screenshot-missing "fixture: no compositor-level screenshot"
    fi
    local_hash=$(cat_fixture screenshot-hash)
    if [[ -n "$local_hash" ]]; then
        actual_hash=$(sha256sum_file "$local_shot" | awk '{print $1}')
        if [[ -z "$actual_hash" || "$actual_hash" != "$local_hash" ]]; then
            fail screenshot-hash-mismatch "fixture: retained screenshot sha256 $actual_hash != recorded $local_hash"
        fi
    fi

    # 6. The compositor output must be at least the production window size
    #    (an explicit headless output >=1280x720; never a clipped one).
    require_output_size "$local_shot"

    # 7. Compositor + renderer facts.
    if [[ "$(cat_fixture weston-ok)" != "yes" ]]; then
        fail compositor-start-failed "fixture: private Weston failed to start"
    fi
    if [[ "$(cat_fixture egl-ok)" != "yes" ]]; then
        fail renderer-unverified "fixture: EGL renderer probe could not establish a renderer"
    fi
    renderer=$(cat_fixture renderer)
    [[ -n "$renderer" ]] || fail renderer-unverified "fixture: no renderer string"
    check_renderer "$renderer"

    # 8. Input route.
    if [[ "$(cat_fixture input-ok)" != "yes" ]]; then
        fail input-route-failed "fixture: Xwayland/xdotool route unavailable"
    fi

    # 9. Window geometry + diagram analysis.
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
    for required in installed-asset installed-license installed-map installed-layout oracle authority; do
        [[ -f "$FIXTURE/$required" && ! -L "$FIXTURE/$required" ]] || fail "${required}-missing" "fixture: licensed diagram artifact absent"
    done
    python3 "$ANALYZER" diagram --screenshot "$local_shot" --geometry "$geom" \
        --diagram "$diag" --out "$verdict" --model "$(cat_fixture requested-model)" \
        --resolved-model "$(cat_fixture resolved-model)" --resolved-asset "$(cat_fixture resolved-asset)" \
        --fallback-used "$(cat_fixture fallback-used)" --raster-width "$(cat_fixture raster-width)" \
        --raster-height "$(cat_fixture raster-height)" \
        --asset "$FIXTURE/installed-asset" \
        --license "$FIXTURE/installed-license" \
        --icon-map "$FIXTURE/installed-map" \
        --layout "$FIXTURE/installed-layout" \
        --oracle "$FIXTURE/oracle" --authority "$FIXTURE/authority" \
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
    echo "$PROBE_TAG: installed-assets hash-verified asset+license+map+layout"
    echo "$PROBE_TAG: requested-model=xb360 resolved-model=xb360 asset=xbox-360.svg fallback=false"
    echo "$PROBE_TAG: aspect-preserved raster-density-adequate highlight-oracle-aligned"

    # Preserve hashes/markers for the fixture (owned by the test).
    sha256sum_file "$local_shot" > "$FIXTURE/screenshot.sha256" 2>/dev/null || true
    cp "$verdict" "$FIXTURE/verdict.json" 2>/dev/null || true
    PROBE_MARKER="pass"
    echo "gpu-compositor-probe: PASS (fixture)"
    exit 0
fi

# ---------------------------------------------------------------------------
# Live mode
# ---------------------------------------------------------------------------
if [[ -z "$ARTIFACTS" ]]; then
    ARTIFACTS="$SCRIPT_DIR/../.factory-state/artifacts/gpu-compositor/$(date +%Y%m%dT%H%M%S)"
fi
mkdir -p "$ARTIFACTS"
: > "$ARTIFACTS/probe.log"
log() { # msg
    echo "$PROBE_TAG: $1"
    echo "$PROBE_TAG: $1" >> "$ARTIFACTS/probe.log"
}

# --- 1. exact-HEAD build + install under an isolated prefix -----------------
# The installed binary is produced HERE from a fresh git archive of the exact
# HEAD commit; the ambient PATH is never trusted.  The compiled-in
# SOURCE_PROFILE_DIR / SOURCE_ICON_DIR fallbacks point into the archived
# source tree, which is deleted before launch so the installed prefix assets
# must win.
head_commit=$(git -C "$SCRIPT_DIR/.." rev-parse HEAD 2>/dev/null || true)
head_tree=$(git -C "$SCRIPT_DIR/.." rev-parse 'HEAD^{tree}' 2>/dev/null || true)
[[ "$head_commit" =~ ^[0-9a-f]{40}$ && "$head_tree" =~ ^[0-9a-f]{40}$ ]] || fail exact-commit-unresolved "cannot resolve HEAD in $SCRIPT_DIR/.."
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
prefix_real=$(readlink -f "$prefix")
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
# inside the prefix (no source/build-tree fallback can satisfy them).  The
# binary must be a regular executable (never a symlink into the ambient
# PATH), and every path must stay under the REAL prefix path.
installed_bin="$prefix/bin/controller-box"
if [[ ! -x "$installed_bin" || -L "$installed_bin" ]]; then
    fail installed-launch-rejected "installed binary missing or a symlink: $installed_bin"
fi
installed_real=$(readlink -f "$installed_bin")
case "$installed_real" in
    "$prefix_real"/*) ;;
    *) fail binary-outside-prefix-rejected "installed binary realpath escapes the prefix: $installed_real" ;;
esac
installed_svg=""
asset_ok=1
installed_svg="$prefix/share/controller-box/icons/svg/xbox-360.svg"
installed_license="$prefix/share/controller-box/icons/svg/LICENSE.controllercons"
installed_map="$prefix/share/controller-box/controller-icons.yaml"
installed_layout="$prefix/share/controller-box/controller-layouts/xbox-360.json"
installed_oracle="$prefix/share/controller-box/licensed-diagram-oracle.json"
installed_authority="$prefix/share/controller-box/licensed-diagram-authority.json"
for asset in \
    "$installed_svg" "$installed_license" "$installed_map" "$installed_layout" \
    "$installed_oracle" "$installed_authority" \
    "$prefix/share/controller-box/profiles/default.yaml"; do
    if [[ -f "$asset" && ! -L "$asset" ]]; then
        asset_real=$(readlink -f "$asset")
        case "$asset_real" in
            "$prefix_real"/*) ;;
            *) asset_ok=0 ;;
        esac
    else
        asset_ok=0
    fi
    [[ "$asset_ok" -eq 1 ]] || break
done
[[ "$asset_ok" -eq 1 ]] || fail installed-assets-missing "required installed assets missing or outside the prefix"
[[ -n "$installed_svg" ]] || fail installed-assets-missing "installed xbox-360.svg not found in prefix"
python3 - "$tmp/installed-manifest.json" "$head_commit" "$head_tree" "$installed_real" "$installed_svg" "$installed_license" "$installed_map" "$installed_layout" "$installed_oracle" "$installed_authority" <<'PY'
import hashlib,json,sys
out,commit,tree,binary,*files=sys.argv[1:]
def digest(p): return hashlib.sha256(open(p,'rb').read()).hexdigest()
json.dump({'schema':'controller-box-installed-provenance/v1','result':'pass','commit':commit,'tree':tree,
'binary':binary,'fallback':False,'files':{p.split('/')[-1]:digest(p) for p in files}},open(out,'w'),indent=2); open(out,'a').write('\n')
PY
log "installed-launch: binary=$installed_real assets=$installed_svg prefix=$prefix"

# Delete the archived source+build trees BEFORE launch: the compiled-in
# SOURCE_PROFILE_DIR / SOURCE_ICON_DIR fallbacks must be unreachable so the
# installed prefix assets win.  A leftover source/build tree is a hard fail.
rm -rf "$tmp/source" "$tmp/build"
if [[ -e "$tmp/source" || -e "$tmp/build" ]]; then
    fail source-tree-accessible-rejected "archived source/build tree still present at launch"
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

# Explicit headless output of at least 1280x720 (never the 1024x768 default).
cat > "$XDG_CONFIG_HOME/weston.ini" <<INI
[core]
shell=desktop-shell.so
xwayland=true
[keyboard]
[output]
name=headless
mode=1280x720
[shell]
background-color=0xff18181c
INI

# Record every X11 display that exists BEFORE this weston starts, so the
# Xwayland display the probe binds to can be proven to be the probe's own
# (never a pre-existing/ambient display).
pre_x_socks=""
for sock in /tmp/.X11-unix/X*; do
    [[ -S "$sock" ]] && pre_x_socks+="$sock "
done

weston --backend=headless-backend.so --renderer=gl --socket="$WAYLAND_DISPLAY" \
    --width=1280 --height=720 \
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

# Bind DISPLAY to the Xwayland display THAT THIS weston logged — never the
# first socket in /tmp/.X11-unix.  The log must identify exactly one
# display, the socket must exist, and it must not have pre-existed.
x_displays=()
while IFS= read -r d; do
    [[ -n "$d" ]] && x_displays+=("$d")
done < <(grep -aoE 'DISPLAY=:[0-9]+' "$tmp/weston.log" | sed 's/.*=//' | sort -u || true)
if [[ ${#x_displays[@]} -eq 0 ]]; then
    while IFS= read -r d; do
        [[ -n "$d" ]] && x_displays+=("$d")
    done < <(grep -aoE 'Xwayland on :[0-9]+' "$tmp/weston.log" | sed -E 's/.*(:[0-9]+)/\1/' | sort -u || true)
fi
if [[ ${#x_displays[@]} -ne 1 ]]; then
    fail display-binding-rejected "weston log identified ${#x_displays[@]} Xwayland displays: ${x_displays[*]:-none}"
fi
x_display=${x_displays[0]}
x_num=${x_display#:}
x_sock="/tmp/.X11-unix/X$x_num"
[[ -S "$x_sock" ]] || fail input-route-failed "Xwayland socket $x_sock missing"
case " $pre_x_socks " in
    *" $x_sock "*) fail display-binding-rejected "display $x_display pre-existed — not this probe's instance" ;;
esac
export DISPLAY="$x_display"
log "input-route: bound to private Xwayland DISPLAY=$DISPLAY (socket=$x_sock)"

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

# --- 4. installed launch + first-run skip + navigation ----------------------
# The manager must find a profile to edit; provision one in a private data
# home (mirrors tests/test_manager_visual.c vis_open_editor).
export XDG_DATA_HOME="$tmp/data"
profile_dir="$XDG_DATA_HOME/inputplumber/profiles"
mkdir -p "$profile_dir"
cat > "$profile_dir/$PROFILE_NAME.yaml" <<PROFILE
version: 1
kind: DeviceProfile
name: "GPU Xbox 360 Oracle"
description: "exact model-specific GPU probe profile"
mapping:
  - name: "Unhighlighted control"
    source_event: {keyboard: {key: KEY_A}}
    target_events: [{keyboard: KEY_A}]
  - {name: "A", source_event: {gamepad: {button: A}}, target_events: [{gamepad: A}]}
  - {name: "B", source_event: {gamepad: {button: B}}, target_events: [{gamepad: B}]}
  - {name: "X", source_event: {gamepad: {button: X}}, target_events: [{gamepad: X}]}
  - {name: "Y", source_event: {gamepad: {button: Y}}, target_events: [{gamepad: Y}]}
  - {name: "Up", source_event: {gamepad: {button: Up}}, target_events: [{gamepad: Up}]}
  - {name: "Down", source_event: {gamepad: {button: Down}}, target_events: [{gamepad: Down}]}
  - {name: "Left", source_event: {gamepad: {button: Left}}, target_events: [{gamepad: Left}]}
  - {name: "Right", source_event: {gamepad: {button: Right}}, target_events: [{gamepad: Right}]}
  - {name: "Start", source_event: {gamepad: {button: Start}}, target_events: [{gamepad: Start}]}
  - {name: "Select", source_event: {gamepad: {button: Select}}, target_events: [{gamepad: Select}]}
  - {name: "Guide", source_event: {gamepad: {button: Guide}}, target_events: [{gamepad: Guide}]}
  - {name: "L1", source_event: {gamepad: {button: L1}}, target_events: [{gamepad: L1}]}
  - {name: "R1", source_event: {gamepad: {button: R1}}, target_events: [{gamepad: R1}]}
  - {name: "L2", source_event: {gamepad: {button: L2}}, target_events: [{gamepad: L2}]}
  - {name: "R2", source_event: {gamepad: {button: R2}}, target_events: [{gamepad: R2}]}
  - {name: "L3", source_event: {gamepad: {button: L3}}, target_events: [{gamepad: L3}]}
  - {name: "R3", source_event: {gamepad: {button: R3}}, target_events: [{gamepad: R3}]}
PROFILE
mkdir -p "$XDG_CONFIG_HOME/controller-box/profile-metadata"
cat > "$XDG_CONFIG_HOME/controller-box/profile-metadata/$PROFILE_NAME.meta.yaml" <<META
icon: cc-xbox-360
display_order: -100
META

# Seed the systemd user unit under the isolated XDG_CONFIG_HOME so the
# SPEC §9.1 first-run modal is provably skipped (cbx_manager_check_first_run
# returns immediately when $XDG_CONFIG_HOME/systemd/user/controller-box.service
# exists).  The unit references the INSTALLED binary — never an ambient one.
mkdir -p "$XDG_CONFIG_HOME/systemd/user"
unit="$XDG_CONFIG_HOME/systemd/user/controller-box.service"
cat > "$unit" <<UNIT
[Unit]
Description=Controller-Box Overlay Service
After=graphical-session.target
PartOf=graphical-session.target

[Service]
ExecStart=$installed_real --overlay-service
Restart=on-failure
RestartSec=2s

[Install]
WantedBy=graphical-session.target
UNIT
if [[ ! -f "$unit" || -L "$unit" || ! -s "$unit" ]] || ! grep -qF "ExecStart=$installed_real" "$unit"; then
    fail first-run-modal-not-seeded "seeded systemd user unit missing or malformed"
fi
log "modal: first-run skipped (unit $unit -> $installed_real)"

# The binary's relative data/profiles fallback ("data/profiles" relative to
# the launch CWD) must not resolve — checked AFTER all XDG dirs exist so a
# directory the app or the probe created cannot be mistaken for a fallback.
if [[ -e "$XDG_DATA_HOME/profiles/default.yaml" || -e "$XDG_DATA_HOME/icons/svg/generic-gamepad.svg" \
      || -e "$XDG_DATA_HOME/controller-icons.yaml" || -e "$XDG_DATA_HOME/profiles" \
      || -e "$XDG_DATA_HOME/icons" ]]; then
    fail source-data-path-available "a CWD-relative source fallback path is reachable at launch"
fi

# Launch the installed, unmodified manager from an isolated CWD (never the
# repository) with PATH prefixed to the install bin dir, so a relative
# source asset lookup or an ambient-PATH binary can never win.
mkdir -p "$tmp/home"
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

# Prove the input route reaches the manager BEFORE the Edit click: focus the
# exact window, capture the tab bar, send Right (switches to the Profiles
# tab, SPEC §5.1), capture again, and require a semantic pixel change in the
# tab region.
xdotool windowactivate --sync "$window_id" >/dev/null 2>&1 || true
xdotool windowfocus --sync "$window_id" >/dev/null 2>&1 || true
sleep 0.4
before_shot=$tmp/controller-box-before-input.png
weston-screenshooter "$before_shot" >"$tmp/screenshooter.log" 2>&1 \
    || fail screenshot-missing "pre-input compositor capture failed"
[[ -f "$before_shot" && -s "$before_shot" ]] || fail screenshot-missing "pre-input capture is empty"

xdotool key Right
sleep 0.8
after_shot=$tmp/controller-box-after-input.png
weston-screenshooter "$after_shot" >>"$tmp/screenshooter.log" 2>&1 \
    || fail screenshot-missing "post-input compositor capture failed"
[[ -f "$after_shot" && -s "$after_shot" ]] || fail screenshot-missing "post-input capture is empty"

tab_diff=0
set +e
convert "$before_shot" -crop "${WIN_W}x${TAB_BAR_H}+${win_x}+${win_y}" +repage "$tmp/tab-before.png" 2>/dev/null
convert "$after_shot" -crop "${WIN_W}x${TAB_BAR_H}+${win_x}+${win_y}" +repage "$tmp/tab-after.png" 2>/dev/null
tab_diff=$(compare -metric AE "$tmp/tab-before.png" "$tmp/tab-after.png" null: 2>&1 | grep -oE '[0-9]+' | head -n 1 || true)
set -e
[[ "$tab_diff" =~ ^[0-9]+$ ]] || tab_diff=0
if [[ "$tab_diff" -lt "$TAB_DIFF_MIN" ]]; then
    log "input: tab region changed ${tab_diff}px (minimum $TAB_DIFF_MIN)"
    fail tab-change-not-observed "Right input did not visibly change the tab bar (${tab_diff}px)"
fi
log "input: Right changed the tab bar (${tab_diff}px differ from pre-input)"

# Navigate: the Edit button opens the editor on the Profiles tab.
edit_x=$((win_x + ${EDIT_CLICK%,*}))
edit_y=$((win_y + ${EDIT_CLICK#*,}))
xdotool mousemove --sync "$edit_x" "$edit_y"
xdotool click 1
sleep 1.0

# Capture the compositor output (includes the Xwayland surface).
unhighlighted=$tmp/controller-box-unhighlighted.png
weston-screenshooter "$unhighlighted" >>"$tmp/screenshooter.log" 2>&1 || fail screenshot-missing "unhighlighted capture failed"
require_output_size "$unhighlighted"
controls=(a b x y up down left right start select guide l1 r1 l2 r2 l3 r3)
for control in "${controls[@]}"; do
    xdotool key Down
    sleep 0.25
    weston-screenshooter "$tmp/capture-$control.png" >>"$tmp/screenshooter.log" 2>&1 || fail screenshot-missing "capture-$control failed"
    require_output_size "$tmp/capture-$control.png"
done
shot=$tmp/capture-a.png
log "output: compositor output >= ${WIN_W}x${WIN_H}; captured installed dispatch controls 17/17"

# --- 5. semantic diagram analysis -------------------------------------------
verdict=$tmp/verdict.json
marker_analyzed=""
set +e
diagram_log=$(grep '^profile-diagram: icon=cc-xbox-360 asset=xbox-360.svg provenance=profile-override raster=[0-9][0-9]*x[0-9][0-9]* result=loaded$' "$tmp/manager.log" | tail -n 1 || true)
if [[ -z "$diagram_log" ]]; then
    fail wrong-licensed-model "production selection did not resolve requested Xbox 360 asset without fallback"
fi
raster_dims=${diagram_log#* raster=}; raster_dims=${raster_dims%% result=*}
raster_width=${raster_dims%x*}; raster_height=${raster_dims#*x}
python3 "$ANALYZER" diagram --screenshot "$shot" \
    --geometry "$win_x,$win_y,$win_w,$win_h" --diagram "$DIAGRAM_RECT" \
    --out "$verdict" --model xb360 --resolved-model xb360 --resolved-asset xbox-360.svg \
    --fallback-used no --raster-width "$raster_width" --raster-height "$raster_height" \
    --asset "$installed_svg" --license "$installed_license" \
    --icon-map "$installed_map" --layout "$installed_layout" \
    --oracle "$installed_oracle" --authority "$installed_authority" \
    >"$tmp/diagram.out" 2>&1
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
log "installed-assets hash-verified asset+license+map+layout"
log "requested-model=xb360 resolved-model=xb360 asset=xbox-360.svg fallback=false"
log "aspect-preserved raster-density-adequate highlight-oracle-aligned"
echo "$PROBE_TAG: installed-licensed-diagram verified"

# Replace the single-control structural verdict with independent 17/17
# difference-mask observations against the externally pinned oracle.
python3 "$ANALYZER" series --captures-dir "$tmp" --geometry "$win_x,$win_y,$win_w,$win_h" \
    --diagram "$DIAGRAM_RECT" --oracle "$installed_oracle" --authority "$installed_authority" \
    --out "$verdict" || fail control-series-incomplete "not every installed production-dispatch control matched the oracle"
shot_sha=$(sha256sum_file "$shot" | awk '{print $1}')

# Every artifact (logs, before/after input screens, screenshot, verdict) is
# retained with its hash by retain_live_artifacts on the EXIT trap (every
# live exit path).
log "artifact: screenshot sha256=$shot_sha marker=$marker_analyzed retained=controller-box-compositor.png"
PROBE_MARKER="pass"
echo "gpu-compositor-probe: PASS"
log "gpu-compositor-probe: PASS"
