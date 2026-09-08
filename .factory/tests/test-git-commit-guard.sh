#!/usr/bin/env bash
# Adversarial coverage for the fail-closed post-Ralph Git commit boundary.
#
# Policy: a commit must carry at least one substantive tracked path. Empty
# commits and any staged `.ralph/**` addition or modification are rejected;
# deletion of a retired tracked path is allowed so migration can finish. There
# is no lifecycle token, scratchpad exception, or final-handoff authorization.
# Every direct-commit vector must be rejected at the boundary with no metadata
# commit reaching history, while substantive commits remain allowed.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/scripts/pi-cli-shims" "$tmp/.factory/tools/pi-cli-shims" "$tmp/.ralph/agent" "$tmp/sub"
mkdir -p "$tmp/.factory/loop" "$tmp/.factory/schemas" "$tmp/.factory/artifacts" "$tmp/.factory/bugs"
for name in git-commit-guard.sh install-git-commit-guard.sh factory_state_io.py; do
    cp "$PROJECT_ROOT/.factory/tools/$name" "$tmp/.factory/tools/"
done
cp "$PROJECT_ROOT/.factory/tools/pi-cli-shims/git" "$tmp/.factory/tools/pi-cli-shims/git"
chmod +x "$tmp/.factory/tools/"*
# The meaningful-substance boundary (:substance) reuses the committed plan
# parser and its requirement-policy registry, so they must live inside the
# isolated fixture for the guard's ``python3 .factory/loop/substance.py`` to
# resolve deterministically.
cp "$PROJECT_ROOT/.factory/loop/substance.py" "$PROJECT_ROOT/.factory/loop/plan_parser.py" "$tmp/.factory/loop/"
cp "$PROJECT_ROOT/.factory/requirement-policy.json" "$tmp/.factory/"
cp "$PROJECT_ROOT/.factory/schemas/factory-plan-v1.requirements.json" "$tmp/.factory/schemas/"
SHIM="$tmp/.factory/tools/pi-cli-shims/git"

printf '.factory-state/\n.factory-lock\n__pycache__/\n.ralph/*\n!.ralph/agent/\n.ralph/agent/*\n!.ralph/agent/scratchpad.md\n' > "$tmp/.gitignore"
printf 'base\n' > "$tmp/source.txt"
printf '# Initial handoff\n\n## Next\n\n- Start.\n' > "$tmp/.ralph/agent/scratchpad.md"
git -C "$tmp" init -q -b develop
git -C "$tmp" config user.name test
git -C "$tmp" config user.email test@example.invalid
git -C "$tmp" add .
git -C "$tmp" commit -qm initial
initial_head=$(git -C "$tmp" rev-parse HEAD)
# The staged model shim accepts commit-producing commands only for the exact
# launch workspace exported by the trusted control plane.
export FACTORY_LOOP_LAUNCH_WORKSPACE="$tmp"

# A side branch carrying only a scratchpad change, created before the boundary
# is installed, feeds the merge vector that never runs pre-commit.
git -C "$tmp" checkout -qb side
printf '# Side handoff\n\n## Next\n\n- Side scratchpad only.\n' > "$tmp/.ralph/agent/scratchpad.md"
git -C "$tmp" add -f .ralph/agent/scratchpad.md
git -C "$tmp" commit -qm side
side_head=$(git -C "$tmp" rev-parse HEAD)
git -C "$tmp" checkout -q develop

# Installation is idempotent and verifiable; a foreign hook is refused.
# The boundary spans pre-commit, pre-merge-commit and commit-msg so commit
# paths that never run pre-commit stay guarded.
(cd "$tmp" && ./.factory/tools/install-git-commit-guard.sh >/dev/null)
(cd "$tmp" && ./.factory/tools/install-git-commit-guard.sh --check >/dev/null)
(cd "$tmp" && ./.factory/tools/install-git-commit-guard.sh >/dev/null)
for hook_name in pre-commit prepare-commit-msg pre-merge-commit applypatch-msg pre-applypatch commit-msg; do
    [[ -x "$tmp/.git/hooks/$hook_name" ]] || { echo "test-git-commit-guard: missing hook $hook_name" >&2; exit 1; }
