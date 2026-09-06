#!/usr/bin/env bash
# Authenticated Nix-gate boundary (sourced by verifier scripts).
#
# Replaces the forgeable CBX_VERIFY_IN_NIX_SHELL / IN_NIX_SHELL trust that let a
# host caller decide whether the full verification ran inside the declared Nix
# environment (shell.nix). The gate has two non-secret-but-verified layers:
#   1. A wrapper-generated capability (.factory/tools/nix-gate-exec.sh): a fresh 64-hex
#      nonce in a mode-0600, current-user-owned, non-symlink regular file. Inner
#      scripts require this capability AND being genuinely inside nix-shell, so a
#      single caller-set environment variable can no longer flip the gate.
#   2. Nix-store authority: the declared tools (convert, Xvfb, xdotool, import,
#      dbus-daemon, pkg-config) must resolve under the immutable, content-addressed
#      /nix/store. A Nix-built artifact is the honest, non-forgeable environment
#      signal (it proves "Nix-built", the ceiling without a pinned flake.lock).
# Hard-fail rule: a verifier that is NOT inside Nix and cannot enter it fails
# (exit 2) rather than silently running against undeclared host packages.

NIX_GATE_PY="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/nix-gate-check.py"
NIX_GATE_TOOLS="${NIX_GATE_TOOLS:-convert Xvfb xdotool import dbus-daemon pkg-config}"

# usage: nix_gate_require <full|optional>
# Returns 0 when running under the authenticated declared Nix environment.
# Returns 1 when NOT authenticated and no capability was presented (optional
#   callers use this to report a tool-unavailable skip; full callers re-exec
#   through the authenticated wrapper).
# Exits 2 on any invalid/forged capability or a claimed-but-incomplete Nix store
#   environment — a forged marker can never silently skip.
nix_gate_require() {
    local mode="${1:-optional}"
    case "$mode" in
        full|optional) ;;
        *) echo "nix-gate: invalid mode '$mode'" >&2; return 2 ;;
    esac
    local cap_present=no
    if [[ -n "${CBX_NIX_GATE_FILE:-}" || -n "${CBX_NIX_GATE_NONCE:-}" ]]; then
        cap_present=yes
    fi

    if [[ "$cap_present" == yes ]]; then
        # The capability is the authenticated wrapper boundary; validate it
        # strictly and refuse to proceed if incomplete or invalid.
        if [[ -z "${CBX_NIX_GATE_FILE:-}" || -z "${CBX_NIX_GATE_NONCE:-}" ]]; then
            echo "nix-gate: incomplete boundary capability (file or nonce missing)" >&2
            exit 2
        fi
        if ! python3 "$NIX_GATE_PY" "$CBX_NIX_GATE_FILE" "$CBX_NIX_GATE_NONCE"; then
            echo "nix-gate: invalid boundary capability (forge/expiry)" >&2
            exit 2
        fi
        if [[ -z "${IN_NIX_SHELL:-}" ]]; then
            echo "nix-gate: capability presented outside nix-shell" >&2
            exit 2
        fi
    elif [[ -z "${IN_NIX_SHELL:-}" ]]; then
        # No capability and not inside a nix-shell: not authenticated. Optional
        # callers report a skip; full callers re-exec through the wrapper.
        return 1
    fi

    # Inside nix-shell (IN_NIX_SHELL set): the declared tools must resolve under
    # the immutable Nix store. A claimed-but-incomplete environment fails closed
    # rather than silently skipping.
    local t tp rp
    for t in $NIX_GATE_TOOLS; do
        tp=$(command -v "$t" 2>/dev/null || true)
        if [[ -z "$tp" ]]; then
            echo "nix-gate: declared tool '$t' is missing from the Nix environment" >&2
            exit 2
        fi
        rp=$(readlink -f "$tp" 2>/dev/null || true)
        if [[ "$rp" != /nix/store/* ]]; then
            echo "nix-gate: declared tool '$t' is not Nix-built ($rp)" >&2
            exit 2
        fi
    done
    return 0
}
