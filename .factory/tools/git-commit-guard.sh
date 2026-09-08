#!/usr/bin/env bash
# Fail-closed post-Ralph Git commit boundary.
#
# Installed by .factory/tools/install-git-commit-guard.sh as six Git hooks that all
# exec this script:
#
#   pre-commit          policy gate for `git commit`
#   prepare-commit-msg  policy gate for `git commit` and `git rebase`
#   pre-merge-commit    policy gate for `git merge`
#   applypatch-msg      policy gate for `git am`
#   pre-applypatch      policy gate for `git am`
#   commit-msg          policy gate for every commit-creation path
#
# Policy: a commit must carry at least one substantive tracked path. An empty
# commit manufactures metadata-only progress and is rejected. A `.ralph/**`
# path may only be deleted from the index; any staged addition or modification
# is rejected so the retired recovery namespace can never be reintroduced.
# There is no lifecycle token, scratchpad exception, legacy lifecycle
# environment, or final-handoff authorization. All six hooks enforce the same
# content policy, so commit-creation paths that never run pre-commit (merge,
# cherry-pick, revert, am, rebase) are guarded identically.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)

if [[ ${1:-} == --hook ]]; then
    shift 2
fi

# A hook always runs inside the repository, but the guard must also work when
# invoked directly by tests from a nested cwd. Resolve the repo and refuse to
# guard a different repository than the one that owns this script.
REPO_ROOT=$(git rev-parse --show-toplevel 2>/dev/null || true)
if [[ -z "$REPO_ROOT" ]]; then
    echo "git-commit-guard: not inside a Git repository" >&2
    exit 1
fi
if [[ "$(realpath -e -- "$REPO_ROOT")" != "$(realpath -e -- "$PROJECT_ROOT")" ]]; then
    echo "git-commit-guard: guard repository mismatch ($REPO_ROOT)" >&2
    exit 1
fi
cd -- "$PROJECT_ROOT"

# Staged paths relative to the repository root, NUL-separated so that any path
# (spaces, newlines) is handled exactly.
mapfile -d '' -t STAGED < <(git diff --cached --name-only -z)

if [[ ${#STAGED[@]} -eq 0 ]]; then
    echo "git-commit-guard: empty commits manufacture metadata-only progress" >&2
    exit 1
fi

# A retired recovery path is acceptable only when absent from the staged
# index (a deletion). Additions and modifications fail closed even when
# substantive paths ride along.
for path in "${STAGED[@]}"; do
    if [[ "$path" == .ralph || "$path" == .ralph/* ]]; then
        if git ls-files --error-unmatch -- "$path" >/dev/null 2>&1; then
            echo "git-commit-guard: staged Ralph recovery path is forbidden: $path" >&2
            exit 1
        fi
    fi
done

# Meaningful-substance boundary: a commit must carry at least one substantive
# path, or be an entirely administrative commit whose implementation-plan
# change is a genuine semantic planning change.  The shared classifier
# (.factory/loop/substance.py) owns both the narrow administrative path set
# (implementation plan, bug ledgers, campaign audit/evidence sidecars) and the
# semantic plan fingerprint; everything else (stable source/config/policies/
# schemas/prompts, product config/docs/tests, authenticated conformance and
# capability contracts) is substantive.
SUBSTANCE="$PROJECT_ROOT/.factory/loop/substance.py"
classify_out=$(python3 "$SUBSTANCE" classify "${STAGED[@]}") || {
    echo "git-commit-guard: substance classifier could not classify the staged set" >&2
    exit 1
}
if [[ "$classify_out" == substantive ]]; then
    echo "git-commit-guard: substantive commit allowed"
    exit 0
fi
if [[ "$classify_out" == empty ]]; then
    echo "git-commit-guard: empty commits manufacture metadata-only progress" >&2
    exit 1
fi

# The staged set is entirely administrative ("admin 0/1").  Only the
# implementation plan can make such a commit meaningful, and only through a
# genuine semantic planning change (task add/remove/reorder, or a change to a
# task's title, priority, dependencies, Scope, or Acceptance criteria);
# Evidence text, completion/status markers, timestamps, and iteration prose
# are not meaningful.
if [[ "${classify_out##* }" != 1 ]]; then
    echo "git-commit-guard: administrative-only commit without a genuine semantic plan change" >&2
    exit 1
fi

PLAN_REL=".factory/artifacts/implementation-plan.md"
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
tmp_old="$tmpdir/old-plan.md"
tmp_new="$tmpdir/new-plan.md"
# HEAD bytes (empty when HEAD predates the plan) and staged index bytes.
if git rev-parse --verify -q HEAD >/dev/null 2>&1 \
        && git cat-file -e "HEAD:$PLAN_REL" >/dev/null 2>&1; then
    git show "HEAD:$PLAN_REL" > "$tmp_old"
else
    : > "$tmp_old"
fi
if ! git show ":$PLAN_REL" > "$tmp_new" 2>/dev/null; then
    echo "git-commit-guard: staged implementation plan is not available for comparison" >&2
    exit 1
fi

set +e
decision=$(python3 "$SUBSTANCE" plan-semantic --old "$tmp_old" --new "$tmp_new" 2>/dev/null)
rc=$?
set -e
if [[ $rc -eq 0 && "$decision" == changed ]]; then
    echo "git-commit-guard: plan-only commit with genuine semantic planning change"
    exit 0
fi
if [[ -z "$decision" ]]; then
    echo "git-commit-guard: could not classify the implementation plan revision" >&2
    exit 1
fi
echo "git-commit-guard: plan-only commit without a genuine semantic planning change (Evidence/status/prose edits are not meaningful)" >&2
exit 1
