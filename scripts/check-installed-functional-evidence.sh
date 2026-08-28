#!/usr/bin/env bash
set -euo pipefail

root=$(git rev-parse --show-toplevel)
evidence=$(python3 - "$root" "${FACTORY_INSTALLED_FUNCTIONAL_EVIDENCE_PATH:-}" <<'PY'
import os, re, stat, sys
from pathlib import Path
root = Path(sys.argv[1]).absolute()
override = sys.argv[2]
if not override:
    print(root / '.factory-state/installed-functional-evidence.env')
    raise SystemExit(0)
path = Path(override)
if not path.is_absolute():
    raise SystemExit('installed-functional-evidence: override must be absolute')
try:
    parts = path.relative_to(root).parts
except ValueError:
    raise SystemExit('installed-functional-evidence: override escapes repository')
if (len(parts) != 4 or parts[:2] != ('.factory-state', 'campaigns')
        or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}', parts[2])
        or parts[3] != 'installed-functional-evidence.env'):
    raise SystemExit('installed-functional-evidence: override is not campaign-owned')
# Complete canonical-ancestor validation (Task 34/35): every ancestor from
# the repository root to the evidence file must be a canonical non-symlink
# directory owned by the current user; the campaign namespace directories
# must be mode 0700.  A symlinked or wrong-owner ancestor anywhere in the
# chain fails closed.
current = root
for part in parts[:-1]:
    current = current / part
    info = current.lstat()
    if (not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode)
            or info.st_uid != os.getuid()):
        raise SystemExit(
            f'installed-functional-evidence: unsafe ancestor {current}'
        )
    if current in (root / parts[0], root / parts[0] / parts[1], path.parent):
        if stat.S_IMODE(info.st_mode) != 0o700:
            raise SystemExit(
                f'installed-functional-evidence: unsafe override directory {current}'
            )
print(path)
PY
) || exit $?
[[ ! -L $evidence && -f $evidence ]] || {
    echo "installed-functional-evidence: missing or unsafe; run ./scripts/verify-project.sh" >&2
    exit 1
}
# Evidence-file invariants (Task 34/35): the target must be a current-user
# owned regular single-link file with safe mode 0600.  A symlink, hardlink,
# wrong owner, or loose mode fails closed.
python3 - "$evidence" <<'PY'
import os, stat, sys
from pathlib import Path
path = Path(sys.argv[1])
info = path.lstat()
if (stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode)
        or info.st_uid != os.getuid() or info.st_nlink != 1
        or stat.S_IMODE(info.st_mode) != 0o600):
    raise SystemExit(
        'installed-functional-evidence: evidence file is not a '
        'current-user-owned single-link regular file with mode 0600'
    )
PY
if [[ $? -ne 0 ]]; then
    exit 1
fi
mapfile -d '' -t EVIDENCE_FIELDS < <(python3 - "$evidence" <<'PY'
import os, re, sys
from pathlib import Path
expected = {'schema', 'commit', 'test', 'result', 'skipped'}
values = {}
for line in Path(sys.argv[1]).read_text(encoding='utf-8').splitlines():
    if not line or '=' not in line:
        raise SystemExit(1)
    key, value = line.split('=', 1)
    if key not in expected or key in values or not value:
        raise SystemExit(1)
    values[key] = value
if set(values) != expected:
    raise SystemExit(1)
checks = {
    'schema': 'factory-installed-functional/v1',
    'test': 'test_installed_functional',
    'result': 'PASS',
    'skipped': '0',
}
if any(values[key] != value for key, value in checks.items()) or not re.fullmatch(r'[0-9a-f]{40}', values['commit']):
    raise SystemExit(1)
for key in ('schema', 'commit', 'test', 'result', 'skipped'):
    os.write(1, values[key].encode() + b'\0')
PY
)
if (( ${#EVIDENCE_FIELDS[@]} != 5 )); then
    echo "installed-functional-evidence: malformed or non-passing evidence" >&2
    exit 1
fi
commit=${EVIDENCE_FIELDS[1]}
git merge-base --is-ancestor "$commit" HEAD || {
    echo "installed-functional-evidence: tested commit is not an ancestor of HEAD" >&2
    exit 1
}
if ! git diff --quiet "$commit" -- CMakeLists.txt config.h.in data packaging src tests \
        scripts/verify-project.sh; then
    echo "installed-functional-evidence: production or acceptance inputs changed since $commit" >&2
    exit 1
fi
echo "installed-functional-evidence: PASS at $commit with zero skips"
