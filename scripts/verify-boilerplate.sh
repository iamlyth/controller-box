#!/usr/bin/env bash
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
cd -- "$PROJECT_ROOT"

# Scenario suites use isolated repositories and must not inherit ambient
# attestation or campaign metadata. Fresh control state is file-backed rather
# than selected through environment variables.
unset FACTORY_FINAL_GATE_ATTEST FACTORY_CAMPAIGN_PHASE FACTORY_CAMPAIGN_ROUND \
      FACTORY_CAMPAIGN_AUDIT_ROUND FACTORY_CAMPAIGN_AUDIT_BASE \
      FACTORY_CAMPAIGN_RUNNER_EVIDENCE_SHA256 FACTORY_CAMPAIGN_OBJECTIVE

# Git status omits ignored plaintext. Scan the actual workspace namespace.
python3 scripts/check-workspace-credentials.py --root "$PROJECT_ROOT"

mapfile -t SHELL_FILES < <(find scripts tests .factory/tests -type f -name '*.sh' -print | sort)
for file in "${SHELL_FILES[@]}"; do
    bash -n "$file"
done
if command -v shellcheck >/dev/null; then
    shellcheck "${SHELL_FILES[@]}"
else
    echo "verify: warning: shellcheck unavailable" >&2
fi

python3 - <<'PY'
import json, pathlib, tomllib
with open('.factory/config.toml', 'rb') as stream:
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
assert config['issues'] == {
    'schema': 'ralph-bug-ledger/v1',
    'open_ledger': '.factory/bugs/open.md',
    'closed_ledger': '.factory/bugs/closed.md',
    'maintenance_plan': '.factory/artifacts/maintenance-plan.md',
    'final_task_title': 'Maintenance verification and documentation audit',
    'providers': ['github', 'forgejo'],
    'external_sync': 'manual',
    'credentials': False,
}
manifest = json.load(open('.factory/verifier-acceptance.json', encoding='utf-8'))
assert manifest.get('schema') == 'ralph-verifier-acceptance/v1'
gates = manifest.get('gates')
assert isinstance(gates, list) and gates
for gate in gates:
    assert isinstance(gate, dict) and set(gate) == {'name', 'args'}
    assert isinstance(gate['name'], str) and gate['name'] and '/' not in gate['name']
    assert isinstance(gate['args'], list) and all(isinstance(a, str) and a for a in gate['args'])
    if gate['name'] not in ('ctest', 'test_installed_functional'):
        assert (pathlib.Path('tests') / gate['name']).is_file(), f'missing gate {gate["name"]}'
