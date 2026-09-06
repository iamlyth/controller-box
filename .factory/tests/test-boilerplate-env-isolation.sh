#!/usr/bin/env bash
# Adversarial regression (BUG-0013): isolated factory scenarios must not
# inherit the final-gate attestation flag. Fresh campaign authority is stored
# in `.factory-state/factory-loop.json`, not selected through lifecycle
# environment variables.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
cd -- "$PROJECT_ROOT"

# The suite runner must neutralize ambient lifecycle state before any test.
grep -q '^unset FACTORY_FINAL_GATE_ATTEST' scripts/verify-boilerplate.sh || {
    echo "test: verify-boilerplate does not sanitize the ambient attestation flag" >&2
    exit 1
}
# Recreate the exact completion-gate leak: the attestation flag set in the
# environment of the plan-cycle scenario chain. The chain must pass.
FACTORY_FINAL_GATE_ATTEST=1 ./.factory/tests/test-plan-cycle.sh || {
    echo "test: plan-cycle chain failed under ambient attestation state" >&2
    exit 1
}

echo "test: boilerplate environment isolation checks passed"