done
printf '#!/usr/bin/env bash\n# foreign\n' > "$tmp/.git/hooks/pre-commit"
set +e
(cd "$tmp" && ./.factory/tools/install-git-commit-guard.sh >/dev/null 2>&1)
foreign_rc=$?
set -e
[[ $foreign_rc -eq 2 ]] || { echo "test-git-commit-guard: foreign hook was not refused" >&2; exit 1; }
rm -f "$tmp/.git/hooks/pre-commit"
(cd "$tmp" && ./.factory/tools/install-git-commit-guard.sh >/dev/null)
(cd "$tmp" && ./.factory/tools/install-git-commit-guard.sh --check >/dev/null)

# Empty commits manufacture metadata-only progress and are rejected.
set +e
(cd "$tmp" && git commit --allow-empty -m empty >/dev/null 2>&1)
empty_rc=$?
set -e
[[ $empty_rc -eq 1 ]] || { echo "test-git-commit-guard: empty commit was accepted" >&2; exit 1; }
[[ $(git -C "$tmp" rev-parse HEAD) == "$initial_head" ]]

# Any staged .ralph addition/modification is rejected, even when substantive
# paths ride along: the recovery namespace must never be reintroduced.
printf '# Current handoff\n\n## Next\n\n- Resume update.\n' > "$tmp/.ralph/agent/scratchpad.md"
git -C "$tmp" add -f .ralph/agent/scratchpad.md
set +e
(cd "$tmp" && git commit -m "factory: refresh scratchpad handoff" >/dev/null 2>&1)
scratch_rc=$?
set -e
[[ $scratch_rc -eq 1 ]] || { echo "test-git-commit-guard: scratchpad-only commit was accepted" >&2; exit 1; }
[[ $(git -C "$tmp" rev-parse HEAD) == "$initial_head" ]]
grep -q "Resume update" "$tmp/.ralph/agent/scratchpad.md"

# Every direct-commit vector must be rejected without history progress.
assert_blocked() {
    local label=$1 expected_head=$2
    shift 2
    set +e
    "$@" >/dev/null 2>&1
    local rc=$?
    set -e
    [[ $rc -eq 1 ]] || { echo "test-git-commit-guard: vector '$label' returned $rc" >&2; exit 1; }
    [[ $(git -C "$tmp" rev-parse HEAD) == "$expected_head" ]] || {
        echo "test-git-commit-guard: vector '$label' advanced history" >&2; exit 1; }
}

printf '# Pathspec\n\n## Next\n\n- pathspec.\n' > "$tmp/.ralph/agent/scratchpad.md"
assert_blocked "pathspec commit" "$initial_head" \
    git -C "$tmp" commit -m pathspec -- .ralph/agent/scratchpad.md
assert_blocked "git -C commit" "$initial_head" \
    git -C "$tmp" commit -m gitslashC
assert_blocked "alternate cwd commit" "$initial_head" \
    bash -c "cd '$tmp/sub' && git commit -m altcwd"
assert_blocked "command/env prefix" "$initial_head" \
    bash -c "cd '$tmp' && env git commit -m envprefix"
printf '# Amend\n\n## Next\n\n- amend.\n' > "$tmp/.ralph/agent/scratchpad.md"
git -C "$tmp" add -f .ralph/agent/scratchpad.md
assert_blocked "amend scratchpad-only" "$initial_head" \
    git -C "$tmp" commit --amend -m amended
assert_blocked "gitlink scratchpad" "$initial_head" bash -c "
    git -C '$tmp' rm -q --cached .ralph/agent/scratchpad.md
    git -C '$tmp' update-index --add --cacheinfo 160000,$(printf 'd%.0s' {1..40}),.ralph/agent/scratchpad.md
    git -C '$tmp' commit -m gitlink
