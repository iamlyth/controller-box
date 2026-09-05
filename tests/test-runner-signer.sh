#!/usr/bin/env bash
# Adversarial runner-receipt signer verification: capability evidence that
# requires runner trust must reject unsigned legacy/local manifests, fabricated
# signatures, unknown or rotated keys, wrong namespaces/principals, stale
# commit bindings, and failure/skip receipts. The root-owned signer (a separate
# out-of-tree helper) never signs caller-provided bytes: it rebuilds and signs
# only manifests whose fields prove a clean pass, and it fails closed on
# unsafe/absent private-key state. Public keys/config live in the repository;
# private signing stays out-of-tree.
set -euo pipefail
export FACTORY_CAMPAIGN_ID=synthetic-signer-protocol
export FACTORY_READINESS_NONCE=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

CHECKER="$PROJECT_ROOT/scripts/check-factory-runner-evidence.py"
SIGNER="$PROJECT_ROOT/scripts/factory-runner-signer.py"

# Ephemeral test keys (never committed; private keys are out-of-tree).
ssh-keygen -q -t ed25519 -N '' -f "$tmp/signer-key"
PUBLIC_KEY=$(cut -d' ' -f1,2 "$tmp/signer-key.pub")
ssh-keygen -q -t ed25519 -N '' -f "$tmp/other-key"
OTHER_PUBLIC_KEY=$(cut -d' ' -f1,2 "$tmp/other-key.pub")

PUBKEY_SHA256=$(python3 - "$PUBLIC_KEY" <<'PY'
import hashlib, sys
print(hashlib.sha256(sys.argv[1].encode()).hexdigest())
PY
)

trust_json() {
    python3 - "$@" <<'PY'
import json, sys
print(json.dumps({
    "schema": "ralph-runner-signer-trust/v1",
    "description": "test fixture",
    "require_signature": True,
    "enabled": json.loads(sys.argv[3]),
    "namespace": "factory-runner-receipt",
    "public_keys": json.loads(sys.argv[1]),
    "allowed_principals": json.loads(sys.argv[2]),
}))
PY
}

DISABLED=$(trust_json '[]' '[]' false)
ENABLED=$(trust_json "[{\"principal\":\"fake-runner\",\"public_key\":\"$PUBLIC_KEY\"}]" '["fake-runner"]' true)
UNKNOWN=$(trust_json "[{\"principal\":\"fake-runner\",\"public_key\":\"$OTHER_PUBLIC_KEY\"}]" '["fake-runner"]' true)
WRONG_PRINCIPAL=$ENABLED
ROTATED_BOTH=$(trust_json "[{\"principal\":\"fake-runner\",\"public_key\":\"$PUBLIC_KEY\"},{\"principal\":\"fake-runner\",\"public_key\":\"$OTHER_PUBLIC_KEY\"}]" '["fake-runner"]' true)

setup_repo() {
    local dir=$1 trust=$2
    mkdir -p "$dir/scripts" "$dir/docs" "$dir/.factory" "$dir/.factory-state"
    cp "$CHECKER" "$PROJECT_ROOT/scripts/factory_runner_artifacts.py" "$dir/scripts/"
    cp "$PROJECT_ROOT/scripts/check-factory-environment.py" "$dir/scripts/"
    chmod +x "$dir/scripts/"*.py
    cat > "$dir/.factory/environment.toml" <<'EOF'
schema_version = 1
[[runners]]
name = "fake-runner"
transport = "ssh"
ssh_config_alias = "fake-runner"
working_directory = "/srv/dev-runner/workspaces/fake-project"
capabilities = ["remote-project-gate"]
verify_argv = ["./scripts/verify-project.sh"]
EOF
    printf '# Spec\n' > "$dir/docs/SPEC.md"
    printf '%s\n' ".factory-state/" > "$dir/.gitignore"
    printf '%s\n' "$trust" > "$dir/.factory/signer-trust.json"
    git -C "$dir" init -q -b develop
    git -C "$dir" config user.name test
    git -C "$dir" config user.email test@example.invalid
    git -C "$dir" add .
    git -C "$dir" commit -qm base
}

# write_evidence <dir> <head>: a manifest with the exact commit-bound bindings
# the checker recomputes (tree, environment blob, verifier argv digest, git
# archive hash) plus the signer identity fields the root signer would bind, so
# signature/principal/namespace/rotation are the variables under test.
write_evidence() {
    local dir=$1 head=$2
    mkdir -p "$dir/.factory-state/runner-evidence/fake-runner/$head"
    python3 - "$dir" "$head" "$PUBKEY_SHA256" <<'PY'
import hashlib, json, pathlib, subprocess, sys
root, head, key_sha256 = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]

def git(*args: str) -> str:
    result = subprocess.run(["git", *args], cwd=root, text=True, capture_output=True)
    if result.returncode:
        raise SystemExit(f"git {args} failed: {result.stderr}")
    return result.stdout.strip()

