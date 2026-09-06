#!/usr/bin/env bash
# Deprecated compatibility wrapper; harness implementation is hidden.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
exec "$ROOT/.factory/tools/verify-boilerplate.sh" "$@"
