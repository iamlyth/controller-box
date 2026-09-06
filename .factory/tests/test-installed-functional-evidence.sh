#!/usr/bin/env bash
set -euo pipefail
# The verifier may export FACTORY_INSTALLED_FUNCTIONAL_EVIDENCE_PATH for its
# own campaign publication; this test owns its fixture and must never inherit
# a foreign override (it would escape the fixture repository).
unset FACTORY_INSTALLED_FUNCTIONAL_EVIDENCE_PATH
source_root=${1:-$PWD}
root=$(mktemp -d)
sentinel=$(mktemp /tmp/controller-box-evidence-injection.XXXXXX)
rm -f -- "$sentinel"
trap 'rm -rf "$root"; rm -f -- "$sentinel"' EXIT
cd "$root"
git init -q
git config user.name test
git config user.email test@example.invalid
mkdir -p scripts .factory/tools .factory-state src tests data packaging
cp "$source_root/.factory/tools/check-installed-functional-evidence.sh" .factory/tools/
cp "$source_root/scripts/check-installed-functional-evidence.sh" scripts/
printf 'source\n' > src/app.c
printf 'cmake\n' > CMakeLists.txt
printf 'config\n' > config.h.in
printf 'verify\n' > scripts/verify-project.sh
git add .
git commit -qm baseline
commit=$(git rev-parse HEAD)
cat > .factory-state/installed-functional-evidence.env <<EOF
schema=factory-installed-functional/v1
commit=$commit
test=test_installed_functional
result=PASS
skipped=0
EOF
chmod 600 .factory-state/installed-functional-evidence.env
./scripts/check-installed-functional-evidence.sh >/dev/null
printf 'changed\n' >> src/app.c
if ./scripts/check-installed-functional-evidence.sh >/dev/null 2>&1; then
    echo 'evidence guard accepted changed production input' >&2
    exit 1
fi
git checkout -q -- src/app.c
sed -i 's/skipped=0/skipped=1/' .factory-state/installed-functional-evidence.env
if ./scripts/check-installed-functional-evidence.sh >/dev/null 2>&1; then
    echo 'evidence guard accepted skipped functional test' >&2
    exit 1
fi
# Reject wrong result
sed -i 's/skipped=1/skipped=0/' .factory-state/installed-functional-evidence.env
sed -i 's/result=PASS/result=FAIL/' .factory-state/installed-functional-evidence.env
if ./scripts/check-installed-functional-evidence.sh >/dev/null 2>&1; then
    echo 'evidence guard accepted failing result' >&2
    exit 1
fi
# Reject wrong test name
sed -i 's/result=FAIL/result=PASS/' .factory-state/installed-functional-evidence.env
sed -i 's/test=test_installed_functional/test=test_other/' .factory-state/installed-functional-evidence.env
if ./scripts/check-installed-functional-evidence.sh >/dev/null 2>&1; then
    echo 'evidence guard accepted wrong test name' >&2
    exit 1
fi
# Reject missing schema
sed -i 's/test=test_other/test=test_installed_functional/' .factory-state/installed-functional-evidence.env
sed -i 's/schema=factory-installed-functional\/v1/schema=other/' .factory-state/installed-functional-evidence.env
if ./scripts/check-installed-functional-evidence.sh >/dev/null 2>&1; then
    echo 'evidence guard accepted wrong schema' >&2
    exit 1
fi
# Evidence is parsed as data, never sourced as shell code.
cat > .factory-state/installed-functional-evidence.env <<EOF
schema=factory-installed-functional/v1
commit=$commit
test=test_installed_functional
result=PASS
skipped=0
touch -- '$sentinel'
EOF
chmod 600 .factory-state/installed-functional-evidence.env
if ./scripts/check-installed-functional-evidence.sh >/dev/null 2>&1 || [[ -e "$sentinel" ]]; then
    echo 'evidence guard accepted or executed injected shell content' >&2
    exit 1
fi
# An explicit override is accepted only at the exact private campaign shape;
# an outside path fails closed. The default root evidence remains untouched.
mkdir -p .factory-state/campaigns/fixture-campaign
chmod 700 .factory-state .factory-state/campaigns .factory-state/campaigns/fixture-campaign
cat > .factory-state/installed-functional-evidence.env <<EOF
schema=factory-installed-functional/v1
commit=$commit
test=test_installed_functional
result=PASS
skipped=0
EOF
chmod 600 .factory-state/installed-functional-evidence.env
cp .factory-state/installed-functional-evidence.env \
   .factory-state/campaigns/fixture-campaign/installed-functional-evidence.env
chmod 600 .factory-state/campaigns/fixture-campaign/installed-functional-evidence.env
FACTORY_INSTALLED_FUNCTIONAL_EVIDENCE_PATH="$root/.factory-state/campaigns/fixture-campaign/installed-functional-evidence.env" \
    ./scripts/check-installed-functional-evidence.sh >/dev/null
if FACTORY_INSTALLED_FUNCTIONAL_EVIDENCE_PATH="$sentinel" \
        ./scripts/check-installed-functional-evidence.sh >/dev/null 2>&1; then
    echo 'evidence guard accepted an override outside the campaign namespace' >&2
    exit 1
fi
# Reject symlinked and missing evidence files entirely.
rm .factory-state/installed-functional-evidence.env
ln -s /dev/null .factory-state/installed-functional-evidence.env
if ./scripts/check-installed-functional-evidence.sh >/dev/null 2>&1; then
    echo 'evidence guard accepted symlinked evidence file' >&2
    exit 1
fi
rm .factory-state/installed-functional-evidence.env
if ./scripts/check-installed-functional-evidence.sh >/dev/null 2>&1; then
    echo 'evidence guard accepted missing evidence file' >&2
    exit 1
fi
# Atomic no-replace publication (Task 34/35): a second publication to an
# existing destination must fail without overwriting the sentinel.
cat > .factory-state/installed-functional-evidence.env <<EOF
schema=factory-installed-functional/v1
commit=$commit
test=test_installed_functional
result=PASS
skipped=0
EOF
chmod 600 .factory-state/installed-functional-evidence.env
if ! python3 - "$root" "$root/.factory-state/installed-functional-evidence.env" "$commit" <<'PY'
import os, stat, sys, tempfile
from pathlib import Path
root, target, commit = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
body = (
    'schema=factory-installed-functional/v1\n'
    f'commit={commit}\n'
    'test=test_installed_functional\n'
    'result=PASS\n'
    'skipped=0\n'
).encode()
fd, temporary = tempfile.mkstemp(prefix='.installed-functional-evidence.', dir=target.parent)
try:
    with os.fdopen(fd, 'wb') as stream:
        stream.write(body)
        stream.flush()
        os.fsync(stream.fileno())
    os.chmod(temporary, 0o600)
    try:
        os.link(temporary, target)
    except FileExistsError:
        raise SystemExit(0)  # atomic no-replace refused the overwrite
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
    raise SystemExit(1)  # the second publication must not succeed
finally:
    try:
        os.unlink(temporary)
    except FileNotFoundError:
        pass
PY
then
    echo 'evidence publication overwrote an existing destination' >&2
    exit 1
fi
# The sentinel survived byte-identically.
grep -q '^result=PASS$' .factory-state/installed-functional-evidence.env
echo 'installed-functional-evidence tests passed'
