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

# Normal operator verification retains the historical root evidence path.
# A production campaign supplies one explicit absolute override inside its
# already-reserved private namespace; no other override shape is accepted.
INSTALLED_EVIDENCE_PATH=$(python3 - "$PROJECT_ROOT" "${FACTORY_INSTALLED_FUNCTIONAL_EVIDENCE_PATH:-}" <<'PY'
import os, re, stat, sys
from pathlib import Path
root = Path(sys.argv[1]).absolute()
override = sys.argv[2]
if not override:
    print(root / '.factory-state/installed-functional-evidence.env')
    raise SystemExit(0)
path = Path(override)
if not path.is_absolute():
    raise SystemExit('verify-project: installed-evidence override must be absolute')
try:
    relative = path.relative_to(root)
except ValueError:
    raise SystemExit('verify-project: installed-evidence override escapes the repository')
parts = relative.parts
if (len(parts) != 4 or parts[:2] != ('.factory-state', 'campaigns')
        or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}', parts[2])
        or parts[3] != 'installed-functional-evidence.env'):
    raise SystemExit('verify-project: installed-evidence override must be the exact campaign-owned path')
for directory in (root / parts[0], root / parts[0] / parts[1], path.parent):
    info = directory.lstat()
    if (not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode)
            or info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o700):
        raise SystemExit(f'verify-project: unsafe campaign evidence directory: {directory}')
print(path)
PY
) || exit $?
export FACTORY_INSTALLED_FUNCTIONAL_EVIDENCE_PATH="$INSTALLED_EVIDENCE_PATH"

# The project gate is bound to the declared Nix environment (shell.nix). The
# authenticated boundary (scripts/nix-gate.sh + scripts/nix-gate-exec.sh)
# replaces the forgeable CBX_VERIFY_IN_NIX_SHELL / IN_NIX_SHELL trust: this
# script never trusts a caller-set variable to claim it is already "inside
# Nix". If it is not running under the authenticated declared environment it
# re-executes through the wrapper, which FAILS rather than silently verifying
# against undeclared host packages when Nix is unavailable.
source "$PROJECT_ROOT/scripts/nix-gate.sh"
if ! nix_gate_require full; then
    exec "$PROJECT_ROOT/scripts/nix-gate-exec.sh" \
        "$PROJECT_ROOT/scripts/verify-project.sh" "$@"
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
# (for example, a bind-mount path differs between verification environments).
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cached_source=$(grep 'CMAKE_HOME_DIRECTORY:INTERNAL=' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | cut -d= -f2)
    if [[ "$cached_source" != "$PROJECT_ROOT" ]]; then
        echo "verify-project: CMake cache source mismatch ($cached_source != $PROJECT_ROOT); rebuilding" >&2
        rm -rf "$BUILD_DIR"
    fi
fi
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD_DIR" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"

# The complete project verification now runs the adversarial visual-audit suite
# (test-visual-audit.sh, a bound verifier gate). Its 13f atomic-capture
# publication regressions drive the real installed binary, so the built artifact
# is installed to a test-owned prefix and exported as
# VISUAL_AUDIT_INSTALL_PREFIX (which the gate's binary discovery consults first;
# a mutable caller-supplied value is validated by the driver's provenance
# hardening before it is ever launched). Under the authenticated Nix inner gate
# the 13f installed-binary-not-found case fails rather than skips.
INSTALL_PREFIX=${CBX_VERIFY_INSTALL_PREFIX:-"$PROJECT_ROOT/.test-install"}
mkdir -p "$INSTALL_PREFIX"
cmake --install "$BUILD_DIR" --prefix "$INSTALL_PREFIX" >/dev/null
if [[ -x "$INSTALL_PREFIX/bin/controller-box" ]]; then
    export VISUAL_AUDIT_INSTALL_PREFIX="$INSTALL_PREFIX"
else
    echo "verify-project: installed binary not found at $INSTALL_PREFIX/bin/controller-box" >&2
    exit 2
fi

# The acceptance gates are the tracked test-discovery contract
# (.factory/verifier-acceptance.json, schema ralph-verifier-acceptance/v1).
# The campaign binding covers that manifest, so adding a gate (strict
# strengthening) auto-rebinds with an audit record instead of halting the
# campaign, while removing a gate or changing the entrypoint requires an
# explicit independently audited policy commit.
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
        if name in ('test_packaging.sh', 'test-visual-audit.sh'):
            # test_packaging.sh and test-visual-audit.sh are strict-rc0 gates.
            # test-visual-audit.sh is non-skipping under the authenticated Nix
            # inner gate, so a 77 (skip) return would be a silent false-pass,
            # never a legitimate skip: it is rejected here (the dead 77 is
            # removed for this gate) so the adversarial visual/atomic-capture
            # regressions can never pass by skipping.
            if result.returncode != 0:
                raise SystemExit(f'verify-project: gate {name} failed (exit {result.returncode})')
        elif result.returncode not in (0, 77):
            raise SystemExit(f'verify-project: gate {name} failed (exit {result.returncode})')
        elif result.returncode == 77:
            print(f'verify-project: gate {name} skipped (77)')
PY
python3 - "$PROJECT_ROOT" "$INSTALLED_EVIDENCE_PATH" "$(git rev-parse HEAD)" <<'PY'
import os, stat, sys, tempfile
from pathlib import Path
root, target, commit = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
if target == root / '.factory-state/installed-functional-evidence.env':
    target.parent.mkdir(mode=0o700, exist_ok=True)
    info = target.parent.lstat()
    if (not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode)
            or info.st_uid != os.getuid()):
        raise SystemExit('verify-project: unsafe default evidence directory')
if target.is_symlink() or (target.exists() and not target.is_file()):
    raise SystemExit('verify-project: unsafe installed-functional evidence target')
body = (
    'schema=factory-installed-functional/v1\n'
    f'commit={commit}\n'
    'test=test_installed_functional\n'
    'result=PASS\n'
    'skipped=0\n'
).encode()
fd, temporary = tempfile.mkstemp(prefix='.installed-functional-evidence.', dir=target.parent)
try:
    with os.fdopen(fd, 'wb') as stream:
        stream.write(body)
        stream.flush()
        os.fsync(stream.fileno())
    os.chmod(temporary, 0o600)
    # Atomic no-replace publication (Task 34/35): os.link fails with
    # FileExistsError if the destination was created or swapped after the
    # preflight (including a symlink or hardlink), so a destination
    # replacement can never overwrite the sentinel.  The temporary bytes are
    # fsynced before publication; the containing directory is fsynced after.
    try:
        os.link(temporary, target)
    except FileExistsError:
        raise SystemExit(
            'verify-project: installed-functional evidence destination '
            'already exists; refusing to overwrite (atomic no-replace)'
        )
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
    dir_fd = os.open(target.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(dir_fd)
    finally:
        os.close(dir_fd)
PY
echo "verify-project: Controller-Box build, tests, functional acceptance, smoke checks, and packaging passed"