"
git -C "$tmp" reset -q
printf '# Symlink\n\n## Next\n\n- symlink.\n' > "$tmp/.ralph/agent/scratchpad.md"
printf 'outside\n' > "$tmp/outside.txt"
rm "$tmp/.ralph/agent/scratchpad.md"
ln -s ../outside.txt "$tmp/.ralph/agent/scratchpad.md"
git -C "$tmp" add -f .ralph/agent/scratchpad.md
assert_blocked "symlink scratchpad" "$initial_head" git -C "$tmp" commit -m symlink
git -C "$tmp" reset -q
rm "$tmp/.ralph/agent/scratchpad.md" "$tmp/outside.txt"
printf '# Restored\n\n## Next\n\n- restored.\n' > "$tmp/.ralph/agent/scratchpad.md"

# A substantive commit that also stages a .ralph modification is rejected.
printf '# Mixed\n\n## Next\n\n- mixed.\n' > "$tmp/.ralph/agent/scratchpad.md"
printf 'changed\n' >> "$tmp/source.txt"
git -C "$tmp" add -A
assert_blocked "substantive with scratchpad" "$initial_head" \
    git -C "$tmp" commit -m mixed
git -C "$tmp" restore --staged .ralph 2>/dev/null || true
git -C "$tmp" restore -- source.txt

# A substantive commit without any .ralph path is allowed.
printf 'changed\n' >> "$tmp/source.txt"
git -C "$tmp" add source.txt
(cd "$tmp" && git commit -qm "implement: source change")
source_head=$(git -C "$tmp" rev-parse HEAD)
[[ "$source_head" != "$initial_head" ]]
mapfile -t changed < <(git -C "$tmp" diff-tree --no-commit-id --name-only -r HEAD | sort)
[[ "${changed[*]}" == 'source.txt' ]]

# Commit-creation paths that never run pre-commit are still guarded: merge
# runs pre-merge-commit + commit-msg (both installed), and the argv shim
# refuses cherry-pick/revert/am/rebase/pull (no hook coverage) so the model
# can never create commits through them. A branch whose only change is a
# scratchpad refresh must not reach history through any path.
git -C "$tmp" merge --no-ff side >/dev/null 2>&1 || true
[[ $(git -C "$tmp" rev-parse HEAD) == "$source_head" ]]
git -C "$tmp" merge --abort 2>/dev/null || true
git -C "$tmp" reset -q --hard "$source_head" >/dev/null
set +e
(cd "$tmp" && "$SHIM" cherry-pick "$side_head" >/dev/null 2>&1)
cherry_shim_rc=$?
(cd "$tmp" && "$SHIM" revert "$side_head" >/dev/null 2>&1)
revert_shim_rc=$?
(cd "$tmp" && "$SHIM" am /tmp/nonexistent.patch >/dev/null 2>&1)
am_shim_rc=$?
(cd "$tmp" && "$SHIM" rebase "$side_head" >/dev/null 2>&1)
rebase_shim_rc=$?
set -e
[[ $cherry_shim_rc -eq 1 && $revert_shim_rc -eq 1 && $am_shim_rc -eq 1 && $rebase_shim_rc -eq 1 ]]
[[ $(git -C "$tmp" rev-parse HEAD) == "$source_head" ]]

# The argv-level shim rejects hook-bypass attempts and missing guards, and
# delegates non-commit and substantive operations transparently.
for bypass in "--no-verify" "-n"; do
    set +e
    (cd "$tmp" && "$SHIM" commit "$bypass" -m "bypass-$bypass" >/dev/null 2>&1)
    rc=$?
    set -e
    [[ $rc -eq 1 ]] || { echo "test-git-commit-guard: shim accepted $bypass" >&2; exit 1; }