required = [
    'AGENTS.md', '.factory/config.toml', '.factory/environment.toml',
    '.factory/artifacts/implementation-plan.md', '.factory/artifacts/maintenance-plan.md',
    '.factory/artifacts/campaign-audit.md', '.factory/bugs/open.md', '.factory/bugs/closed.md',
    '.factory/prompts/planner.md', '.factory/prompts/developer.md',
    '.factory/prompts/tester.md', '.factory/prompts/auditor.md',
    'scripts/bug-ledger.py', 'scripts/validate-maintenance-plan.py',
    'scripts/validate-implementation-plan.py',
    'scripts/factory-lock.sh', 'scripts/factory-lock-exec.py',
    'scripts/factory_lock.py', 'scripts/factory_state_io.py',
    'scripts/factory-state-file.py',
    'scripts/campaign-verifier-binding.py',
    'scripts/git-commit-guard.sh', 'scripts/install-git-commit-guard.sh',
    'scripts/pi-cli-shims/git', 'tests/test-git-commit-guard.sh',
    'scripts/check-installed-functional-evidence.sh',
    'scripts/check-installed-harness-evidence.sh',
    'scripts/check-maintenance-freshness.sh',
    'docs/BUG_WORKFLOW.md', 'tests/test-bug-workflow.sh',
    'tests/test-plan-cycle.sh',
    'tests/test-installed-functional-evidence.sh',
    '.factory/environment.toml', '.factory/artifacts/campaign-audit.md',
    'scripts/check-factory-environment.py',
    'scripts/initialize-campaign-audit.py',
    'scripts/validate-campaign-audit.py', 'scripts/campaign-audit-scope-guard.sh',
    '.factory/verifier-acceptance.json',
    'scripts/run-factory-runners.py', 'scripts/check-factory-runner-evidence.py',
    'scripts/factory-runner-server.py', 'scripts/factory-runner-broker.py',
    'scripts/factory_runner_authority.py', 'scripts/factory_runner_artifacts.py',
    'scripts/build-runner-probe-authority.py', 'scripts/install-factory-runner-v2.sh',
    'scripts/archive-factory-campaign.py', 'scripts/check-workspace-credentials.py', 'scripts/validate-runner-artifacts-semantic.py',
    'scripts/inputplumber-dbus-audit.py',
    'deploy/factory-runner-authority-v1/authority.json',
    'deploy/factory-runner-authority-v1/forced-command-v2.txt',
    'scripts/pi2-secure-exec.py',
    'tests/test-factory-environment.sh', 'tests/test-factory-runner.sh',
    'tests/test-runner-authority.py', 'tests/test-runner-install-bootstrap.py',
    'tests/test-runner-installer-security.py', 'tests/test-campaign-archive.py', 'tests/test-workspace-credentials.py', 'tests/test-iprunner-probes.sh',
    'tests/test-broker-security.py', 'tests/test-dbus-audit.py', 'tests/test-campaign-residuals.py',
    'tests/test-gpu-compositor-probe.sh',
    'tests/test-campaign-audit.sh',
    'tests/test-factory-lock.py',
    'tests/test-orchestration-security.py',
    'tests/test-boilerplate-env-isolation.sh',
    'tests/test-production-path-bypass.sh',
    'tests/test-visual-audit-sdk-authority.sh',
    '.factory/schemas/conformance.schema.json', '.factory/capability-contracts.json',
    'scripts/validate-conformance.py', 'scripts/check-capability-contracts.py',
    'scripts/check-capability-evidence.py', 'scripts/machine-receipt.py',
    'scripts/check-audit-receipts.py',
    'scripts/validate-blocked-facts.py', 'scripts/check-campaign-objectives.py',
    'scripts/check-golden-policy.py',
    '.factory/visual-audit.toml', '.factory/visual-audit-inventory.json',
    '.factory/visual-audit-calibration.json',
    '.factory/schemas/visual-audit-review.schema.json',
    '.factory/prompts/visual-audit.md',
    'scripts/visual-audit-provenance.py', 'scripts/visual-audit-lease.py',
    'scripts/visual-audit-review.py', 'scripts/visual-audit-review-sdk.mjs',
    'scripts/visual-audit-capture.sh', 'scripts/visual-audit-probe.sh',
    'scripts/visual-capture-driver.sh', 'scripts/check-visual-audit.py',
    'scripts/visual-audit-gate.sh', 'tests/test-visual-audit.sh',
    'scripts/nix-gate.sh', 'scripts/nix-gate-exec.sh',
    'scripts/nix-gate-check.py', 'tests/test-nix-gate.sh',
    'scripts/credential-guard.py', 'tests/test-credential-guard.sh',
    'scripts/pi-factory-guard-extension.mjs',
    'tests/test-credential-extension.sh',
    '.factory/artifacts/blocked-facts.json', '.factory/artifacts/conformance.json',
    '.factory/campaign-objectives.json', '.factory/golden-policy.json',
    '.factory/golden-review.json', '.factory/production-graphics-approval.json',
    '.factory/human-review-trust.json', '.factory/runner-policy-enrollment.json',
    '.factory/schemas/human-review-trust-anchor-v2.schema.json',
    '.factory/schemas/factory-runner-policy-v1.schema.json',
    '.factory/schemas/blocked-facts.schema.json',
    '.factory/schemas/golden-review.schema.json',
    '.factory/schemas/production-graphics-approval-v2.schema.json',
    'tests/test-conformance.sh', 'tests/test-capability-contracts.sh',
    'tests/test-core-acceptance.sh', 'tests/test-audit-receipts.sh',
    'tests/test-blocked-facts.sh', 'tests/test-campaign-objectives.sh',
    'tests/test-golden-policy.sh',
    'tests/test-runner-signer.sh', 'scripts/check-spec-provided.sh',
    'scripts/check-generic-leakage.sh', '.factory/generic-leak-allowlist',
    '.factory/signer-trust.json', '.factory/requirement-policy.json',
    '.factory/campaign-receipt-policy.json',
    '.factory/loop/migration.py', '.factory/ralph-freeze',
    '.factory/tests/test-factory-migration.py',
    '.factory/tests/test-factory-migration.sh',
    '.factory/tests/adversarial-manifest.json',
    '.factory/tests/test-factory-adversarial.py',
    '.factory/tests/test-factory-adversarial.sh',
    '.factory/loop/installer.py',
    '.factory/bin/factory-launch',
    '.factory/bin/factory-campaign',
    '.factory/tests/test-factory-installed.py',
    '.factory/tests/test-factory-installed.sh',
    '.factory/loop/generic_evidence.py',
    '.factory/bin/publish-generic-evidence',
    '.factory/tests/test-factory-generic-evidence.py',
    '.factory/tests/test-factory-generic-evidence.sh',
    '.factory/smoke/evidence_smoke_common.py',
    '.factory/smoke/evidence_smoke_driver.py',
    '.factory/smoke/evidence_smoke_gate.py',
    '.factory/smoke/evidence_smoke.py',
    '.factory/tests/test-factory-smoke.py',
    '.factory/tests/test-factory-smoke.sh',
    '.factory/tests/test-factory-confinement-order.sh',
    '.factory/tests/test-factory-supervision.sh',
    '.factory/loop/plan_parser.py', '.factory/loop/selector.py',
    '.factory/loop/state.py', '.factory/loop/readiness.py', '.factory/loop/evidence.py',
    '.factory/loop/gitutil.py', '.factory/loop/footprint.py',
    '.factory/loop/lock.py', '.factory/loop/launch.py',
    '.factory/loop/campaign.py', '.factory/loop/confine_launcher.py',
    '.factory/loop/pre_round.py', '.factory/pre-round-hooks.json',
    '.factory/tests/test-factory-pre-round.py', '.factory/loop/findings.py',
    '.factory/loop/audit_objectives.py', '.factory/loop/usage.py',
    '.factory/loop/usage_fetch.py', '.factory/loop/redaction.py',
    '.factory/loop/promptset.py', '.factory/loop/workspace_confinement.py',
    '.factory/loop/plan_parser.py',
    '.factory/schemas/factory-plan-v1.schema.json',
    '.factory/schemas/factory-plan-v1.requirements.json',
    '.factory/schemas/factory-plan-v1.schema.md',
    '.factory/schemas/factory-state-v2.schema.md',
    '.factory/schemas/factory-campaign-result-v1.schema.json',
    '.factory/schemas/factory-readiness-result-v2.schema.json',
    '.factory/schemas/factory-runner-receipt-v3.schema.json',
    '.factory/schemas/factory-runner-aggregate-v4.schema.json',
    '.factory/schemas/factory-phase-result-v1.schema.json',
    '.factory/schemas/factory-launch-result-v1.schema.json',
    '.factory/schemas/factory-confinement-v1.schema.json',
    '.factory/schemas/factory-findings-v1.schema.json',
    '.factory/schemas/factory-findings-receipt-v1.schema.json',
    '.factory/schemas/audit-objectives-v1.schema.json',
    '.factory/schemas/ollama-usage-v1.schema.json',
    '.factory/audit-objectives/registry.json',
    'docs/FACTORY-LOOP-SPEC.md',
]
for name in required:
    assert pathlib.Path(name).is_file(), f'missing {name}'
