#!/usr/bin/env bash
# Compatibility wrapper for the hidden installed-evidence checker.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
exec "$ROOT/.factory/tools/check-installed-functional-evidence.sh" "$@"