done
# Combined short-option clusters: -n is the no-verify flag only when it is a
# standalone flag or a member of a cluster of no-value options. It must be
# rejected in -an/-qn/-sn/-vn/-pn/-on/-zn and when it follows an optional-value
# -S/-u as a separate argv (-S -n, -u -n), but never when it is the value of a
# value-taking option (-m -n, -mn, -cn) or the optional value of -S/-u (-Sn,
# -un).
for cluster in -an -qn -sn -vn -pn -on -zn; do
    set +e
    (cd "$tmp" && "$SHIM" commit "$cluster" -m "bypass-$cluster" >/dev/null 2>&1)
    rc=$?
    set -e
    [[ $rc -eq 1 ]] || { echo "test-git-commit-guard: shim accepted cluster $cluster" >&2; exit 1; }
done
for pair in "-S -n" "-u -n"; do
    set +e
    # shellcheck disable=SC2086 # intended word splitting: each item is a full argv fragment
    (cd "$tmp" && "$SHIM" commit $pair -m "bypass-$pair" >/dev/null 2>&1)
    rc=$?
    set -e
    [[ $rc -eq 1 ]] || { echo "test-git-commit-guard: shim accepted $pair" >&2; exit 1; }
done
# Must-allow: -n as the value of a value-taking option is not the no-verify
# flag, so the shim delegates and the substantive commit succeeds.
printf 'shim-allow\n' >> "$tmp/source.txt"
git -C "$tmp" add source.txt
set +e
(cd "$tmp" && "$SHIM" commit -m -n >/dev/null 2>&1)
rc=$?
set -e
[[ $rc -eq 0 ]] || { echo "test-git-commit-guard: shim rejected -m -n (message value)" >&2; exit 1; }
printf 'shim-allow-mn\n' >> "$tmp/source.txt"
git -C "$tmp" add source.txt
set +e
(cd "$tmp" && "$SHIM" commit -mn >/dev/null 2>&1)
rc=$?
set -e
[[ $rc -eq 0 ]] || { echo "test-git-commit-guard: shim rejected -mn (message value)" >&2; exit 1; }
for argument in "-c core.hooksPath=/tmp/x commit -m hooks" \
        "--config core.hooksPath=/tmp/x commit -m hooks"; do
    set +e
    # shellcheck disable=SC2086 # intended word splitting: each item is a full argv fragment
    (cd "$tmp" && "$SHIM" $argument >/dev/null 2>&1)
    rc=$?
    set -e
    [[ $rc -eq 1 ]] || { echo "test-git-commit-guard: shim accepted hooksPath redirect" >&2; exit 1; }
done
set +e
(cd "$tmp" && GIT_CONFIG_COUNT=1 "$SHIM" commit -m gconfig >/dev/null 2>&1)
rc=$?
set -e
[[ $rc -eq 1 ]]
# Alternate repository/object/index environment and argv forms are rejected
# by the shim itself even if an extension rewrite is bypassed.
for variable in GIT_DIR GIT_WORK_TREE GIT_COMMON_DIR GIT_OBJECT_DIRECTORY \
        GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_NAMESPACE GIT_INDEX_FILE \
        GIT_CONFIG GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM; do
    set +e
    (cd "$tmp" && env "$variable=$tmp/.git" "$SHIM" commit -m "alt-$variable" >/dev/null 2>&1)
    rc=$?
    set -e
    [[ $rc -eq 1 ]] || { echo "test-git-commit-guard: shim accepted $variable" >&2; exit 1; }
done
for alternate in "-C $tmp commit -m alt-C" \
        "--git-dir=$tmp/.git commit -m alt-git-dir" \
        "--work-tree=$tmp commit -m alt-work-tree"; do
    set +e
    # shellcheck disable=SC2086 # intended adversarial argv fragments
    (cd "$tmp" && "$SHIM" $alternate >/dev/null 2>&1)
    rc=$?
    set -e
    [[ $rc -eq 1 ]] || { echo "test-git-commit-guard: shim accepted $alternate" >&2; exit 1; }