tree = git("rev-parse", f"{head}^{{tree}}")
environment_blob = git("rev-parse", f"{head}:.factory/environment.toml")
argv_digest = hashlib.sha256(json.dumps(["./scripts/verify-project.sh"], separators=(",", ":")).encode()).hexdigest()
archive = subprocess.run(
    ["git", "archive", "--format=tar", "--output", str(root / "commit-archive.tar"), head],
    cwd=root, capture_output=True,
)
if archive.returncode:
    raise SystemExit("cannot archive fixture commit")
archive_sha256 = hashlib.sha256((root / "commit-archive.tar").read_bytes()).hexdigest()
(root / "commit-archive.tar").unlink()
empty = hashlib.sha256(b"").hexdigest()
manifest = {
    "schema": "factory-runner-receipt/v2", "result": "pass", "runner": "fake-runner",
    "commit": head, "tree": tree, "environment_blob": environment_blob,
    "verify_argv_sha256": argv_digest, "archive_sha256": archive_sha256,
    "campaign_id": "synthetic-signer-protocol", "readiness_nonce": "b" * 64, "authority_pins_sha256": "4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945", "nonce": "0" * 64,
    "capabilities": ["remote-project-gate"], "exit_code": 0, "timed_out": False,
    "started_at": 1, "finished_at": 2, "cleanup": True,
    "stdout_sha256": empty, "stderr_sha256": empty,
    "artifact_protocol":"factory-runner-artifacts/v1",
    "artifact_limits":{"count":64,"file_bytes":8388608,"aggregate_bytes":50331648},
    "artifact_count":0,"artifact_bytes":0,
    "artifact_manifest_sha256":hashlib.sha256(b"[]\n").hexdigest(),
    "artifact_scope_sha256":hashlib.sha256(json.dumps({"campaign_id":"synthetic-signer-protocol","readiness_nonce":"b"*64,"nonce":"0"*64,"artifact_manifest_sha256":hashlib.sha256(b"[]\n").hexdigest()},sort_keys=True,separators=(",",":")).encode()).hexdigest(),"artifacts":[],
    "signer_principal": "fake-runner", "signer_key_sha256": key_sha256,
    "namespace": "factory-runner-receipt", "signature_algorithm": "ssh-ed25519",
}
raw = (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode()
manifest_path = root / f".factory-state/runner-evidence/fake-runner/{head}/manifest.json"
manifest_path.write_bytes(raw)
aggregate = {
    "schema": "factory-runner-aggregate/v3",
    "campaign_id": "synthetic-signer-protocol",
    "readiness_nonce": "b" * 64,
    "commit": head,
    "tree": tree,
    "environment_blob": environment_blob,
    "runners": [
        {"name": "fake-runner", "manifest": f".factory-state/runner-evidence/fake-runner/{head}/manifest.json",
         "manifest_sha256": hashlib.sha256(raw).hexdigest(), "capabilities": ["remote-project-gate"],
         "artifact_manifest_sha256":hashlib.sha256(b"[]\n").hexdigest(),"artifact_count":0,"artifact_bytes":0,
         "signer": {"principal": "fake-runner", "key_sha256": key_sha256,
                    "algorithm": "ssh-ed25519", "signature_sha256": ""}}
    ],
}
(root / ".factory-state/runner-evidence.json").write_text(json.dumps(aggregate, sort_keys=True, indent=2) + "\n")
(root / f".factory-state/runner-evidence/fake-runner/{head}/stdout.log").write_bytes(b"")
(root / f".factory-state/runner-evidence/fake-runner/{head}/stderr.log").write_bytes(b"")
PY
}

# sign <dir> <head> <key> [namespace]: sign the manifest and register the
# detached signature (and its digest) in the aggregate metadata.
sign() {
    local dir=$1 head=$2 key=$3 namespace=${4:-factory-runner-receipt}
    local sig="$dir/.factory-state/runner-evidence/fake-runner/$head/manifest.sig"
    cat "$dir/.factory-state/runner-evidence/fake-runner/$head/manifest.json" \
        | ssh-keygen -Y sign -f "$key" -n "$namespace" \
            > "$sig" 2>/dev/null
    python3 - "$dir/.factory-state/runner-evidence.json" "$sig" <<'PY'
import hashlib, json, pathlib, sys
aggregate_path, sig_path = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
aggregate = json.loads(aggregate_path.read_text())
aggregate["runners"][0]["signer"]["signature_sha256"] = hashlib.sha256(sig_path.read_bytes()).hexdigest()
aggregate_path.write_text(json.dumps(aggregate, sort_keys=True, indent=2) + "\n")
PY
}

