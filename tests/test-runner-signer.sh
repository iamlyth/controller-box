#!/usr/bin/env bash
# Direct signer/fabricated-manifest adversarial regression.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
python3 - "$ROOT" <<'PY'
import importlib.util,json,pathlib,sys
root=pathlib.Path(sys.argv[1]); p=root/'scripts/factory-runner-signer.py'
spec=importlib.util.spec_from_file_location('runner_signer',p); m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
manifest={'schema':'factory-runner-receipt/v3','result':'pass'}
raw=(json.dumps({'schema':'factory-runner-sign-request/v1','manifest':manifest})+'\n').encode()
try: m.validate_manifest(raw,{'name':'fake','allowed_capabilities':['x']})
except SystemExit as exc:
 assert exc.code == 1
else: raise AssertionError('direct signer oracle accepted a fabricated manifest')
server=(root/'scripts/factory-runner-server.py').read_text()
assert 'factory-runner-signer' not in server
install=(root/'scripts/install-factory-runner-v2.sh').read_text()
assert '/bin/rm -f /etc/sudoers.d/factory-runner-signer' in install
assert 'pwd.getpwuid' in install and 'factory-runner-v2.bundle' in install
assert 'trap rollback EXIT INT TERM HUP' in install
assert 'FACTORY_BROKER_SIGNING' not in (root/'scripts/factory-runner-signer.py').read_text()
print('test: direct signer and fabricated manifest rejected')
PY
