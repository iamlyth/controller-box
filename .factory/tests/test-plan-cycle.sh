#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/scripts" "$tmp/docs" "$tmp/.factory-state" \
    "$tmp/.factory/artifacts" "$tmp/.factory/bugs" "$tmp/.factory/loop"
cp "$PROJECT_ROOT/.factory/tools/bug-ledger.py" \
   "$PROJECT_ROOT/.factory/tools/check-plan-freshness.sh" \
   "$PROJECT_ROOT/scripts/final-gate.sh" \
   "$PROJECT_ROOT/.factory/tools/validate-implementation-plan.py" \
   "$PROJECT_ROOT/.factory/tools/validate-maintenance-plan.py" "$tmp/scripts/"
cp "$PROJECT_ROOT/.factory/loop/gitutil.py" "$tmp/.factory/loop/"
for policy in campaign-receipt-policy.json requirement-policy.json capability-contracts.json; do
    cp "$PROJECT_ROOT/.factory/$policy" "$tmp/.factory/$policy"
done
chmod +x "$tmp/scripts/"*
cat > "$tmp/.factory/config.toml" <<'EOF'
[project]
spec = "docs/SPEC.md"
plan = ".factory/artifacts/implementation-plan.md"
[issues]
maintenance_plan = ".factory/artifacts/maintenance-plan.md"
EOF
printf '# Current specification\n' > "$tmp/docs/SPEC.md"
cat > "$tmp/.factory/bugs/open.md" <<'EOF'
# Open Bugs

Canonical queue of defects awaiting maintenance.

Schema: `ralph-bug-ledger/v1`

```json
[
  {
    "id": "BUG-0001",
    "title": "Fresh maintenance cycle",
    "status": "triaged",
    "severity": "medium",
    "reported": "2026-08-07",
    "external": [],
    "contract_change": false,
    "reproduction": "run it",
    "expected": "works",
    "actual": "fails",
    "acceptance": "passes",
    "resolution": "",
    "verification": "",
    "closed": null
  }
]
```
EOF
cat > "$tmp/.factory/bugs/closed.md" <<'EOF'
# Closed Bugs

Completed defects and their verification evidence.

Schema: `ralph-bug-ledger/v1`

```json
[]
```
EOF
printf '.factory-state/\n' > "$tmp/.gitignore"
printf 'OLD IMPLEMENTATION TASKS MUST DISAPPEAR\n' > "$tmp/.factory/artifacts/implementation-plan.md"
printf 'OLD MAINTENANCE TASKS MUST DISAPPEAR\n' > "$tmp/.factory/artifacts/maintenance-plan.md"
cd "$tmp"
git init -q
git config user.name test
git config user.email test@example.invalid
git add .
git commit -qm baseline
base=$(git rev-parse HEAD)

spec_commit=$(git log -1 --format=%H -- docs/SPEC.md)
spec_blob=$(git rev-parse HEAD:docs/SPEC.md)
cat > .factory/artifacts/implementation-plan.md <<EOF
---
spec_path: docs/SPEC.md
spec_commit: $spec_commit
spec_blob: $spec_blob
base_commit: $base
status: active
---
# Implementation Plan
## Specification conformance matrix
| ID | Spec § | Classification | Evidence | Task |
|---|---|---|---|---|
| REQ-1 | §1 | missing | no implementation | Task 1 |
## Interaction acceptance inventory
| Control | Controller path | Pointer path | Semantic outcome | Production dispatch |
|---|---|---|---|---|
| Example | A | click | state changes | SDL event loop |
## Task 1: Implement current gap
- Status: pending
- Dependencies: none
- Scope: bounded work
- Acceptance criteria: behavior works
- Verification: run tests
- Documentation impact: none
## Task 2: Final documentation and specification audit
- Status: pending
- Dependencies: Task 1
- Scope: canonical definition of done (§11.2), conformance, and interaction audit
- Acceptance criteria: conformance verified; interaction complete; open bugs resolved; independent review passes; clean tree
- Verification: run final checks
- Documentation impact: README
EOF
printf '%s\n' "$base" > .factory-state/planning-base-commit
git add .factory/artifacts/implementation-plan.md
git commit -qm 'valid planning checkpoint'
attested_head=$(git rev-parse HEAD)
FACTORY_FINAL_GATE_ATTEST=1 FACTORY_PLANNING_BASE_COMMIT=$base \
    ./scripts/final-gate.sh --planning >/dev/null