expect() {
    local dir=$1 expected=$2 label=$3 expected_commit=${4:-}
    local args=()
    [[ -z $expected_commit ]] || args=(--expected-commit "$expected_commit")
    set +e
    (cd "$dir" && ./scripts/check-factory-runner-evidence.py "${args[@]}" >/dev/null 2>&1)
    local rc=$?
    set -e
    [[ $rc -eq $expected ]] || {
        echo "test: runner-signer $label (expected rc=$expected, got rc=$rc)" >&2
        exit 1
    }
}

# --- Checker adversarial cases -------------------------------------------------

# Unsigned legacy/local manifest with no signer provisioned: rejected.
setup_repo "$tmp/unsigned" "$DISABLED"
head=$(git -C "$tmp/unsigned" rev-parse HEAD)
write_evidence "$tmp/unsigned" "$head"
expect "$tmp/unsigned" 1 "unsigned manifest without a provisioned signer"

# Unsigned manifest while a signer IS provisioned: missing signature rejected.
setup_repo "$tmp/missing-sig" "$ENABLED"
head=$(git -C "$tmp/missing-sig" rev-parse HEAD)
write_evidence "$tmp/missing-sig" "$head"
expect "$tmp/missing-sig" 1 "unsigned manifest with signer provisioned"

# Fabricated signature (garbage .sig): rejected.
setup_repo "$tmp/fabricated" "$ENABLED"
head=$(git -C "$tmp/fabricated" rev-parse HEAD)
write_evidence "$tmp/fabricated" "$head"
printf 'not-a-real-signature\n' > "$tmp/fabricated/.factory-state/runner-evidence/fake-runner/$head/manifest.sig"
python3 - "$tmp/fabricated/.factory-state/runner-evidence.json" <<'PY'
import hashlib, json, pathlib, sys
path = pathlib.Path(sys.argv[1])
aggregate = json.loads(path.read_text())
aggregate["runners"][0]["signer"]["signature_sha256"] = hashlib.sha256(b"not-a-real-signature\n").hexdigest()
path.write_text(json.dumps(aggregate, sort_keys=True, indent=2) + "\n")
PY
expect "$tmp/fabricated" 1 "fabricated signature"

# A signature from an unknown key (not in the trust store): rejected.
setup_repo "$tmp/unknown-key" "$UNKNOWN"
head=$(git -C "$tmp/unknown-key" rev-parse HEAD)
write_evidence "$tmp/unknown-key" "$head"
sign "$tmp/unknown-key" "$head" "$tmp/other-key"
expect "$tmp/unknown-key" 1 "signature from an unknown key"

# A manifest claiming a foreign signer key digest: rejected even with a valid
# signature from the provisioned key.
setup_repo "$tmp/wrong-key-digest" "$ENABLED"
head=$(git -C "$tmp/wrong-key-digest" rev-parse HEAD)
write_evidence "$tmp/wrong-key-digest" "$head"
sign "$tmp/wrong-key-digest" "$head" "$tmp/signer-key"
python3 - "$tmp/wrong-key-digest" "$head" <<'PY'
import json, pathlib, sys
root, head = pathlib.Path(sys.argv[1]), sys.argv[2]
manifest_path = root / f".factory-state/runner-evidence/fake-runner/{head}/manifest.json"
aggregate_path = root / ".factory-state/runner-evidence.json"
manifest = json.loads(manifest_path.read_text())
manifest["signer_key_sha256"] = "1" * 64
manifest_path.write_text(json.dumps(manifest, sort_keys=True, indent=2) + "\n")
aggregate = json.loads(aggregate_path.read_text())
aggregate["runners"][0]["signer"]["key_sha256"] = "1" * 64
aggregate_path.write_text(json.dumps(aggregate, sort_keys=True, indent=2) + "\n")
PY
expect "$tmp/wrong-key-digest" 1 "wrong signer key digest"

# A manifest signed by the provisioned key but claiming a wrong principal:
# rejected (principal binding).
setup_repo "$tmp/wrong-principal" "$WRONG_PRINCIPAL"
head=$(git -C "$tmp/wrong-principal" rev-parse HEAD)
write_evidence "$tmp/wrong-principal" "$head"
sign "$tmp/wrong-principal" "$head" "$tmp/signer-key"
python3 - "$tmp/wrong-principal" "$head" <<'PY'
import json, pathlib, sys
root, head = pathlib.Path(sys.argv[1]), sys.argv[2]
manifest_path = root / f".factory-state/runner-evidence/fake-runner/{head}/manifest.json"
aggregate_path = root / ".factory-state/runner-evidence.json"
manifest = json.loads(manifest_path.read_text())
manifest["signer_principal"] = "other-signer"
manifest_path.write_text(json.dumps(manifest, sort_keys=True, indent=2) + "\n")
aggregate = json.loads(aggregate_path.read_text())
aggregate["runners"][0]["signer"]["principal"] = "other-signer"
aggregate_path.write_text(json.dumps(aggregate, sort_keys=True, indent=2) + "\n")
PY
expect "$tmp/wrong-principal" 1 "wrong signer principal"