# The fresh Python-factory control plane is stdlib-only: a Ralph runtime
# import in the hidden loop fails the generic suite.
migration_text = pathlib.Path('.factory/loop/migration.py').read_text(encoding='utf-8')
for import_token in ('from ralph', 'import ralph', 'ralph.emit',
                     'ralph.plan', 'ralph.audit', 'ralph.memory',
                     'ralph_event', 'ralph_emit'):
    assert import_token not in migration_text, \
        f'migration.py must not import the Ralph runtime: {import_token}'
# The deprecated context-summary authority is removed from the tracked tree
# and unwired from every new-path control step, so the stale mirror can
# never compete with the canonical plan as a task authority.
assert not pathlib.Path('.factory/artifacts/context-summary.md').exists(), \
    'the stale context-summary mirror must not be tracked'
for script in ('scripts/final-gate.sh',):
    text = pathlib.Path(script).read_text(encoding='utf-8')
    for token in ('check-context-summary', 'ralph-context-summary'):
        assert token not in text, f'{script} still wires the deprecated {token} authority'
forbidden_root_factory_files = {
    'PROMPT.md', 'IMPLEMENTATION_PLAN.md', 'MAINTENANCE_PLAN.md',
    'CAMPAIGN_AUDIT.md', 'factory.toml', 'factory-environment.toml',
    'open-bugs.md', 'closed-bugs.md', 'ralph.yml', 'ralph.plan.yml',
    'ralph.audit.yml', 'ralph.maintenance.yml', 'ralph.maintenance-plan.yml',
}
root_files = {path.name for path in pathlib.Path('.').iterdir() if path.is_file()}
assert not (root_files & forbidden_root_factory_files), 'factory files leaked back into repository root'
json.load(open('.pi/subagents.json', encoding='utf-8'))
for path in pathlib.Path('.pi/agents').glob('*.md'):
    text = path.read_text(encoding='utf-8')
    header = text.split('---', 2)[1]
    tools = next(line for line in header.splitlines() if line.startswith('tools:'))
    for forbidden in ('edit', 'write', 'bash'):
        assert forbidden not in tools, f'{path}: read-only agent exposes {forbidden}'
