#!/usr/bin/env bash
# Compatibility wrapper for the hidden factory acceptance gate.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
exec "$ROOT/.factory/tools/final-gate.sh" "$@"
