#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
GUARD="$PROJECT_ROOT/scripts/ollama-usage-guard.sh"

json=$($GUARD --check --json --html-file "$SCRIPT_DIR/fixtures/usage-ok.html")
python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["blocked"] is False; assert d["session_percent"] == 12.5' <<< "$json"

set +e
$GUARD --check --html-file "$SCRIPT_DIR/fixtures/usage-blocked.html" >/dev/null
blocked_rc=$?
OLLAMA_WAIT_MAX_POLLS=1 OLLAMA_WAIT_INTERVAL_SECONDS=0 \
    $GUARD --wait --html-file "$SCRIPT_DIR/fixtures/usage-blocked.html" >/dev/null 2>&1
wait_rc=$?
$GUARD --check --html-file "$SCRIPT_DIR/fixtures/login.html" >/dev/null 2>&1
login_rc=$?
set -e
[[ $blocked_rc -eq 1 ]] || { echo "test: blocked usage returned $blocked_rc" >&2; exit 1; }
[[ $wait_rc -eq 1 ]] || { echo "test: bounded wait returned $wait_rc" >&2; exit 1; }
[[ $login_rc -eq 2 ]] || { echo "test: login page returned $login_rc" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/scripts" "$tmp/docs" "$tmp/.factory/artifacts" "$tmp/.factory/loop"
cp "$PROJECT_ROOT/scripts/check-plan-freshness.sh" "$tmp/scripts/"
cp "$PROJECT_ROOT/.factory/loop/gitutil.py" "$tmp/.factory/loop/"
for policy in campaign-receipt-policy.json requirement-policy.json capability-contracts.json; do
    cp "$PROJECT_ROOT/.factory/$policy" "$tmp/.factory/$policy"
done
printf '# Trial specification\n' > "$tmp/docs/SPEC.md"
cat > "$tmp/.factory/config.toml" <<'EOF'
[project]
spec = "docs/SPEC.md"
EOF
git -C "$tmp" init -q
git -C "$tmp" config user.name test
git -C "$tmp" config user.email test@example.invalid
git -C "$tmp" add docs/SPEC.md
git -C "$tmp" commit -qm spec
spec_commit=$(git -C "$tmp" rev-parse HEAD)
spec_blob=$(git -C "$tmp" rev-parse HEAD:docs/SPEC.md)
cat > "$tmp/.factory/artifacts/implementation-plan.md" <<EOF
---
spec_path: docs/SPEC.md
spec_commit: $spec_commit
spec_blob: $spec_blob
base_commit: $spec_commit
status: active
---
# Plan
EOF
git -C "$tmp" add .factory/artifacts/implementation-plan.md .factory/config.toml \
    .factory/loop/gitutil.py .factory/campaign-receipt-policy.json \
    .factory/requirement-policy.json .factory/capability-contracts.json \
    scripts/check-plan-freshness.sh
git -C "$tmp" commit -qm plan
"$tmp/scripts/check-plan-freshness.sh" >/dev/null
printf '\nchanged\n' >> "$tmp/docs/SPEC.md"
set +e
"$tmp/scripts/check-plan-freshness.sh" >/dev/null 2>&1
stale_rc=$?
set -e
[[ $stale_rc -eq 1 ]] || { echo "test: dirty specification was not rejected" >&2; exit 1; }

FACTORY_ALLOW_TRIAL_BRANCH=1 "$PROJECT_ROOT/scripts/branch-guard.sh" >/dev/null

"$PROJECT_ROOT/scripts/bug-ledger.py" validate >/dev/null
cmp -s "$PROJECT_ROOT/.github/ISSUE_TEMPLATE/bug_report.md" "$PROJECT_ROOT/.forgejo/ISSUE_TEMPLATE/bug_report.md"
python3 - "$PROJECT_ROOT" <<'PY'
import pathlib, sys, tomllib
root = pathlib.Path(sys.argv[1])
with (root / '.factory/config.toml').open('rb') as stream:
    config = tomllib.load(stream)
assert config['concurrency']['mutating_workers'] == 1
assert config['concurrency']['integration_workers'] == 1
assert config['git']['allow_worktrees'] is False
assert isinstance(config.get('campaign', {}).get('required_capabilities'), list)
assert all(isinstance(item, str) and item for item in config['campaign']['required_capabilities'])
assert config['verification']['maintenance_command'] == ['./scripts/verify-project.sh']
assert isinstance(config['verification']['campaign_command'], list)
assert config['verification']['campaign_command']
assert all(isinstance(arg, str) and arg for arg in config['verification']['campaign_command'])
assert config['issues']['providers'] == ['github', 'forgejo']
assert config['issues']['external_sync'] == 'manual'
assert config['issues']['credentials'] is False
for path in ('AGENTS.md', '.factory/bugs/open.md', '.factory/bugs/closed.md',
             '.factory/artifacts/maintenance-plan.md', '.factory/artifacts/campaign-audit.md',
             'scripts/bug-ledger.py', 'scripts/validate-maintenance-plan.py',
             'scripts/validate-implementation-plan.py', 'scripts/check-maintenance-freshness.sh',
             'scripts/factory-lock.sh', 'scripts/factory-lock-exec.py',
             'scripts/factory_lock.py', 'scripts/factory_state_io.py',
             'scripts/factory-state-file.py', 'scripts/campaign-verifier-binding.py',
             'scripts/git-commit-guard.sh', 'scripts/install-git-commit-guard.sh',
             'scripts/pi-cli-shims/git', 'tests/test-git-commit-guard.sh',
             'scripts/check-installed-functional-evidence.sh',
             'scripts/check-installed-harness-evidence.sh',
             'docs/BUG_WORKFLOW.md', 'tests/test-bug-workflow.sh',
             'tests/test-plan-cycle.sh', '.factory/environment.toml',
             'scripts/check-factory-environment.py', 'scripts/initialize-campaign-audit.py',
             'scripts/validate-campaign-audit.py', 'scripts/campaign-audit-scope-guard.sh',
             '.factory/verifier-acceptance.json', 'tests/test-factory-environment.sh',
             'tests/test-campaign-audit.sh', 'tests/test-factory-lock.py',
             'tests/test-orchestration-security.py', 'scripts/pi2-secure-exec.py',
             'tests/test-pi2-ollama-wrapper.sh', 'tests/test-visual-audit-sdk-authority.sh'):
    assert (root / path).is_file(), f'missing maintenance artifact: {path}'
PY
"$PROJECT_ROOT/tests/test-bug-workflow.sh"
"$PROJECT_ROOT/tests/test-plan-cycle.sh"
"$PROJECT_ROOT/tests/test-factory-environment.sh"
"$PROJECT_ROOT/tests/test-campaign-audit.sh"
"$PROJECT_ROOT/tests/test-factory-lock.py"
"$PROJECT_ROOT/tests/test-orchestration-security.py"

echo "test: boilerplate integration checks passed"
