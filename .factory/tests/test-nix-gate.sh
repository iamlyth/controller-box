#!/usr/bin/env bash
# Regression for the authenticated Nix gate (.factory/tools/nix-gate.sh,
# .factory/tools/nix-gate-exec.sh, .factory/tools/nix-gate-check.py).
# shellcheck disable=SC2016  # $1/$2 inside single-quoted bash -c are positional
#
# Proves the forgeable CBX_VERIFY_IN_NIX_SHELL / IN_NIX_SHELL environment-variable
# trust is retired and that the wrapper-generated capability + Nix-store authority
# now decide whether a verifier runs under the declared Nix environment:
#   * no capability and not inside nix-shell -> not authenticated (optional SKIP);
#   * a forged/incomplete capability -> hard refusal (exit 2), never a silent skip;
#   * a claimed-but-incomplete Nix store environment -> hard refusal (exit 2);
#   * the wrapper fails rather than verifying against host packages when nix-shell
#     is unavailable;
#   * under nix-shell the declared tools resolve under /nix/store (store authority).
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
cd -- "$PROJECT_ROOT"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail() { echo "test-nix-gate: $*" >&2; exit 1; }
ok()  { echo "test-nix-gate: $*"; }

# --- 1. Source invariants: the forgeable env var is retired -----------------
for f in scripts/verify-project.sh scripts/verify-sanitizers.sh .factory/tests/test-visual-audit.sh; do
    # Only actual assignment/read usage is forbidden (export CBX_VERIFY_IN_NIX_SHELL=1
    # and ${CBX_VERIFY_IN_NIX_SHELL:-0}); prose comments explaining the retirement
    # may mention the legacy name.
    if grep -Eq 'CBX_VERIFY_IN_NIX_SHELL=|CBX_VERIFY_IN_NIX_SHELL:-' "$f"; then
        fail "forgeable CBX_VERIFY_IN_NIX_SHELL must be retired from $f"
    fi
done
ok "forgeable CBX_VERIFY_IN_NIX_SHELL is retired from the verifier scripts"

# The normal installed-evidence default must not be exported before the Nix
# wrapper re-exec. Otherwise the authenticated child mistakes that default for
# a caller-supplied campaign override and rejects its non-campaign path.
python3 - scripts/verify-project.sh <<'PY' || fail "installed-evidence export precedes authenticated Nix re-exec"
import sys
text = open(sys.argv[1], encoding="utf-8").read()
reexec = text.index('if ! nix_gate_require full; then')
export = text.index('export FACTORY_INSTALLED_FUNCTIONAL_EVIDENCE_PATH=')
if export <= reexec:
    raise SystemExit(1)
PY
ok "normal installed-evidence default survives authenticated Nix re-exec"

# --- shared runner: source the gate and call nix_gate_require <mode> ---------
run_gate() { # <mode> ...(env vars already set in the subprocess env)
    local mode=$1
    env -u CBX_NIX_GATE_FILE -u CBX_NIX_GATE_NONCE -u IN_NIX_SHELL \
        bash -c 'source "$1/.factory/tools/nix-gate.sh"; nix_gate_require "$2"' \
        _ "$PROJECT_ROOT" "$mode"
}

expect_rc() { # <expected> <label> <cmd...>
    local expected=$1 label=$2; shift 2
    set +e
    "$@" >"$tmp/out" 2>&1
    local rc=$?
    set -e
    [[ $rc -eq "$expected" ]] \
        || fail "$label: expected exit $expected, got $rc ($(cat "$tmp/out"))"
    ok "$label (exit $expected)"
}

# --- 2. Capability verification robustness (deterministic negatives) --------
expect_rc 1 "no capability, not in nix-shell -> optional SKIP" \
    run_gate optional

# forged: file content does not match the presented nonce
CAP="$tmp/cap"; mkdir -p "$CAP"; chmod 700 "$CAP"
printf 'forged-content' > "$CAP/nonce"; chmod 600 "$CAP/nonce"
expect_rc 2 "forged nonce content is refused" \
    env CBX_NIX_GATE_FILE="$CAP/nonce" CBX_NIX_GATE_NONCE="expected-nonce" \
    bash -c 'source "$1/.factory/tools/nix-gate.sh"; nix_gate_require optional' _ "$PROJECT_ROOT"

# forged: file mode is not exactly 0600 (world/group readable)
printf 'x' > "$CAP/nonce2"; chmod 0644 "$CAP/nonce2"
expect_rc 2 "non-0600 capability file is refused" \
    env CBX_NIX_GATE_FILE="$CAP/nonce2" CBX_NIX_GATE_NONCE="x" \
    bash -c 'source "$1/.factory/tools/nix-gate.sh"; nix_gate_require optional' _ "$PROJECT_ROOT"