PY

grep -q '^## Build' AGENTS.md
grep -q '^## Immediate validation' AGENTS.md
(( $(wc -l < AGENTS.md) <= 100 )) || { echo 'verify: AGENTS.md must remain concise (100 lines maximum)' >&2; exit 1; }
# Fresh role prompts: the planner owns the sole task ledger, the developer
# never leaves placeholders and treats the conformance sidecar/receipts as
# the acceptance authority, the tester never substitutes proxy evidence for
# the real production path, and the auditor treats blocked/partial rows as
# failing unless re-classified with evidence.
grep -q 'The plan is the sole task ledger' .factory/prompts/planner.md
grep -q 'status: active' .factory/prompts/planner.md
grep -q 'leave placeholders, stubs, weakened assertions' .factory/prompts/developer.md
grep -q 'machine-readable conformance sidecar and exact-commit receipts remain the' .factory/prompts/developer.md
grep -q 'not the real system service' .factory/prompts/tester.md
grep -q 'synthetic producer is not the target' .factory/prompts/tester.md
grep -q 'a declaration is not evidence' .factory/prompts/tester.md
grep -q 'blocked. and .partial. rows fail' .factory/prompts/auditor.md
grep -q 'out-of-band' .factory/prompts/auditor.md
grep -q 'exact-commit receipt or manifest' .factory/prompts/auditor.md
for role in visual-reviewer runner-reviewer evidence-reviewer spec-reviewer; do
    grep -q 'no runtime-certification authority' ".pi/agents/$role.md"
done
# The fresh control plane never invokes the deprecated ralph shim or the
# retired emit extension, and the retained production gates never invoke
# them either.  The generic model-side Pi guard extension
# (scripts/pi-factory-guard-extension.mjs) is the required replacement and
# is bound by the launch authority and the credential-extension suite.
# verify-boilerplate.sh is the checker itself: its only mentions of these
# tokens are the check literals below, so it is not scanned for them.
for script in scripts/final-gate.sh \
        scripts/campaign-verifier-binding.py \
        scripts/check-installed-harness-evidence.sh \
        scripts/visual-audit-provenance.py; do
    text=$(cat "$script")
    for token in 'pi-cli-shims/ralph' 'pi-ralph-emit-extension' 'ralph emit' 'ralph_emit'; do
        [[ $text != *"$token"* ]] || {
            echo "verify: $script must not depend on '$token'" >&2
            exit 1
        }
    done
    [[ $text == *'pi-factory-guard-extension'* ]] && {
        echo "verify: $script must not depend on the model-side guard extension" >&2
        exit 1
    }
