#!/usr/bin/env bash
# Enforce planning or implementation completion before Ralph may terminate.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
MODE=${1:-}
cd -- "$PROJECT_ROOT"

case "$MODE" in
    --planning)
        ./scripts/plan-scope-guard.sh
        ./scripts/check-plan-freshness.sh
        python3 - <<'PY'
import re
text = open('IMPLEMENTATION_PLAN.md', encoding='utf-8').read()
statuses = re.findall(r'^- Status:\s*(pending|in_progress|complete|blocked)\s*$', text, re.M)
if not statuses:
    raise SystemExit('final-gate: plan has no tasks using required `- Status:` format')
if 'Final documentation and specification audit' not in text:
    raise SystemExit('final-gate: mandatory final documentation task is missing')
if not re.search(r'^status:\s*active\s*$', text, re.M):
    raise SystemExit('final-gate: new plan front matter must have status: active')
PY
        echo "final-gate: planning completion accepted"
        ;;
    --implementation)
        ./scripts/check-plan-freshness.sh
        python3 - <<'PY'
import re
text = open('IMPLEMENTATION_PLAN.md', encoding='utf-8').read()
statuses = re.findall(r'^- Status:\s*(pending|in_progress|complete|blocked)\s*$', text, re.M)
if not statuses:
    raise SystemExit('final-gate: plan has no machine-checkable task statuses')
unfinished = [status for status in statuses if status != 'complete']
if unfinished:
    raise SystemExit(f'final-gate: {len(unfinished)} plan task(s) are not complete')
pattern = r'^## Task[^\n]*Final documentation and specification audit\s*$.*?^- Status:\s*complete\s*$'
if not re.search(pattern, text, re.M | re.S):
    raise SystemExit('final-gate: final documentation task is missing or incomplete')
if not re.search(r'^status:\s*complete\s*$', text, re.M):
    raise SystemExit('final-gate: plan front matter must have status: complete')
PY
        ./scripts/check-docs-sync.sh
        ./scripts/verify-boilerplate.sh
        if [[ -x scripts/verify-project.sh ]]; then
            ./scripts/verify-project.sh
        fi
        echo "final-gate: implementation, specification, tests, and documentation accepted"
        ;;
    *)
        echo "Usage: scripts/final-gate.sh --planning|--implementation" >&2
        exit 2
        ;;
esac
