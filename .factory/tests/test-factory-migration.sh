#!/usr/bin/env bash
# test-factory-migration.sh — hidden-namespace Task 15 post-migration shell
# driver.
#
# The specification (HIDE-01, §3) keeps harness-only tests out of the adopting
# product's visible `tests/` tree, so the migration suite lives under the
# hidden `.factory/tests/` namespace like the Task 13 footprint series.  This
# driver:
#
#   1. runs `.factory/tests/test-factory-migration.py` warning-free under
#      `-W error::ResourceWarning` (tracked absence of the forbidden legacy
#      pathnames, freeze-marker safety, canonical plan/spec/roles currency,
#      foreign `.ralph/` preservation);
#   2. verifies the tracked freeze marker exists as a regular non-executable
#      file and the migration `status` CLI reports the Ralph removal is
#      complete on the live repository (no forbidden tracked pathname, safe
#      marker, presence-only `.ralph/` report);
#   3. proves the freeze-guard exit table (0 frozen / 1 not frozen / 2
#      unsafe) through the retained hidden authority
#      (`.factory/loop/migration.py freeze --guard`) in private fixtures,
#      and that a symlink/FIFO marker fails closed without silently
#      unfreezing;
#   4. proves the foreign `.ralph/` directory is never opened, enumerated,
#      or modified: a fixture sentinel's bytes, mode, and mtime are
#      preserved exactly across `status`/`verify` runs, and the live
#      repository's `.ralph/` entry is only ever lstat'ed for presence.
#
# The driver never writes to the live repository and never opens, reads, or
# deletes anything under `.ralph/`.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
cd -- "$ROOT"

PY=${PYTHON:-python3}

fail() {
    echo "test-factory-migration: $*" >&2
    exit 1
}

# -- Controlled shell context -----------------------------------------------
# Strip the same Git redirector families the pinned boundary strips, so a
# caller environment cannot steer the fixture commands at a different object
# store, index, work tree, or config set.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
    GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_NAMESPACE \
    GIT_CEILING_DIRECTORIES GIT_SSH GIT_SSH_COMMAND GIT_ASKPASS \
    GIT_TERMINAL_PROMPT GIT_EXEC_PATH GIT_TEMPLATE_DIR \
    GIT_CONFIG_PARAMETERS GIT_CONFIG GIT_CONFIG_SYSTEM GIT_CONFIG_GLOBAL \
    GIT_CONFIG_NOSYSTEM GIT_CONFIG_COUNT 2>/dev/null || true
unset "${!GIT_CONFIG_KEY_@}" "${!GIT_CONFIG_VALUE_@}" 2>/dev/null || true

tmp=$(mktemp -d "${TMPDIR:-/tmp}/factory-migration.XXXXXX")
cleanup() {
    rm -rf -- "$tmp"
}
# shellcheck disable=SC2154 # rc is assigned inside the trap from $?
trap 'rc=$?; cleanup; trap - EXIT INT TERM; exit "$rc"' EXIT INT TERM

# -- 1. The hidden Python suite runs warning-free ---------------------------
echo "test-factory-migration: running .factory/tests/test-factory-migration.py"
"$PY" -W error::ResourceWarning .factory/tests/test-factory-migration.py \
    >"$tmp/suite.log" 2>&1 || {
    echo "test-factory-migration: Python suite failed:" >&2
    tail -60 "$tmp/suite.log" >&2
    exit 1
}
if grep -qiE 'ResourceWarning|unclosed file' "$tmp/suite.log"; then
    echo "test-factory-migration: ResourceWarning leaked from the suite:" >&2
    grep -iE 'ResourceWarning|unclosed file' "$tmp/suite.log" | head -20 >&2
    exit 1
fi

# -- 2. Freeze marker and live-repo migration status ------------------------
# The tracked freeze marker must be a regular non-executable file.
[[ -f .factory/ralph-freeze && ! -L .factory/ralph-freeze ]] \
    || fail "the tracked .factory/ralph-freeze marker must be a regular file"