done
other_repo="$tmp/other-repo"
mkdir "$other_repo"
git -C "$other_repo" init -q
set +e
(cd "$other_repo" && "$SHIM" commit -m foreign-repo >/dev/null 2>&1)
rc=$?
set -e
[[ $rc -eq 1 ]] || { echo "test-git-commit-guard: shim accepted foreign repository cwd" >&2; exit 1; }
git -C "$tmp" config core.hooksPath /tmp/other-hooks
set +e
(cd "$tmp" && "$SHIM" commit -m hooked >/dev/null 2>&1)
rc=$?
set -e
[[ $rc -eq 1 ]]
git -C "$tmp" config --unset core.hooksPath
mv "$tmp/.git/hooks/pre-commit" "$tmp/.git/hooks/pre-commit.bak"
set +e
(cd "$tmp" && "$SHIM" commit -m nohook >/dev/null 2>&1)
rc=$?
set -e
[[ $rc -eq 1 ]]
mv "$tmp/.git/hooks/pre-commit.bak" "$tmp/.git/hooks/pre-commit"
printf 'shim\n' >> "$tmp/source.txt"
git -C "$tmp" add source.txt
set +e
(cd "$tmp" && "$SHIM" commit -m "substantive via shim" >/dev/null 2>&1)
rc=$?
set -e
[[ $rc -eq 0 ]]
(cd "$tmp" && "$SHIM" status --porcelain >/dev/null)
(cd "$tmp" && "$SHIM" rev-parse HEAD >/dev/null)

# Every non-commit direct Git call is a genuinely read-only allowlisted verb.
# Plumbing/object/ref mutators and output-writing/external-helper options are
# refused before dispatch, leaving HEAD, refs, and the object inventory exact.
readonly_head=$(git -C "$tmp" rev-parse HEAD)
readonly_refs=$(git -C "$tmp" show-ref | sort)
readonly_objects=$(git -C "$tmp" count-objects -v)
tree=$(git -C "$tmp" rev-parse 'HEAD^{tree}')
for mutator in \
        "commit-tree $tree -m forged" \
        "update-ref refs/heads/forged $readonly_head" \
        "fast-import" \
        "hash-object -w source.txt" \
        "replace $readonly_head $readonly_head" \
        "notes add -m forged $readonly_head" \
        "branch forged $readonly_head" \
        "diff --output=$tmp/forged.diff" \
        "show --textconv HEAD"; do
    set +e
    # shellcheck disable=SC2086 # adversarial argv fragments are intentional
    (cd "$tmp" && "$SHIM" $mutator </dev/null >/dev/null 2>&1)
    rc=$?
    set -e
    [[ $rc -eq 1 ]] || { echo "test-git-commit-guard: shim accepted mutator $mutator" >&2; exit 1; }
done
[[ ! -e "$tmp/forged.diff" ]]
[[ $(git -C "$tmp" rev-parse HEAD) == "$readonly_head" ]]
[[ $(git -C "$tmp" show-ref | sort) == "$readonly_refs" ]]
[[ $(git -C "$tmp" count-objects -v) == "$readonly_objects" ]]