done
# The fresh launch authority invokes the committed secure wrapper directly
# and always loads the committed model-side Pi guard extension.
grep -q 'pi2-secure-exec.py' .factory/loop/launch.py
grep -q 'pi-factory-guard-extension.mjs' .factory/loop/launch.py
# The migration tombstone is a safe non-executable regular file, and the
# migration verifier enforces complete tracked absence of retired launchers.
[[ -f .factory/ralph-freeze && ! -L .factory/ralph-freeze && ! -x .factory/ralph-freeze ]] || {
    echo "verify: .factory/ralph-freeze must be a non-executable regular tracked file" >&2
    exit 1
}
python3 .factory/loop/migration.py --root "$PROJECT_ROOT" verify >/dev/null
# The designated smoke seam, gate, and operator command are tracked
# executables (100755): they execute only from their bound committed
# descriptors through the pinned interpreter, never a PATH-resolved name.
for name in .factory/smoke/evidence_smoke_driver.py \
        .factory/smoke/evidence_smoke_gate.py .factory/smoke/evidence_smoke.py; do
    entry=$(git ls-files -s -- "$name")
    [[ -n "$entry" ]] || { echo "verify: $name is not tracked" >&2; exit 1; }
    [[ ${entry%% *} == 100755 ]] || {
        echo "verify: $name must be tracked executable 100755" >&2
        exit 1
    }
done
# Machine visual-audit production invariants. Controller stays disabled until
# its genuine authenticated Pi2 probe and independently accepted calibration
# controls pass, but its configured and SDK-enforced identity is already fixed:
# callers cannot select a production reviewer model. Mutable artifacts remain
# under ignored factory paths.
grep -q '^enabled = false' .factory/visual-audit.toml
grep -q '^vision_model = "ollama/kimi-k2.6"$' .factory/visual-audit.toml
grep -q '^const PRODUCTION_VISION_MODEL = "ollama/kimi-k2.6";$' scripts/visual-audit-review-sdk.mjs
grep -q 'caller model override refused' scripts/visual-audit-review-sdk.mjs
grep -q '^lease_file = ".factory-state/visual-audit' .factory/visual-audit.toml
grep -q '^capture_dir = ".factory/artifacts/visual-audit/' .factory/visual-audit.toml
grep -q '^review_dir = ".factory/artifacts/visual-audit/' .factory/visual-audit.toml
git check-ignore -q .factory-state/visual-audit/captures/good-main.png
git check-ignore -q .factory-state/visual-audit/reviews/report.json
git check-ignore -q .factory-state/visual-audit/lease
# The visual-audit completion gate is a check-only mechanical step: the
# implementation final gate invokes it, and the gate itself only runs the
# aggregate checker against existing review evidence -- it never invokes the
# capture/probe paths or the review SDK driver / vision model.
grep -q 'scripts/visual-audit-gate.sh' scripts/final-gate.sh
grep -q 'check-visual-audit.py' scripts/visual-audit-gate.sh
if grep -Eq 'visual-audit-(capture|probe)|review-sdk' scripts/visual-audit-gate.sh; then
    echo "verify: visual-audit-gate.sh must never invoke capture/review-sdk/probe" >&2
    exit 1
fi
./scripts/check-factory-environment.py
./scripts/check-capability-contracts.py
./scripts/check-spec-provided.sh
./scripts/validate-blocked-facts.py planning .factory/artifacts/blocked-facts.json
./scripts/check-golden-policy.py
cmp -s .github/ISSUE_TEMPLATE/bug_report.md .forgejo/ISSUE_TEMPLATE/bug_report.md
./scripts/bug-ledger.py validate
./scripts/check-generic-leakage.sh
./scripts/check-docs-sync.sh