git ls-files --error-unmatch .factory/ralph-freeze >/dev/null 2>&1 \
    || fail "the freeze marker must be tracked"
if [[ -x .factory/ralph-freeze ]]; then
    fail "the freeze marker must be non-executable"
fi

# The migration status CLI reports the Ralph removal is complete on the live
# repository: no forbidden tracked pathname, a safe marker, and a
# presence-only `.ralph/` report.
"$PY" .factory/loop/migration.py --root "$ROOT" status >"$tmp/status.json" 2>&1 \
    || fail "the live migration status failed: $(tail -3 "$tmp/status.json")"
"$PY" - "$tmp/status.json" <<'PY'
import json, sys
payload = json.load(open(sys.argv[1], encoding="utf-8"))
assert payload.get("schema") == "factory-migration/v1", payload
assert payload["tracked_forbidden"] == [], payload["tracked_forbidden"]
assert payload["freeze_marker"]["safe"], payload["freeze_marker"]
assert payload["freeze_marker"]["tracked"], payload["freeze_marker"]
assert not payload["freeze_marker"]["executable"], payload["freeze_marker"]
assert "ralph_presence" in payload and isinstance(
    payload["ralph_presence"]["present"], bool), payload
PY
echo "test-factory-migration: live migration status clean (tracked absence, safe marker)"

# -- 3. Freeze-guard exit table through the retained authority --------------
# The guard's own exit table (0 frozen / 1 not frozen / 2 unsafe) is checked
# directly against the retained hidden authority in a private fixture, so a
# symlink/FIFO marker fails closed without silently unfreezing.
provision_loop_authority() {
    local fixture="$1"
    mkdir -p "$fixture/.factory/loop" "$fixture/.factory/schemas"
    for module in migration gitutil plan_parser; do
        cp "$ROOT/.factory/loop/$module.py" "$fixture/.factory/loop/$module.py"
    done
    # The plan parser loads the committed requirement-policy map and its
    # flat registry at import time; the fixture provisions them exactly like
    # a real deployment would.
    cp "$ROOT/.factory/requirement-policy.json" \
        "$fixture/.factory/requirement-policy.json"
    cp "$ROOT/.factory/schemas/factory-plan-v1.requirements.json" \
        "$fixture/.factory/schemas/factory-plan-v1.requirements.json"
}

guard_fixture="$tmp/guard-fixture"
mkdir -p "$guard_fixture/.factory"
provision_loop_authority "$guard_fixture"
guard=("$PY" "$guard_fixture/.factory/loop/migration.py" \
    --root "$guard_fixture" freeze --guard)
guard_rc=0
"${guard[@]}" >/dev/null 2>&1 || guard_rc=$?
[[ $guard_rc -eq 1 ]] \
    || fail "missing marker must report not-frozen (exit 1, got $guard_rc)"
printf '# frozen\n' > "$guard_fixture/.factory/ralph-freeze"
"${guard[@]}" >/dev/null 2>&1 \
    || fail "a regular marker must report frozen (exit 0)"
ln -sfn "$guard_fixture/ralph-freeze-target" "$guard_fixture/.factory/ralph-freeze"
printf 't\n' > "$guard_fixture/ralph-freeze-target"
guard_rc=0
"${guard[@]}" >/dev/null 2>&1 || guard_rc=$?
[[ $guard_rc -eq 2 ]] \
    || fail "an unsafe (symlink) marker must fail closed (exit 2, got $guard_rc)"
rm -f "$guard_fixture/.factory/ralph-freeze" "$guard_fixture/ralph-freeze-target"
mkfifo "$guard_fixture/.factory/ralph-freeze"
guard_rc=0
"${guard[@]}" >/dev/null 2>&1 || guard_rc=$?
[[ $guard_rc -eq 2 ]] \
    || fail "an unsafe (FIFO) marker must fail closed (exit 2, got $guard_rc)"
