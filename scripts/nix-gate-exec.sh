#!/usr/bin/env bash
# Authenticated Nix verification boundary.
#
# The campaign/completion verifier runs inner scripts THROUGH this wrapper so
# the "am I in the declared Nix environment" decision is made here rather than
# by a caller-set environment variable. The wrapper:
#   1. clears the forgeable CBX_VERIFY_IN_NIX_SHELL / IN_NIX_SHELL markers;
#   2. generates a fresh 64-hex boundary nonce in a mode-0600, current-user-owned,
#      non-symlink regular file under a mode-0700 directory;
#   3. re-execs into `nix-shell` (the declared shell.nix), carrying the capability
#      through the environment;
#   4. on the inner side, verifies the capability and nix-store authority, then
#      execs the real inner script.
# If nix-shell is unavailable the verifier FAILS (exit 2) instead of silently
# running against undeclared host packages.
#
# Usage:
#   nix-gate-exec.sh <inner-script> [args...]          # outer/wrapper side
#   nix-gate-exec.sh inner <inner-script> [args...]    # inner side (inside nix)
set -euo pipefail

SELF_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./nix-gate.sh
# shellcheck disable=SC1091  # sourced library is validated on its own
source "$SELF_DIR/nix-gate.sh"

if [[ "${1:-}" == "inner" ]]; then
    shift
    [[ $# -ge 1 ]] || { echo "nix-gate-exec: missing inner script" >&2; exit 2; }
    INNER=$1
    shift
    # Inner side: authenticate, then exec the real script.
    if ! nix_gate_require full; then
        echo "nix-gate-exec: inner boundary not authenticated" >&2
        exit 2
    fi
    exec "$INNER" "$@"
fi

# Outer side.
[[ $# -ge 1 ]] || { echo "usage: nix-gate-exec.sh <inner-script> [args...]" >&2; exit 2; }
INNER=$1
shift

unset CBX_VERIFY_IN_NIX_SHELL IN_NIX_SHELL 2>/dev/null || true

NONCE=$(python3 -c 'import secrets; print(secrets.token_hex(32))')
GATE_DIR=$(mktemp -d "${TMPDIR:-/tmp}/cbx-nix-gate.XXXXXX")
trap 'rm -rf -- "$GATE_DIR"' EXIT
chmod 700 "$GATE_DIR"
umask 077
printf '%s' "$NONCE" > "$GATE_DIR/nonce"
chmod 600 "$GATE_DIR/nonce"

if ! command -v nix-shell >/dev/null 2>&1; then
    echo "nix-gate-exec: Nix unavailable; refusing to verify against undeclared host packages" >&2
    exit 2
fi

export CBX_NIX_GATE_NONCE="$NONCE"
export CBX_NIX_GATE_FILE="$GATE_DIR/nonce"

# Build the nix-shell --run command with robust quoting.
cmd_parts=( "$SELF_DIR/nix-gate-exec.sh" "inner" "$INNER" )
for a in "$@"; do
    cmd_parts+=( "$a" )
done
printf -v nix_run '%q ' "${cmd_parts[@]}"
exec nix-shell --run "$nix_run"