# Campaign verifier execution retains the script inode and invokes it through
# /proc/self/fd/N. Root discovery must canonicalize that descriptor path just
# like verify-project.sh rather than deriving /proc/self as the repository.
exec {final_gate_fd}<./scripts/final-gate.sh
FACTORY_PLANNING_BASE_COMMIT=$base \
    bash "/proc/self/fd/$final_gate_fd" --planning >/dev/null
exec {final_gate_fd}<&-
[[ $(git rev-parse HEAD) == "$attested_head" ]]
[[ -z $(git status --porcelain --untracked-files=normal) ]]
cp .factory/artifacts/implementation-plan.md "$tmp/valid-plan.md"
sed -i '0,/- Status: pending/s//- Status: complete/' .factory/artifacts/implementation-plan.md
set +e
FACTORY_PLANNING_BASE_COMMIT=$base ./scripts/final-gate.sh --planning >/dev/null 2>&1
non_pending_rc=$?
set -e
[[ $non_pending_rc -eq 1 ]]

cp "$tmp/valid-plan.md" .factory/artifacts/implementation-plan.md
sed -i 's/status: active/status: complete/; s/- Status: pending/- Status: complete/g; s/| missing |/| verified |/' .factory/artifacts/implementation-plan.md
./.factory/tools/validate-implementation-plan.py complete .factory/artifacts/implementation-plan.md >/dev/null
cp .factory/artifacts/implementation-plan.md "$tmp/valid-complete-plan.md"

# Adversarial structure checks: IDs, columns, fields, and dependencies are
# canonical rather than inferred from nearby prose.
cp "$tmp/valid-complete-plan.md" .factory/artifacts/implementation-plan.md
sed -i '/| REQ-1 |/a | REQ-1 | §2 | verified | duplicate ID | |' .factory/artifacts/implementation-plan.md
if ./.factory/tools/validate-implementation-plan.py complete .factory/artifacts/implementation-plan.md >/dev/null 2>&1; then
    echo 'test-plan-cycle: validator accepted a duplicate conformance ID' >&2; exit 1
fi
cp "$tmp/valid-complete-plan.md" .factory/artifacts/implementation-plan.md
sed -i 's/| REQ-1 | §1 | verified | no implementation | Task 1 |/| REQ-1 | §1 | verified | Task 1 |/' .factory/artifacts/implementation-plan.md
if ./.factory/tools/validate-implementation-plan.py complete .factory/artifacts/implementation-plan.md >/dev/null 2>&1; then
    echo 'test-plan-cycle: validator accepted a short conformance row' >&2; exit 1
fi
cp "$tmp/valid-complete-plan.md" .factory/artifacts/implementation-plan.md
sed -i 's/- Dependencies: Task 1/- Dependencies: Task 99/' .factory/artifacts/implementation-plan.md
if ./.factory/tools/validate-implementation-plan.py complete .factory/artifacts/implementation-plan.md >/dev/null 2>&1; then
    echo 'test-plan-cycle: validator accepted an unknown dependency' >&2; exit 1
fi
cp "$tmp/valid-complete-plan.md" .factory/artifacts/implementation-plan.md
sed -i 's/- Dependencies: Task 1/- Dependencies: Task 1 trailing prose/' .factory/artifacts/implementation-plan.md
if ./.factory/tools/validate-implementation-plan.py complete .factory/artifacts/implementation-plan.md >/dev/null 2>&1; then
    echo 'test-plan-cycle: validator accepted a malformed dependency expression' >&2; exit 1
