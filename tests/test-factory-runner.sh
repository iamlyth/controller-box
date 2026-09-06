#!/usr/bin/env bash
# Disposable v2 runner trust-boundary checks. No external runner is contacted.
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
python3 "$ROOT/tests/test-runner-authority.py"
python3 "$ROOT/tests/test-runner-artifacts.py"
python3 "$ROOT/tests/test-broker-security.py"
python3 "$ROOT/tests/test-runner-client-security.py"
pycache=$(mktemp -d)
trap 'rm -rf -- "$pycache"' EXIT HUP INT TERM
PYTHONPYCACHEPREFIX="$pycache" python3 -m py_compile "$ROOT/scripts/factory-runner-broker.py" "$ROOT/scripts/factory-runner-server.py" "$ROOT/scripts/factory_runner_authority.py" "$ROOT/scripts/run-factory-runners.py" "$ROOT/scripts/check-factory-runner-evidence.py"
bash -n "$ROOT/scripts/install-factory-runner-v2.sh" "$ROOT/deploy/factory-runner-authority-v1/probe-controller-production-routing.sh" "$ROOT/deploy/factory-runner-authority-v1/probe-gpu-compositor.sh"
# The checked-in enrollment is explicitly pending and exactly matches the
# reproducible authority manifest. It must never promote itself.
python3 - "$ROOT" <<'PY'
import hashlib,json,pathlib,sys
r=pathlib.Path(sys.argv[1]); e=json.loads((r/'.factory/runner-policy-enrollment.json').read_text()); raw=(r/'deploy/factory-runner-authority-v1/authority.json').read_bytes(); d=hashlib.sha256(raw).hexdigest()
assert e['schema']=='controller-box-runner-policy-enrollment/v3' and e['status']=='pending-human-review'
assert e['host_executable_enrollment']['status']=='pending-root-install'
assert all(x['authority_sha256']==d and x['status']=='pending-root-install' for x in e['probe_authorities'].values())
PY
echo 'test: factory runner v2 broker boundary passed'
