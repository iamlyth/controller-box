#!/usr/bin/env bash
# Compatibility wrapper for the hidden factory acceptance gate.
set -euo pipefail
# The campaign executes this gate through a retained descriptor
# (/proc/self/fd/N) so a pathname swap cannot substitute another file. The
# procfs descriptor path is not a real directory, so resolve it to the
# canonical script path before deriving ROOT.
SCRIPT_SOURCE=${BASH_SOURCE[0]}
if [[ "$SCRIPT_SOURCE" == /proc/self/fd/* ]]; then
    SCRIPT_SOURCE=$(readlink -f -- "$SCRIPT_SOURCE") || {
        echo "final-gate: cannot resolve retained descriptor script path" >&2
        exit 2
    }
fi
ROOT=$(cd -- "$(dirname -- "$SCRIPT_SOURCE")/.." && pwd)
exec "$ROOT/.factory/tools/final-gate.sh" "$@"
