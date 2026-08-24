#!/usr/bin/env bash
set -euo pipefail

# Retained-descriptor execution names `/proc/self/fd/N`, so the trusted
# verifier pins the canonical root. Direct invocation may derive it from the
# script path; either route must contain the committed gate and config.
if [[ -n "${FACTORY_VERIFIER_ROOT:-}" ]]; then
    PROJECT_ROOT=${FACTORY_VERIFIER_ROOT%/}
    SCRIPT_DIR=$PROJECT_ROOT/scripts
else
    SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
    PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
fi
for required_marker in scripts/check-docs-sync.sh .factory/config.toml; do
    [[ -e "$PROJECT_ROOT/$required_marker" ]] || {
        echo "docs-sync: cannot resolve the canonical repository root from FACTORY_VERIFIER_ROOT/BASH_SOURCE (missing $PROJECT_ROOT/$required_marker)" >&2
        exit 1
    }
done
cd -- "$PROJECT_ROOT"

BASE=$(python3 - <<'PY'
lines = open('.factory/artifacts/implementation-plan.md', encoding='utf-8').read().splitlines()
for line in lines:
    if line.startswith('base_commit:'):
        print(line.split(':', 1)[1].strip().strip('"\''))
        break
PY
)
[[ -n "$BASE" && "$BASE" != UNPLANNED ]] || { echo "docs-sync: plan has no valid base_commit" >&2; exit 1; }
git cat-file -e "$BASE^{commit}" 2>/dev/null || { echo "docs-sync: unknown base commit '$BASE'" >&2; exit 1; }

mapfile -t CHANGED < <({
    git diff --name-only "$BASE"
    git ls-files --others --exclude-standard
} | sort -u)
product_changed=false
docs_changed=false
for path in "${CHANGED[@]}"; do
    case "$path" in
        README.md|docs/*) docs_changed=true ;;
        .factory/artifacts/implementation-plan.md|.pi/*) ;;
        *) product_changed=true ;;
    esac
done

if $product_changed && ! $docs_changed; then
    echo "docs-sync: implementation changed since planning but README/docs did not" >&2
    exit 1
fi

echo "docs-sync: documentation change gate passed"