# A signature over the wrong namespace: rejected.
setup_repo "$tmp/wrong-namespace" "$ENABLED"
head=$(git -C "$tmp/wrong-namespace" rev-parse HEAD)
write_evidence "$tmp/wrong-namespace" "$head"
sign "$tmp/wrong-namespace" "$head" "$tmp/signer-key" other-namespace
expect "$tmp/wrong-namespace" 1 "wrong signature namespace"

# Tampering the manifest after signing invalidates the signature.
setup_repo "$tmp/tampered" "$ENABLED"
head=$(git -C "$tmp/tampered" rev-parse HEAD)
write_evidence "$tmp/tampered" "$head"
sign "$tmp/tampered" "$head" "$tmp/signer-key"
printf 'tamper\n' >> "$tmp/tampered/.factory-state/runner-evidence/fake-runner/$head/manifest.json"
expect "$tmp/tampered" 1 "tampered manifest after signing"

# A manifest bound to a stale commit (the aggregate claims a newer commit while
# the manifest still names the old one): rejected.
setup_repo "$tmp/stale-commit" "$ENABLED"
head=$(git -C "$tmp/stale-commit" rev-parse HEAD)
write_evidence "$tmp/stale-commit" "$head"
sign "$tmp/stale-commit" "$head" "$tmp/signer-key"
printf 'change\n' > "$tmp/stale-commit/extra.txt"
git -C "$tmp/stale-commit" add extra.txt
git -C "$tmp/stale-commit" commit -qm newer-commit
newhead=$(git -C "$tmp/stale-commit" rev-parse HEAD)
python3 - "$tmp/stale-commit" "$newhead" <<'PY'
import json, pathlib, subprocess, sys
root, newhead = pathlib.Path(sys.argv[1]), sys.argv[2]
tree = subprocess.run(["git", "rev-parse", f"{newhead}^{{tree}}"], cwd=root,
                      capture_output=True, text=True).stdout.strip()
environment_blob = subprocess.run(["git", "rev-parse", f"{newhead}:.factory/environment.toml"], cwd=root,
                                  capture_output=True, text=True).stdout.strip()
path = root / ".factory-state/runner-evidence.json"
aggregate = json.loads(path.read_text())
aggregate["commit"] = newhead
aggregate["tree"] = tree
aggregate["environment_blob"] = environment_blob
path.write_text(json.dumps(aggregate, sort_keys=True, indent=2) + "\n")
PY
expect "$tmp/stale-commit" 1 "stale-commit manifest"

# Failure/skip receipts can never be evidence: result=fail, timed out, and
# cleanup=false manifests are all rejected even when signed.
for variant in fail-result timed-out cleanup; do
    setup_repo "$tmp/$variant" "$ENABLED"
    head=$(git -C "$tmp/$variant" rev-parse HEAD)
    write_evidence "$tmp/$variant" "$head"
    sign "$tmp/$variant" "$head" "$tmp/signer-key"
    python3 - "$tmp/$variant" "$head" "$variant" <<'PY'
import json, pathlib, sys
root, head, variant = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
path = root / f".factory-state/runner-evidence/fake-runner/{head}/manifest.json"
manifest = json.loads(path.read_text())
if variant == "fail-result":
    manifest["result"] = "fail"
elif variant == "timed-out":
    manifest["timed_out"] = True
else:
    manifest["cleanup"] = False
path.write_text(json.dumps(manifest, sort_keys=True, indent=2) + "\n")
PY
    expect "$tmp/$variant" 1 "failure/skip variant $variant cannot be evidence"
done

# Schema v1 has no explicit rotation window, so multiple keys for one
# principal are ambiguous and rejected. A later committed revocation also
# rejects a historical receipt whose issuance commit trusted the old key.
setup_repo "$tmp/rotation-old" "$ROTATED_BOTH"
head=$(git -C "$tmp/rotation-old" rev-parse HEAD)
write_evidence "$tmp/rotation-old" "$head"
sign "$tmp/rotation-old" "$head" "$tmp/signer-key"
expect "$tmp/rotation-old" 1 "ambiguous rotation is rejected"
setup_repo "$tmp/revoked" "$ENABLED"
head=$(git -C "$tmp/revoked" rev-parse HEAD)
write_evidence "$tmp/revoked" "$head"
sign "$tmp/revoked" "$head" "$tmp/signer-key"
python3 - "$tmp/revoked/.factory/signer-trust.json" "$OTHER_PUBLIC_KEY" <<'PY'
import json, pathlib, sys
path = pathlib.Path(sys.argv[1]); trust = json.loads(path.read_text())
trust["public_keys"] = [{"principal": "fake-runner", "public_key": sys.argv[2]}]
path.write_text(json.dumps(trust, sort_keys=True, indent=2) + "\n")
PY
git -C "$tmp/revoked" add .factory/signer-trust.json
git -C "$tmp/revoked" commit -qm revoke-old-key
expect "$tmp/revoked" 1 "committed revocation rejects historical evidence" "$head"