if git ls-files | grep -E '(^|/)(\.ollama-usage-env|\.env)$' >/dev/null; then
    echo "verify: secret environment file is tracked" >&2
    exit 1
fi
python3 - <<'PY'
import subprocess, urllib.parse
for name in subprocess.check_output(['git', 'remote'], text=True).split():
    url = subprocess.check_output(['git', 'remote', 'get-url', name], text=True).strip()
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme in {'http', 'https'} and (parsed.username or parsed.password):
        raise SystemExit(f'verify: remote {name} embeds credentials; use SSH or a credential helper')
PY

# -- hidden fresh factory shell suites (deterministic serial order) ---------
# The hidden suites are the harness's own acceptance path (HIDE-01 §3): they
# run in isolated fixture repositories and never invoke the live publisher or
# the runner server/client.  The installed suite is the exact-commit
# installed-tier evidence driver; the generic-evidence suite proves the
# two-stage publisher against fixture authorities; the migration suite proves
# the legacy freeze surface; the adversarial suite proves the retained
# receipt/evidence channels; the smoke suite drives one full campaign round.
./.factory/tests/test-factory-footprint.sh
./.factory/tests/test-factory-installed.sh
./.factory/tests/test-factory-generic-evidence.sh
./.factory/tests/test-factory-migration.sh
./.factory/tests/test-factory-adversarial.sh
./.factory/tests/test-factory-smoke.sh
./.factory/tests/test-factory-confinement-order.sh

# -- hidden fresh factory Python suites (deterministic serial order) ---------
# Every hidden Python suite that is not already driven by a shell wrapper
# above runs here directly (unittest entrypoints, warning-free under
# ResourceWarning like the shell drivers), so a regression in any hidden
# authority can never hide from the boilerplate gate.  Suites already covered
# by their shell wrappers (footprint, installed, generic-evidence, migration,
# adversarial, smoke) are intentionally not duplicated here.
for hidden_suite in \
        test-factory-campaign \
        test-factory-confinement \
        test-factory-conformance \
        test-factory-evidence \
        test-factory-findings \
        test-factory-launch \
        test-factory-lock \
        test-factory-plan-parser \
        test-factory-pre-round \
        test-factory-redaction \
        test-factory-readiness \
        test-factory-selector \
        test-factory-state \
        test-factory-usage; do
    echo "verify: running hidden suite $hidden_suite"
    python3 -W error::ResourceWarning ".factory/tests/$hidden_suite.py"
done

# -- retained visible (non-Ralph) suites -------------------------------------
./tests/test-git-commit-guard.sh
./tests/test-installed-functional-evidence.sh "$PROJECT_ROOT"
./tests/test-factory-environment.sh
./tests/test-factory-runner.sh
python3 ./tests/test-runner-install-bootstrap.py
python3 ./tests/test-runner-installer-security.py
python3 ./tests/test-broker-security.py
python3 ./tests/test-dbus-audit.py
python3 ./tests/test-campaign-residuals.py
python3 ./tests/test-campaign-archive.py
python3 ./tests/test-workspace-credentials.py
./tests/test-iprunner-probes.sh
nix-shell --run 'bash ./tests/test-gpu-compositor-probe.sh'
./tests/test-campaign-audit.sh
./tests/test-factory-lock.py
./tests/test-boilerplate-env-isolation.sh
./tests/test-production-path-bypass.sh
./tests/test-conformance.sh
./tests/test-capability-contracts.sh
./tests/test-core-acceptance.sh
./tests/test-audit-receipts.sh
./tests/test-blocked-facts.sh
./tests/test-campaign-objectives.sh
./tests/test-golden-policy.sh
./tests/test-runner-signer.sh
./tests/test-boilerplate.sh
./tests/test-visual-audit.sh
./tests/test-visual-audit-sdk-authority.sh
./tests/test-credential-guard.sh
./tests/test-credential-extension.sh
echo "verify: boilerplate checks passed"
