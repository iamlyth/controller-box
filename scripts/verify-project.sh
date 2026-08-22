#!/usr/bin/env bash
# Full Controller-Box verification used by the maintenance completion gate.
set -euo pipefail

# The campaign executes this verifier through a retained descriptor
# (/proc/self/fd/N) so a pathname swap cannot substitute another file. The
# procfs descriptor path is not a real directory, so resolve it to the
# canonical script path before deriving SCRIPT_DIR.
SCRIPT_SOURCE=${BASH_SOURCE[0]}
if [[ "$SCRIPT_SOURCE" == /proc/self/fd/* ]]; then
    SCRIPT_SOURCE=$(readlink -f -- "$SCRIPT_SOURCE") || {
        echo "verify-project: cannot resolve retained descriptor script path" >&2
        exit 2
    }
fi
SCRIPT_DIR=$(cd -- "$(dirname -- "$SCRIPT_SOURCE")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${CBX_VERIFY_BUILD_DIR:-build-maintenance-verify}
cd -- "$PROJECT_ROOT"

# The project gate is Nix-bound even when a host happens to provide similarly
# named development packages. Host package versions and feature defaults are
# not the declared verification environment.
if [[ ${CBX_VERIFY_IN_NIX_SHELL:-0} != 1 && -z ${IN_NIX_SHELL:-} ]] \
        && command -v nix-shell >/dev/null; then
    export CBX_VERIFY_IN_NIX_SHELL=1
    exec nix-shell --run './scripts/verify-project.sh'
fi

required=(sdl2 SDL2_ttf SDL2_image libsystemd yaml-0.1 cmocka)
if ! pkg-config --exists "${required[@]}"; then
    echo "verify-project: required native dependencies are unavailable" >&2
    exit 2
fi

# Nix gate for the visual-validator negative regressions (test-visual-audit.sh
# 13e). The complete project verification always runs under the declared Nix
# environment (shell.nix provides ImageMagick), so convert must be present
# here. Asserting it guarantees the non-skipping blank-overlay/unselected-list
# regressions can never silently skip for lack of the validator prerequisite.
if ! command -v convert >/dev/null 2>&1; then
    echo "verify-project: ImageMagick convert is required for the visual validator" >&2
    exit 2
fi

# Nix gate for the atomic-capture publication regressions (test-visual-audit.sh
# 13f). The complete project verification always runs under the declared Nix
# environment (which provides Xvfb/xdotool/ImageMagick, with dbus-daemon
# available transitively), so the display/capture toolchain must be present
# here. Asserting it guarantees the 13f non-skipping atomic publication (incl.
# fsync ordering, TOCTOU race, symlink/hardlink refusal, and signal-withdrawal)
# regressions can never silently skip for lack of a display server or capture
# tool.
for tool in Xvfb xdotool import convert dbus-daemon; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "verify-project: $tool is required for the visual capture publication regressions" >&2
        exit 2
    fi
done

# Invalidate the CMake cache when the source directory has changed
# (e.g. bind-mount path differs between Ralph and campaign environments).
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cached_source=$(grep 'CMAKE_HOME_DIRECTORY:INTERNAL=' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | cut -d= -f2)
    if [[ "$cached_source" != "$PROJECT_ROOT" ]]; then
        echo "verify-project: CMake cache source mismatch ($cached_source != $PROJECT_ROOT); rebuilding" >&2
        rm -rf "$BUILD_DIR"
    fi
fi
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD_DIR" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"

# The acceptance gates are the tracked test-discovery contract
# (.factory/verifier-acceptance.json, schema ralph-verifier-acceptance/v1).
# The campaign binding covers that manifest, so adding a gate (strict
# strengthening) auto-rebinds with an audit record instead of halting the
# campaign, while removing a gate or changing the entrypoint requires the
# audited operator pathway (scripts/ralph-verifier-migrate.sh).
python3 - "$BUILD_DIR" <<'PY' || exit 1
import json, subprocess, sys
from pathlib import Path
root = Path.cwd()
build_dir = Path(sys.argv[1])
manifest = json.load(open(root / '.factory/verifier-acceptance.json', encoding='utf-8'))
if manifest.get('schema') != 'ralph-verifier-acceptance/v1':
    raise SystemExit('verify-project: invalid verifier acceptance manifest schema')
gates = manifest.get('gates')
if not isinstance(gates, list) or not gates:
    raise SystemExit('verify-project: verifier acceptance manifest has no gates')
for gate in gates:
    if not isinstance(gate, dict) or set(gate) != {'name', 'args'}:
        raise SystemExit(f'verify-project: invalid gate entry: {gate!r}')
    name = gate['name']
    args = gate['args']
    if not isinstance(name, str) or not name or '/' in name or name.startswith('.'):
        raise SystemExit(f'verify-project: invalid gate name: {name!r}')
    if not isinstance(args, list) or not all(isinstance(a, str) and a for a in args):
        raise SystemExit(f'verify-project: invalid gate args: {name!r}')
    print(f'verify-project: running gate {name}', flush=True)
    if name == 'ctest':
        subprocess.run(
            ['ctest', '--test-dir', str(build_dir), '--output-on-failure', '--timeout', '120'],
            check=True,
        )
    elif name == 'test_installed_functional':
        result = subprocess.run(
            ['ctest', '--test-dir', str(build_dir), '--no-tests=error', '--timeout', '120',
             '-R', '^test_installed_functional$', '--output-on-failure'],
            capture_output=True, text=True,
        )
        combined = result.stdout + result.stderr
        if result.returncode != 0 or any(t in combined for t in ('Skipped', 'Not Run', '0 tests passed')):
            print(combined, file=sys.stderr)
            raise SystemExit(f'verify-project: installed functional acceptance was skipped or failed (gate {name})')
    else:
        argv = [str(root / 'tests' / name)]
        argv += [str(build_dir) if a == '{BUILD_DIR}' else a for a in args]
        result = subprocess.run(argv)
        if name == 'test_packaging.sh':
            if result.returncode != 0:
                raise SystemExit(f'verify-project: gate {name} failed (exit {result.returncode})')
        elif result.returncode not in (0, 77):
            raise SystemExit(f'verify-project: gate {name} failed (exit {result.returncode})')
        elif result.returncode == 77:
            print(f'verify-project: gate {name} skipped (77)')
PY
mkdir -p .factory-state
cat > .factory-state/installed-functional-evidence.env <<EOF
schema=factory-installed-functional/v1
commit=$(git rev-parse HEAD)
test=test_installed_functional
result=PASS
skipped=0
EOF
echo "verify-project: Controller-Box build, tests, functional acceptance, smoke checks, and packaging passed"