# Dirty worktree trust injection is not authority: both issuance and current
# trust are loaded from commits, so adding a key only to the checkout fails.
setup_repo "$tmp/uncommitted-injection" "$DISABLED"
head=$(git -C "$tmp/uncommitted-injection" rev-parse HEAD)
printf '%s\n' "$ENABLED" > "$tmp/uncommitted-injection/.factory/signer-trust.json"
write_evidence "$tmp/uncommitted-injection" "$head"
sign "$tmp/uncommitted-injection" "$head" "$tmp/signer-key"
expect "$tmp/uncommitted-injection" 1 "uncommitted trust injection"

# Strict trust parser regressions, including field, principal, key, duplicate,
# cross-principal, unsupported algorithm, malformed wire, and ambiguous v1
# rotation states.
python3 - "$CHECKER" "$PUBLIC_KEY" "$OTHER_PUBLIC_KEY" <<'PY'
import copy, importlib.util, json, sys
path, public, other = sys.argv[1:]
spec = importlib.util.spec_from_file_location("checker", path)
mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
base = {"schema":"ralph-runner-signer-trust/v1", "description":"fixture",
        "require_signature":True, "enabled":True, "namespace":"factory-runner-receipt",
        "public_keys":[{"principal":"fake-runner","public_key":public}],
        "allowed_principals":["fake-runner"]}
def accepted(value):
    mod.git = lambda *args: json.dumps(value)
    try: mod.load_signer_trust("a" * 40, "test")
    except SystemExit: return False
    return True
assert accepted(base)
mod.git=lambda *args: '{"schema":"ralph-runner-signer-trust/v1","schema":"ralph-runner-signer-trust/v1"}'
try: mod.load_signer_trust("a"*40,"test")
except SystemExit: pass
else: raise AssertionError('duplicate JSON object field accepted')
variants=[]
def changed(fn):
    value=copy.deepcopy(base); fn(value); variants.append(value)
changed(lambda x: x.update(extra=True))
changed(lambda x: x["allowed_principals"].append("fake-runner"))
changed(lambda x: x["allowed_principals"].__setitem__(0, "Bad Name"))
changed(lambda x: x["public_keys"].append(copy.deepcopy(x["public_keys"][0])))
changed(lambda x: x["public_keys"].append({"principal":"fake-runner","public_key":other}))
changed(lambda x: x["public_keys"].clear())
changed(lambda x: x["allowed_principals"].append("orphan"))
changed(lambda x: x["public_keys"][0].update(extra="field"))
changed(lambda x: x["public_keys"][0].update(principal="orphan"))
changed(lambda x: x["public_keys"][0].update(public_key=public + " comment"))
changed(lambda x: x["public_keys"][0].update(public_key=" " + public))
changed(lambda x: x["public_keys"][0].update(public_key="ssh-rsa " + public.split(" ",1)[1]))
changed(lambda x: x["public_keys"][0].update(public_key="ssh-ed25519 !!!"))
changed(lambda x: x["public_keys"][0].update(public_key="ssh-ed25519 AAAA"))
def shared(x):
    x["allowed_principals"].append("other-runner")
    x["public_keys"].append({"principal":"other-runner","public_key":public})
changed(shared)
assert all(not accepted(value) for value in variants)
print("test: strict signer trust parser negatives passed")
PY

# The committed production trust has exactly one distinct canonical key for
# every declared class, including the exact enrolled production values.
python3 - "$PROJECT_ROOT" <<'PY'
import base64, hashlib, json, pathlib, tomllib, sys
root=pathlib.Path(sys.argv[1])
env=tomllib.loads((root/'.factory/environment.toml').read_text())
trust=json.loads((root/'.factory/signer-trust.json').read_text())
expected={
'dev-runner-vm':'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIH1aM5OOElPaP30GHR9P/hiz3lJkil/TIQi7lc3Wd8Su',
'iprunner':'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIEHWU31Sso29CXbjKE/erN1FOq3Dz0hx2cF6Mh497TXX',
'gpurunner':'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIKCkkRbD/mDyDA22vY/FJXeqflxhinFfPkknEWHGES9e'}
actual={entry['principal']:entry['public_key'] for entry in trust['public_keys']}
assert actual == expected
assert set(trust['allowed_principals']) == {runner['name'] for runner in env['runners']} == set(expected)
assert len(set(actual.values())) == len(expected)
fingerprints={name:'SHA256:'+base64.b64encode(hashlib.sha256(base64.b64decode(key.split()[1])).digest()).decode().rstrip('=') for name,key in actual.items()}
assert fingerprints['iprunner'] == 'SHA256:/hl+xb+EMEr2bHsij90I3IvTI7rGzEuxTLuvT1yhOOE'
assert fingerprints['gpurunner'] == 'SHA256:h+Zy8/y3kPv25eP2Ov3pkwuEG9Vl6IaHba7pIMvPTGg'
PY