# Shell indirection cannot bypass the boundary.  Production places the exact
# staged shim first in a sealed/sanitized child PATH; these adversarial forms
# deliberately evade direct-text reduction but their eventual unqualified
# `git` exec still reaches the shim and cannot mutate refs/history.
sealed_bin="$tmp/sealed-bin"
mkdir "$sealed_bin"
cp "$SHIM" "$sealed_bin/git"
chmod 500 "$sealed_bin" "$sealed_bin/git"
source_vector="$tmp/indirect-source.sh"
printf '%s\n' 'git update-ref refs/heads/indirect-source HEAD' > "$source_vector"
# shellcheck disable=SC2016 # literal payloads expand only in the nested adversarial shell
for vector in \
    'g=git; "$g" update-ref refs/heads/indirect-variable HEAD' \
    'a=g; b=it; "$a$b" update-ref refs/heads/indirect-concat HEAD' \
    'shopt -s expand_aliases; alias g=git; eval "g update-ref refs/heads/indirect-alias HEAD"' \
    'g(){ git "$@"; }; g update-ref refs/heads/indirect-function HEAD' \
    'cmd="git update-ref refs/heads/indirect-eval HEAD"; eval "$cmd"' \
    '. "$INDIRECT_SOURCE"'; do
    set +e
    (cd "$tmp" && PATH="$sealed_bin:$PATH" INDIRECT_SOURCE="$source_vector" \
        bash -c "$vector" >/dev/null 2>&1)
    rc=$?
    set -e
    [[ $rc -eq 1 ]] || {
        echo "test-git-commit-guard: indirection vector escaped: $vector (rc=$rc)" >&2
        exit 1
    }
done
[[ -z $(git -C "$tmp" for-each-ref --format='%(refname)' 'refs/heads/indirect-*') ]]
[[ $(git -C "$tmp" rev-parse HEAD) == "$readonly_head" ]]

# Configured read helpers never execute.  Status runs with fsmonitor and
# untracked-cache disabled under fixed system/global config, while the
# diff/show/log/blame family is rejected rather than risking external diff or
# textconv drivers.
helper_marker="$tmp/helper.marker"
helper="$tmp/helper.sh"
printf '#!/usr/bin/env bash\nprintf helper > %q\nexit 0\n' "$helper_marker" > "$helper"
chmod +x "$helper"
git -C "$tmp" config core.fsmonitor "$helper"
git -C "$tmp" config diff.evil.command "$helper"
git -C "$tmp" config diff.evil.textconv "$helper"
printf '*.txt diff=evil\n' > "$tmp/.gitattributes"
(cd "$tmp" && "$SHIM" status --short >/dev/null)
[[ ! -e "$helper_marker" ]] || {
    echo "test-git-commit-guard: fsmonitor helper executed" >&2; exit 1; }
for unsafe_read in 'diff HEAD' 'show HEAD' 'log -1' 'blame source.txt'; do
    set +e
    # shellcheck disable=SC2086 # intentional exact adversarial argv fragments
    (cd "$tmp" && "$SHIM" $unsafe_read >/dev/null 2>&1)
    rc=$?
    set -e
    [[ $rc -eq 1 ]] || {
        echo "test-git-commit-guard: helper-capable read accepted: $unsafe_read" >&2
        exit 1
    }
done
[[ ! -e "$helper_marker" ]] || {
    echo "test-git-commit-guard: diff/textconv helper executed" >&2; exit 1; }
git -C "$tmp" config --unset core.fsmonitor
git -C "$tmp" config --remove-section diff.evil
rm -f "$tmp/.gitattributes" "$helper" "$source_vector"
chmod 700 "$sealed_bin"

# Empty-but-present repository/config/alternate environment variables are
# still an attempted override and fail closed before dispatch.
for variable in GIT_DIR GIT_OBJECT_DIRECTORY GIT_ALTERNATE_OBJECT_DIRECTORIES \
        GIT_CONFIG GIT_CONFIG_COUNT GIT_EXEC_PATH GIT_SHALLOW_FILE; do
    set +e
    (cd "$tmp" && env "$variable=" "$SHIM" status --short >/dev/null 2>&1)
    rc=$?
    set -e
    [[ $rc -eq 1 ]] || { echo "test-git-commit-guard: shim accepted empty $variable" >&2; exit 1; }
done

# The one permitted `.ralph` transition is removal from the index. This is a
# substantive migration commit and cannot reintroduce bytes.
git -C "$tmp" rm -qr .ralph
git -C "$tmp" add -u
(cd "$tmp" && git commit -qm 'migration: remove retired recovery namespace')
[[ -z $(git -C "$tmp" ls-files '.ralph/**') ]]