fi
cp "$tmp/valid-complete-plan.md" .factory/artifacts/implementation-plan.md
sed -i 's/## Task 2:/## Task 3:/' .factory/artifacts/implementation-plan.md
if ./.factory/tools/validate-implementation-plan.py complete .factory/artifacts/implementation-plan.md >/dev/null 2>&1; then
    echo 'test-plan-cycle: validator accepted non-contiguous task IDs' >&2; exit 1
fi
cp "$tmp/valid-complete-plan.md" .factory/artifacts/implementation-plan.md
sed -i '/- Status: complete/a - Status: complete' .factory/artifacts/implementation-plan.md
if ./.factory/tools/validate-implementation-plan.py complete .factory/artifacts/implementation-plan.md >/dev/null 2>&1; then
    echo 'test-plan-cycle: validator accepted duplicate canonical task fields' >&2; exit 1
fi

for unfinished in pending blocked in_progress; do
    cp "$tmp/valid-complete-plan.md" .factory/artifacts/implementation-plan.md
    sed -i "0,/- Status: complete/s//- Status: $unfinished/" .factory/artifacts/implementation-plan.md
    if ./.factory/tools/validate-implementation-plan.py complete .factory/artifacts/implementation-plan.md >/dev/null 2>&1; then
        echo "test-plan-cycle: complete validator accepted task status $unfinished" >&2
        exit 1
    fi
done
cp "$tmp/valid-complete-plan.md" .factory/artifacts/implementation-plan.md
sed -i 's/status: complete/status: blocked/' .factory/artifacts/implementation-plan.md
if ./.factory/tools/validate-implementation-plan.py complete .factory/artifacts/implementation-plan.md >/dev/null 2>&1; then
    echo 'test-plan-cycle: complete validator accepted blocked front matter' >&2
    exit 1
fi
cp "$tmp/valid-complete-plan.md" .factory/artifacts/implementation-plan.md
sed -i 's/| verified |/| partial |/' .factory/artifacts/implementation-plan.md
if ./.factory/tools/validate-implementation-plan.py complete .factory/artifacts/implementation-plan.md >/dev/null 2>&1; then
    echo 'test-plan-cycle: complete validator accepted a nonverified row' >&2
    exit 1
fi
cp "$tmp/valid-complete-plan.md" .factory/artifacts/implementation-plan.md
sed -i 's/no implementation/hardware deferred and unavailable/' .factory/artifacts/implementation-plan.md
if ./.factory/tools/validate-implementation-plan.py complete .factory/artifacts/implementation-plan.md >/dev/null 2>&1; then
    echo 'test-plan-cycle: complete validator accepted a hidden hardware deferral' >&2
    exit 1
fi
cp "$tmp/valid-complete-plan.md" .factory/artifacts/implementation-plan.md
git add .factory/artifacts/implementation-plan.md
git commit -qm 'complete plan'
set +e
./scripts/final-gate.sh --implementation >/dev/null 2>&1
open_bug_rc=$?
set -e
[[ $open_bug_rc -eq 1 ]]

git restore -- .factory/artifacts/implementation-plan.md

fingerprint=$(./.factory/tools/bug-ledger.py fingerprint BUG-0001)
cat > .factory/artifacts/maintenance-plan.md <<EOF
---
bug_id: BUG-0001
bug_fingerprint: $fingerprint
spec_path: docs/SPEC.md
spec_commit: $spec_commit
spec_blob: $spec_blob
base_commit: $base
status: active
---
# Maintenance Plan
## Task 1: Maintenance verification and documentation audit
- Status: pending
- Dependencies: none
- Scope: verify
- Acceptance criteria: fixed
- Verification:
  run checks