# forged: capability file is a symlink (O_NOFOLLOW refuses it)
printf 'x' > "$CAP/nonce-real"; chmod 600 "$CAP/nonce-real"
ln -s "$CAP/nonce-real" "$CAP/nonce-link"
expect_rc 2 "symlink capability file is refused" \
    env CBX_NIX_GATE_FILE="$CAP/nonce-link" CBX_NIX_GATE_NONCE="x" \
    bash -c 'source "$1/.factory/tools/nix-gate.sh"; nix_gate_require optional' _ "$PROJECT_ROOT"

# incomplete: file set but nonce not presented
expect_rc 2 "incomplete capability (file without nonce) is refused" \
    env CBX_NIX_GATE_FILE="$CAP/nonce" \
    bash -c 'source "$1/.factory/tools/nix-gate.sh"; nix_gate_require optional' _ "$PROJECT_ROOT"

# valid capability but presented outside nix-shell (only assertable outside nix)
printf 'valid-nonce' > "$CAP/valid"; chmod 600 "$CAP/valid"
if [[ -z "${IN_NIX_SHELL:-}" ]]; then
    expect_rc 2 "valid capability outside nix-shell is refused" \
        env CBX_NIX_GATE_FILE="$CAP/valid" CBX_NIX_GATE_NONCE="valid-nonce" \
        bash -c 'source "$1/.factory/tools/nix-gate.sh"; nix_gate_require optional' _ "$PROJECT_ROOT"
else
    echo "test-nix-gate: (inside nix-shell a valid capability is authenticated)"
fi

# claimed-but-incomplete store environment: a declared tool is missing
expect_rc 2 "missing declared store tool fails closed" \
    env IN_NIX_SHELL=impure NIX_GATE_TOOLS="definitely-not-a-real-tool-xyz" \
    bash -c 'source "$1/.factory/tools/nix-gate.sh"; nix_gate_require optional' _ "$PROJECT_ROOT"

# --- 3. Fail if Nix unavailable (wrapper never verifies host packages) ------
# Build a PATH with the wrapper's required tools (dirname, python3, mktemp,
# chmod, rm) but WITHOUT nix-shell, so the wrapper's Nix-unavailable branch is
# exercised. If nix-shell is unavoidably on that PATH, we skip the assertion
# rather than mis-asserting.
NO_NIX_TOOL_DIRS=""
for t in dirname python3 mktemp chmod rm; do
    d=$(dirname -- "$(command -v "$t")")
    case ":$NO_NIX_TOOL_DIRS:" in *":$d:"*) ;; *) NO_NIX_TOOL_DIRS="$NO_NIX_TOOL_DIRS:$d" ;; esac
done
NO_NIX_PATH="${NO_NIX_TOOL_DIRS#:}"
if ! (PATH="$NO_NIX_PATH" command -v nix-shell >/dev/null 2>&1); then
    expect_rc 2 "wrapper fails when nix-shell is unavailable" \
        env -u CBX_NIX_GATE_FILE -u CBX_NIX_GATE_NONCE -u IN_NIX_SHELL PATH="$NO_NIX_PATH" \
        "$(command -v bash)" .factory/tools/nix-gate-exec.sh /bin/true
else
    echo "test-nix-gate: (nix-shell is on the no-nix PATH here; wrapper-unavailable case skipped)"
fi

# --- 4. Store authority: declared tools resolve under /nix/store -------------
if [[ -n "${IN_NIX_SHELL:-}" ]]; then
    # no capability but genuinely inside nix-shell with store-built tools
    expect_rc 0 "nix-store authority authenticates inside nix-shell" \
        env -u CBX_NIX_GATE_FILE -u CBX_NIX_GATE_NONCE IN_NIX_SHELL="$IN_NIX_SHELL" \
        bash -c 'source "$1/.factory/tools/nix-gate.sh"; nix_gate_require optional' _ "$PROJECT_ROOT"
    # positive wrapper round-trip: capability + nix-store authority -> exec inner
    cat > "$tmp/inner.sh" <<INNER
#!/usr/bin/env bash
set -euo pipefail
source "$PROJECT_ROOT/.factory/tools/nix-gate.sh"
if nix_gate_require full; then
    echo "NIX_GATE_AUTH_OK"
else
    exit 1
fi
INNER
    chmod +x "$tmp/inner.sh"
    expect_rc 0 "authenticated wrapper round-trip executes the inner script" \
        bash .factory/tools/nix-gate-exec.sh "$tmp/inner.sh"
    grep -q "NIX_GATE_AUTH_OK" "$tmp/out" \
        || fail "wrapper round-trip did not reach the authenticated inner script"
    ok "wrapper round-trip reached the authenticated inner script"
else
    echo "test-nix-gate: store-authority and wrapper round-trip run inside nix-shell"
fi

echo "test-nix-gate: authenticated Nix gate regressions passed"
