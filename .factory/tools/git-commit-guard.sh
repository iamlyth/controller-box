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

echo "git-commit-guard: substantive commit allowed"
exit 0