- Documentation impact: none
EOF
./.factory/tools/validate-maintenance-plan.py planning .factory/artifacts/maintenance-plan.md >/dev/null
sed -i 's/- Status: pending/- Status: complete/' .factory/artifacts/maintenance-plan.md
set +e
./.factory/tools/validate-maintenance-plan.py planning .factory/artifacts/maintenance-plan.md >/dev/null 2>&1
maintenance_non_pending_rc=$?
set -e
[[ $maintenance_non_pending_rc -eq 1 ]]

# Maintenance dependencies are parsed as a strict graph. The final audit must
# depend on every prior task, and no task may depend on itself, a future task,
# an unknown task, or trailing prose.
cat > .factory/artifacts/maintenance-plan.md <<EOF
---
bug_id: BUG-0001
bug_fingerprint: $fingerprint
spec_path: docs/SPEC.md
spec_commit: $spec_commit
spec_blob: $spec_blob
base_commit: $base
status: active
---
# Maintenance Plan
## Task 1: Fix the defect
- Status: pending
- Dependencies: none
- Scope: bounded fix
- Acceptance criteria: fixed
- Verification: focused test
- Documentation impact: none
## Task 2: Maintenance verification and documentation audit
- Status: pending
- Dependencies: Task 1
- Scope: verify
- Acceptance criteria: all gates pass
- Verification: full gate
- Documentation impact: ledger
EOF
./.factory/tools/validate-maintenance-plan.py planning .factory/artifacts/maintenance-plan.md >/dev/null
cp .factory/artifacts/maintenance-plan.md "$tmp/valid-maintenance-dependencies.md"
for mutation in metadata-order field-order unknown-field; do
    cp "$tmp/valid-maintenance-dependencies.md" .factory/artifacts/maintenance-plan.md
    python3 - "$mutation" <<'PY'
from pathlib import Path
import sys
path=Path('.factory/artifacts/maintenance-plan.md')
text=path.read_text()
if sys.argv[1] == 'metadata-order':
    text=text.replace(
        'bug_id: BUG-0001\nbug_fingerprint:',
        'bug_fingerprint:', 1,
    ).replace('---\nbug_fingerprint:', '---\nbug_fingerprint:', 1)
    # Reinsert bug_id after the fingerprint value to preserve all required keys.
    lines=text.splitlines()
    fingerprint=next(line for line in lines if line.startswith('bug_fingerprint:'))
    index=lines.index(fingerprint)
    lines.insert(index + 1, 'bug_id: BUG-0001')
    text='\n'.join(lines)+'\n'
elif sys.argv[1] == 'field-order':
    text=text.replace(
        '- Status: pending\n- Dependencies: none',
        '- Dependencies: none\n- Status: pending', 1,
    )
else:
    text=text.replace('- Scope: bounded fix', '- Unexpected field: reject\n- Scope: bounded fix', 1)
path.write_text(text)
PY
    if ./.factory/tools/validate-maintenance-plan.py planning .factory/artifacts/maintenance-plan.md >/dev/null 2>&1; then
        echo "test-plan-cycle: maintenance validator accepted $mutation" >&2
        exit 1
    fi
done
for replacement in \
    'Dependencies: none' \
    'Dependencies: Task 2' \
    'Dependencies: Task 99' \
    'Dependencies: Task 1 trailing prose'; do
    cp "$tmp/valid-maintenance-dependencies.md" .factory/artifacts/maintenance-plan.md
    python3 - "$replacement" <<'PY'
from pathlib import Path
import sys
path=Path('.factory/artifacts/maintenance-plan.md')
text=path.read_text()
old='Dependencies: Task 1'
path.write_text(text.replace(old, sys.argv[1], 1))
PY
    if ./.factory/tools/validate-maintenance-plan.py planning .factory/artifacts/maintenance-plan.md >/dev/null 2>&1; then
        echo "test-plan-cycle: maintenance validator accepted $replacement" >&2
        exit 1
    fi
done

echo "test: fresh planning cycles discard completed task context"