echo "test-factory-migration: freeze-guard exit table and unsafe-marker closure verified"

# -- 4. Foreign .ralph/ preservation ----------------------------------------
# A fixture repository with a foreign `.ralph/` sentinel: `status`/`verify`
# never open, enumerate, or modify it — the sentinel's bytes, mode, and
# mtime are preserved exactly, and no sentinel byte reaches the report.
ralph_fixture="$tmp/ralph-fixture"
mkdir -p "$ralph_fixture/.factory/artifacts" "$ralph_fixture/.factory/prompts" \
    "$ralph_fixture/docs" "$ralph_fixture/.ralph/agent"
printf '.ralph/\n.factory-state/\n' > "$ralph_fixture/.gitignore"
printf '# Fixture specification\n' > "$ralph_fixture/docs/SPEC.md"
printf '# frozen\n' > "$ralph_fixture/.factory/ralph-freeze"
for role in planner developer tester auditor; do
    printf '# %s fixture prompt\n' "$role" > "$ralph_fixture/.factory/prompts/$role.md"
done
printf 'FOREIGN-RALPH-SENTINEL-7c2d9e\n' > "$ralph_fixture/.ralph/agent/tasks.jsonl"
git -C "$ralph_fixture" init -q -b boilerplate-develop
git -C "$ralph_fixture" config user.email factory@test
git -C "$ralph_fixture" config user.name factory
git -C "$ralph_fixture" add -A
git -C "$ralph_fixture" commit -qm baseline
# Bind the canonical plan to the committed spec blob.
spec_blob=$(git -C "$ralph_fixture" rev-parse HEAD:docs/SPEC.md)
head=$(git -C "$ralph_fixture" rev-parse HEAD)
sed -e "s/^spec_commit: .*/spec_commit: $head/" \
    -e "s/^spec_blob: .*/spec_blob: $spec_blob/" \
    -e "s/^base_commit: .*/base_commit: $head/" \
    "$ROOT/.factory/tests/fixtures/plan-valid-base.md" \
    > "$ralph_fixture/.factory/artifacts/implementation-plan.md"
git -C "$ralph_fixture" add -A
git -C "$ralph_fixture" commit -qm "bind the canonical plan"
sentinel="$ralph_fixture/.ralph/agent/tasks.jsonl"
before_bytes=$(sha256sum "$sentinel" | awk '{print $1}')
before_mode=$(stat -c '%a' "$sentinel")
before_mtime=$(stat -c '%Y' "$sentinel")
"$PY" .factory/loop/migration.py --root "$ralph_fixture" status >"$tmp/rf-status.json" 2>&1 \
    || fail "fixture migration status failed: $(tail -3 "$tmp/rf-status.json")"
"$PY" .factory/loop/migration.py --root "$ralph_fixture" verify >"$tmp/rf-verify.json" 2>&1 \
    || fail "fixture migration verify failed: $(tail -3 "$tmp/rf-verify.json")"
grep -q 'FOREIGN-RALPH-SENTINEL' "$tmp/rf-status.json" "$tmp/rf-verify.json" \
    && fail "a foreign .ralph byte leaked into a migration report"
[[ "$(sha256sum "$sentinel" | awk '{print $1}')" == "$before_bytes" ]] \
    || fail "the foreign .ralph sentinel bytes were modified"
[[ "$(stat -c '%a' "$sentinel")" == "$before_mode" ]] \
    || fail "the foreign .ralph sentinel mode was modified"
[[ "$(stat -c '%Y' "$sentinel")" == "$before_mtime" ]] \
    || fail "the foreign .ralph sentinel mtime was modified"
echo "test-factory-migration: foreign .ralph sentinel preserved; no legacy bytes in reports"

echo "test-factory-migration: all checks passed"