# Valid signed manifest with the provisioned ephemeral key: accepted.
setup_repo "$tmp/signed" "$ENABLED"
head=$(git -C "$tmp/signed" rev-parse HEAD)
write_evidence "$tmp/signed" "$head"
sign "$tmp/signed" "$head" "$tmp/signer-key"
expect "$tmp/signed" 0 "valid signed runner manifest"

# --- Root-owned signer helper adversarial cases (never signs caller bytes) ---

helper_dir="$tmp/helper"
mkdir -p "$helper_dir"
cp "$SIGNER" "$helper_dir/factory-runner-signer.py"
cp "$PROJECT_ROOT/scripts/factory_runner_policy.py" "$PROJECT_ROOT/scripts/factory_runner_artifacts.py" "$helper_dir/"
chmod +x "$helper_dir/factory-runner-signer.py"
# The disposable copied harness substitutes its immutable tool-store path and
# test UID/boundary. Production has no environment override for either.
SSH_KEYGEN_BIN=$(command -v ssh-keygen)
python3 - "$helper_dir/factory-runner-signer.py" "$SSH_KEYGEN_BIN" "$tmp" <<'PY'
import pathlib, sys
path, keygen, boundary = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
text = text.replace('SSH_KEYGEN_PATH = "/usr/bin/ssh-keygen"', f'SSH_KEYGEN_PATH = {keygen!r}')
text = text.replace('ROOT_UID = 0', 'ROOT_UID = os.getuid()')
text = text.replace('STATE_CHAIN_BOUNDARY = Path("/")', f'STATE_CHAIN_BOUNDARY = Path({boundary!r})')
text = text.replace('VALIDATE_EXECUTABLE_CHAIN = True', 'VALIDATE_EXECUTABLE_CHAIN = False')
path.write_text(text)
PY

printf 'fake-runner\n' > "$tmp/signer-principal"
chmod 0600 "$tmp/signer-principal"
chmod 0600 "$tmp/signer-key"
# The signer's capability policy is root-configured: the harness installs a
# fixture policy whose class grants exactly the capability the valid request
# claims, so the signer enforces the class allowlist even in harness mode.
python3 - "$tmp" <<'PY' > "$tmp/policy.json"
import json, os, sys
print(json.dumps({
    "schema": "factory-runner-policy/v1",
    "namespace": "factory-runner-receipt",
    "authority_pins": [],
    "classes": [
        {
            "name": "fake-runner",
            "uid": os.getuid(),
            "workspace_root": "/srv/dev-runner/workspaces",
            "verify_argv": ["./scripts/verify-project.sh"],
            "allowed_capabilities": ["remote-project-gate"],
            "signer_helper": "/usr/local/libexec/factory-runner-signer",
            "signer_key": f"{sys.argv[1]}/signer-key",
            "signer_principal_file": f"{sys.argv[1]}/signer-principal",
        }
    ],
}))
PY

valid_request() {
    python3 - "$PUBKEY_SHA256" <<'PY'
import hashlib, json, sys
manifest = {
    "schema": "factory-runner-receipt/v2", "result": "pass", "runner": "fake-runner",
    "commit": "a" * 40, "tree": "b" * 40, "environment_blob": "c" * 40,
    "verify_argv_sha256": hashlib.sha256(b"x").hexdigest(),
    "archive_sha256": hashlib.sha256(b"y").hexdigest(),
    "campaign_id": "synthetic-signer-protocol", "readiness_nonce": "b" * 64,
    "authority_pins_sha256": "4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945", "nonce": "0" * 64,
    "capabilities": ["remote-project-gate"], "exit_code": 0, "timed_out": False,
    "started_at": 1, "finished_at": 2, "cleanup": True,
    "stdout_sha256": hashlib.sha256(b"").hexdigest(),
    "stderr_sha256": hashlib.sha256(b"").hexdigest(),
    "artifact_protocol":"factory-runner-artifacts/v1",
    "artifact_limits":{"count":64,"file_bytes":8388608,"aggregate_bytes":50331648},
    "artifact_count":0,"artifact_bytes":0,
    "artifact_manifest_sha256":hashlib.sha256(b"[]\n").hexdigest(),"artifacts":[],
}
manifest["artifact_scope_sha256"]=hashlib.sha256(json.dumps({"campaign_id":manifest["campaign_id"],"readiness_nonce":manifest["readiness_nonce"],"nonce":manifest["nonce"],"artifact_manifest_sha256":manifest["artifact_manifest_sha256"]},sort_keys=True,separators=(",",":")).encode()).hexdigest()
print(json.dumps({"schema": "factory-runner-sign-request/v1", "manifest": manifest}))
PY
}

