#!/usr/bin/env bash
# Root deployment/migration for the v2 privileged broker. Does not enroll policy.
set -euo pipefail
[[ $(/usr/bin/id -u) -eq 0 && $# -eq 1 ]] || { echo 'usage: sudo install-factory-runner-v2.sh APPROVED-POLICY.json' >&2; exit 2; }
ROOT=$(cd -- "$(/usr/bin/dirname -- "${BASH_SOURCE[0]}")/.." && /bin/pwd)
POLICY=$1
/usr/bin/python3 - "$POLICY" <<'PY'
import json,os,stat,sys
p=sys.argv[1]; i=os.lstat(p); d=json.load(open(p))
assert d['schema']=='factory-runner-policy/v2' and i.st_uid==0 and not i.st_mode&0o022
PY
AUTH="$ROOT/deploy/factory-runner-authority-v1"
EXPECTED=$(/usr/bin/python3 - "$ROOT/.factory/runner-policy-enrollment.json" <<'PY'
import json,sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["probe_authorities"]["iprunner"]["authority_sha256"])
PY
)
ACTUAL=$(/usr/bin/sha256sum "$AUTH/authority.json" | /usr/bin/cut -d' ' -f1)
[[ "$ACTUAL" == "$EXPECTED" ]] || { echo 'probe authority digest differs from enrollment request' >&2; exit 1; }
/bin/rm -rf /opt/factory-runner/authority/v1.new
/bin/mkdir -p /opt/factory-runner/authority/v1.new /usr/local/libexec /etc/factory-runner
/bin/cp -a "$AUTH/." /opt/factory-runner/authority/v1.new/
/usr/bin/chown -R root:root /opt/factory-runner/authority/v1.new
/usr/bin/chmod -R go-w /opt/factory-runner/authority/v1.new
/bin/mv /opt/factory-runner/authority/v1.new /opt/factory-runner/authority/v1
/bin/cp "$ROOT/scripts/factory-runner-broker.py" /usr/local/libexec/factory-runner-broker
/bin/cp "$ROOT/scripts/factory-runner-signer.py" /usr/local/libexec/factory-runner-signer
/bin/cp "$ROOT/scripts/factory-runner-server.py" /usr/local/libexec/factory-runner-server
/bin/cp "$ROOT/scripts/factory_runner_policy.py" "$ROOT/scripts/factory_runner_artifacts.py" "$ROOT/scripts/factory_runner_authority.py" /usr/local/libexec/
/usr/bin/chown root:root /usr/local/libexec/factory-runner-* /usr/local/libexec/factory_runner_*.py
/usr/bin/chmod 0700 /usr/local/libexec/factory-runner-broker /usr/local/libexec/factory-runner-signer
/usr/bin/chmod 0755 /usr/local/libexec/factory-runner-server
/bin/cp "$POLICY" /etc/factory-runner/runner-policy.json
/usr/bin/chown root:root /etc/factory-runner/runner-policy.json
/usr/bin/chmod 0640 /etc/factory-runner/runner-policy.json
/usr/bin/python3 - "$POLICY" <<'PY' >/etc/sudoers.d/factory-runner-broker
import json,sys
for c in json.load(open(sys.argv[1]))['classes']:
 print(f"{c['name']} ALL=(root) NOPASSWD: /usr/local/libexec/factory-runner-broker")
PY
/usr/bin/chown root:root /etc/sudoers.d/factory-runner-broker
/usr/bin/chmod 0440 /etc/sudoers.d/factory-runner-broker
/usr/sbin/visudo -cf /etc/sudoers.d/factory-runner-broker
# Remove the obsolete direct signer grant if present.  authorized_keys must be
# migrated to the exact command shown in deploy/factory-runner-authority-v1/forced-command-v2.txt.
/bin/rm -f /etc/sudoers.d/factory-runner-signer