# -- meaningful-substance plan boundary -----------------------------------
# An entirely-administrative commit (implementation plan, bug ledgers, or
# campaign audit/evidence sidecar only) is allowed only when the staged plan
# carries a genuine semantic planning change.  Seed the canonical plan into
# the fixture HEAD through a substantive commit, then exercise the plan-only
# allowed/rejected decision.
PLAN_REL=".factory/artifacts/implementation-plan.md"
cp "$PROJECT_ROOT/$PLAN_REL" "$tmp/$PLAN_REL"
printf 'seed\n' >> "$tmp/source.txt"
git -C "$tmp" add "$PLAN_REL" source.txt
(cd "$tmp" && git commit -qm "implement: seed canonical plan")
seed_head=$(git -C "$tmp" rev-parse HEAD)

# A plan-only commit that carries a genuine semantic planning change (task 1
# priority bumped, plan still parseable) is allowed and advances history.
python3 - "$tmp/$PLAN_REL" "$tmp/.factory/loop" <<'PY'
import re, sys
sys.path.insert(0, sys.argv[2])
from plan_parser import Plan
path = sys.argv[1]
plan = Plan.from_file(path)
for blk in plan._blocks:
    heading = (blk.heading or '').strip()
    if re.match(r'^## Task\s+1:', heading):
        for i, line in enumerate(blk.lines):
            match = re.match(r'^(-?\s*Priority:\s*)(\d+)$', line.strip())
            if match:
                blk.lines[i] = line.replace(
                    match.group(2), str(int(match.group(2)) + 1), 1)
                break
        break
open(path, 'w', encoding='utf-8').write(plan.serialize())
PY
git -C "$tmp" add "$PLAN_REL"
(cd "$tmp" && git commit -qm "plan: genuine semantic task-1 priority change")
semantic_head=$(git -C "$tmp" rev-parse HEAD)
[[ "$semantic_head" != "$seed_head" ]]

# A plan-only commit that merely extends Evidence/verification prose (no
# semantic planning field changed) is rejected without advancing history.
python3 - "$tmp/$PLAN_REL" "$tmp/.factory/loop" <<'PY'
import re, sys
sys.path.insert(0, sys.argv[2])
from plan_parser import Plan
path = sys.argv[1]
plan = Plan.from_file(path)
applied = False
for blk in plan._blocks:
    heading = (blk.heading or '').strip()
    if re.match(r'^## Task\s+1:', heading):
        for i, line in enumerate(blk.lines):
            if line.strip().startswith('- Verification:'):
                blk.lines.insert(i + 1, '  appended prose-only note')
                applied = True
                break
        break
assert applied, 'no Verification field to extend'
open(path, 'w', encoding='utf-8').write(plan.serialize())
PY
git -C "$tmp" add "$PLAN_REL"
set +e
(cd "$tmp" && git commit -m "plan: prose-only revision" >/dev/null 2>&1)
prose_rc=$?
set -e
[[ $prose_rc -eq 1 ]] || { echo "test-git-commit-guard: prose-only plan commit was accepted" >&2; exit 1; }
[[ $(git -C "$tmp" rev-parse HEAD) == "$semantic_head" ]]

# An administrative-only commit that touches no substantive path and no plan
# (a bug ledger only) is rejected without advancing history.
printf '# Bug ledger\n\n- BUG-0000: seeded.\n' > "$tmp/.factory/bugs/open.md"
git -C "$tmp" add .factory/bugs/open.md
set +e
(cd "$tmp" && git commit -m "admin: bug ledger only" >/dev/null 2>&1)
ledger_rc=$?
set -e
[[ $ledger_rc -eq 1 ]] || { echo "test-git-commit-guard: bug-ledger-only commit was accepted" >&2; exit 1; }
[[ $(git -C "$tmp" rev-parse HEAD) == "$semantic_head" ]]

echo 'test: fail-closed Git commit boundary passed'