helper_run() {
    local key_path=$1 principal_path=$2
    shift 2
    set +e
    FACTORY_SIGNER_KEY="$key_path" FACTORY_SIGNER_PRINCIPAL_FILE="$principal_path" \
        FACTORY_SIGNER_CLASS=fake-runner FACTORY_RUNNER_POLICY="$tmp/policy.json" \
        python3 -I "$helper_dir/factory-runner-signer.py" \
        >"$tmp/helper-out" 2>"$tmp/helper-err"
    local rc=$?
    set -e
    echo $rc
}

# A valid clean-pass request under a healthy key is signed; the returned
# detached signature verifies under the provisioned public key and the signed
# manifest carries the signer identity binding. The private key never appears
# in the response.
mkdir -p "$tmp/hostile-bin"
printf '#!/bin/sh\nexit 99\n' > "$tmp/hostile-bin/ssh-keygen"
chmod +x "$tmp/hostile-bin/ssh-keygen"
rc=$(valid_request | PATH="$tmp/hostile-bin:$PATH" helper_run "$tmp/signer-key" "$tmp/signer-principal")
[[ $rc -eq 0 ]] || { echo "test: runner-signer helper rejected a valid request or trusted hostile PATH (rc=$rc)" >&2; exit 1; }
# Numeric sudo identity selects policy UID, not class text or an unrelated
# account name. This covers devrunner -> dev-runner-vm and forged/mismatched/
# ambiguous identity failures without requiring those production accounts in
# the disposable test host NSS database.
python3 - "$helper_dir/factory-runner-signer.py" <<'PY'
import importlib.util, os, types, sys
path=sys.argv[1]; spec=importlib.util.spec_from_file_location('signer', path)
mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
classes=[{'name':'dev-runner-vm','uid':4101,'signer_key':'/k','signer_principal_file':'/p'}]
mod.load_policy=lambda: {'classes':classes}
mod._validate_chain=lambda *args, **kwargs: None
mod.pwd.getpwuid=lambda uid: types.SimpleNamespace(pw_name='devrunner', pw_uid=4101)
mod.pwd.getpwnam=lambda name: types.SimpleNamespace(pw_name='devrunner', pw_uid=4101)
os.environ.update(SUDO_UID='4101', SUDO_USER='devrunner')
assert mod.resolve_signing_state()[2]['name'] == 'dev-runner-vm'
for uid,user in [('4102','devrunner'),('4101','gpurunner'),('4101','dev-runner-vm')]:
    os.environ.update(SUDO_UID=uid,SUDO_USER=user)
    try: mod.resolve_signing_state()
    except SystemExit: pass
    else: raise AssertionError((uid,user))
os.environ.update(SUDO_UID='4101',SUDO_USER='devrunner')
classes.append({'name':'other','uid':4101,'signer_key':'/x','signer_principal_file':'/y'})
try: mod.resolve_signing_state()
except SystemExit: pass
else: raise AssertionError('ambiguous uid accepted')
print('test: numeric sudo caller/class resolution passed')
PY

python3 - "$PUBLIC_KEY" "$tmp/helper-out" <<'PY'
import base64, hashlib, json, pathlib, subprocess, sys
public_key, out_path = sys.argv[1], pathlib.Path(sys.argv[2])
response = json.loads(out_path.read_text())
assert response["result"] == "signed"
manifest = json.loads(base64.b64decode(response["manifest_b64"]))
assert manifest["signer_principal"] == "fake-runner"
assert manifest["signer_key_sha256"] == hashlib.sha256(public_key.encode()).hexdigest()
assert manifest["namespace"] == "factory-runner-receipt"
assert manifest["signature_algorithm"] == "ssh-ed25519"
assert response["signature_sha256"] == hashlib.sha256(base64.b64decode(response["signature_b64"])).hexdigest()
allowed = pathlib.Path(out_path.parent) / "allowed-signers"
allowed.write_text(f"fake-runner {public_key}\n")
with open(out_path.parent / "signature.sig", "wb") as stream:
    stream.write(base64.b64decode(response["signature_b64"]))
with open(out_path.parent / "manifest.json", "wb") as stream:
    stream.write(base64.b64decode(response["manifest_b64"]))
verified = subprocess.run(
    ["ssh-keygen", "-Y", "verify", "-f", str(allowed), "-I", "fake-runner",
     "-n", "factory-runner-receipt", "-s", str(out_path.parent / "signature.sig")],
    input=base64.b64decode(response["manifest_b64"]), capture_output=True,
)
assert verified.returncode == 0, verified.stderr
assert b"OPENSSH PRIVATE KEY" not in out_path.read_bytes()
PY

# A caller-supplied manifest blob is never signed: an opaque request, a wrong
# schema, an extra caller field, and caller-supplied signer identity are all
# rejected without producing a signature. A capability outside the class
# allowlist (even one the old hardcoded list happened to reject differently)
# is also never signed.
rc=$(printf '%s\n' '{"schema":"factory-runner-sign-request/v1","manifest":"not-a-dict"}' \
    | helper_run "$tmp/signer-key" "$tmp/signer-principal")
[[ $rc -eq 1 ]] || { echo "test: signer signed an opaque caller manifest" >&2; exit 1; }
rc=$(valid_request | sed 's/"schema": "factory-runner-sign-request\/v1"/"schema": "wrong-schema"/' \
    | helper_run "$tmp/signer-key" "$tmp/signer-principal")
[[ $rc -eq 1 ]] || { echo "test: signer accepted a wrong request schema" >&2; exit 1; }
rc=$(valid_request | sed 's/"capabilities": \["remote-project-gate"\]/"capabilities": ["remote-project-gate"], "signer_principal": "intruder"/' \
    | helper_run "$tmp/signer-key" "$tmp/signer-principal")
[[ $rc -eq 1 ]] || { echo "test: signer accepted caller-supplied signer identity" >&2; exit 1; }

# Failures, skips, and capability claims outside the class allowlist can never
# be signed.
for broken in \
    '"result": "fail"' \
    '"exit_code": 3' \
    '"timed_out": true' \
    '"cleanup": false' \
    '"capabilities": ["gpu-compositor"]' \
    '"runner": "intruder-runner"'; do
    rc=$(valid_request | sed "s/\"result\": \"pass\"/$broken/" \
        | helper_run "$tmp/signer-key" "$tmp/signer-principal")
    [[ $rc -eq 1 ]] || { echo "test: signer signed a rejected claim ($broken)" >&2; exit 1; }
done

# Unprivileged key state is fail-closed: a symlinked key, a world-readable
# key, a missing key, a missing principal, and a world-readable principal are
# all refused, and no signature is produced.
ln -s "$tmp/signer-key" "$tmp/key-link"
rc=$(valid_request | helper_run "$tmp/key-link" "$tmp/signer-principal")
[[ $rc -eq 1 ]] || { echo "test: signer accepted a symlinked key" >&2; exit 1; }
chmod 0644 "$tmp/signer-key"
rc=$(valid_request | helper_run "$tmp/signer-key" "$tmp/signer-principal")
[[ $rc -eq 1 ]] || { echo "test: signer accepted a world-readable key" >&2; exit 1; }
chmod 0600 "$tmp/signer-key"
rc=$(valid_request | helper_run "$tmp/missing-key" "$tmp/signer-principal")
[[ $rc -eq 1 ]] || { echo "test: signer accepted a missing key" >&2; exit 1; }
rc=$(valid_request | helper_run "$tmp/signer-key" "$tmp/missing-principal")
[[ $rc -eq 1 ]] || { echo "test: signer accepted a missing principal" >&2; exit 1; }
chmod 0644 "$tmp/signer-principal"
rc=$(valid_request | helper_run "$tmp/signer-key" "$tmp/signer-principal")
[[ $rc -eq 1 ]] || { echo "test: signer accepted a world-readable principal" >&2; exit 1; }
chmod 0600 "$tmp/signer-principal"
ssh-keygen -q -t rsa -b 2048 -N '' -f "$tmp/rsa-key"
chmod 0600 "$tmp/rsa-key"
rc=$(valid_request | helper_run "$tmp/rsa-key" "$tmp/signer-principal")
[[ $rc -eq 1 ]] || { echo "test: signer accepted a non-ed25519 key" >&2; exit 1; }
mkdir "$tmp/insecure-parent"
cp "$tmp/signer-key" "$tmp/insecure-parent/key"
cp "$tmp/signer-principal" "$tmp/insecure-parent/principal"
chmod 0600 "$tmp/insecure-parent/key" "$tmp/insecure-parent/principal"
chmod 0770 "$tmp/insecure-parent"
rc=$(valid_request | helper_run "$tmp/insecure-parent/key" "$tmp/insecure-parent/principal")
[[ $rc -eq 1 ]] || { echo "test: signer accepted writable signer-state parent" >&2; exit 1; }
chmod 0700 "$tmp/insecure-parent"
python3 - "$helper_dir/factory-runner-signer.py" "$tmp/insecure-parent/key" <<'PY'
import importlib.util, os, pathlib, sys
spec=importlib.util.spec_from_file_location('signer_mutation',sys.argv[1])
mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
path=pathlib.Path(sys.argv[2]); original=path.read_bytes(); fd=mod._open_bound(path,private=True)
replacement=path.with_name('replacement'); replacement.write_bytes(b'mutated'); replacement.chmod(0o600); replacement.replace(path)
assert os.pread(fd,len(original),0)==original
os.close(fd)
print('test: signer state descriptor remains inode-bound across substitution')
PY
if grep -q 'OPENSSH PRIVATE KEY' "$tmp/helper-out" "$tmp/helper-err" 2>/dev/null; then
    echo "test: signer leaked private key material" >&2
    exit 1
fi

echo "test: runner signer adversarial verification checks passed"
