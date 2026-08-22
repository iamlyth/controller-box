#!/usr/bin/env bash
# Adversarial visual-audit validation (deterministic mock reviewer, no model).
#
# Exercises the auditor-mandated fail-closed cases without any model call:
#   - missing images / provenance mismatch
#   - prompt drift and schema drift (freeze digest)
#   - malformed / hallucinated model output (schema rejection)
#   - false-positive known-bad (calibration blocks a blind model)
#   - shared-session races (dedicated lease refuses concurrent capture)
#   - replay (an older report cannot be replayed as current evidence)
#   - tamper (finding image hash not in provenance manifest)
#   - model outage (reviewer unavailable fails closed)
#   - capture driver fails closed (consumer must implement installed capture)
#   - capture-hang hardening: per-state hard timeout, own session/group,
#     TERM-group -> bounded grace -> KILL-group -> reap, partial image
#     removal, lease reacquirable after failure, validated state ids,
#     config timeout/grace ceilings, probe/model subprocess hard timeout
#   - mutable state lives under the ignored .factory-state/visual-audit/
#   - C3a trust hardening: pre-model provenance verification (image
#     ownership/link/symlink/hash, commit==HEAD, tree==HEAD^{tree}, tracked
#     worktree/index clean, environment binding to the committed
#     environment.toml), per-task finding binding (no cross-state swap),
#     state-id validation before path construction, and test-only execution
#     overrides gated behind RALPH_VISUAL_AUDIT_TESTING=1
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
cd -- "$PROJECT_ROOT"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

PY3="$PROJECT_ROOT/scripts/visual-audit-provenance.py"
REVIEW="$PROJECT_ROOT/scripts/visual-audit-review.py"
CHECK="$PROJECT_ROOT/scripts/check-visual-audit.py"
LEASE="$PROJECT_ROOT/scripts/visual-audit-lease.py"
CAPTURE="$PROJECT_ROOT/scripts/visual-audit-capture.py"
PROBE="$PROJECT_ROOT/scripts/visual-audit-probe.sh"

# --- deterministic PNG writer (stdlib) --------------------------------------
write_png() { # path w h rgb-triplet-csv
    python3 - "$1" "$2" "$3" "$4" <<'PY'
import struct, sys, zlib
path, w, h, rgb = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), tuple(int(x) for x in sys.argv[4].split(','))
def chunk(t, d):
    c = t + d
    return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
raw = b''.join(b'\x00' + bytes(rgb) * w for _ in range(h))
data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
open(path, 'wb').write(data)
PY
}

mkdir -p "$tmp/scripts" "$tmp/.factory/schemas" "$tmp/.factory/prompts" "$tmp/docs"
for script in visual-audit-provenance.py visual-audit-review.py visual-audit-lease.py \
        visual-audit-review-sdk.mjs visual-audit-capture.py visual-audit-capture.sh \
        visual-audit-probe.sh visual-capture-driver.sh check-visual-audit.py \
        visual-audit-gate.sh; do
    cp "$PROJECT_ROOT/scripts/$script" "$tmp/scripts/" 2>/dev/null || true
done
cp "$PROJECT_ROOT/.factory/schemas/visual-audit-review.schema.json" "$tmp/.factory/schemas/"
cp "$PROJECT_ROOT/.factory/prompts/visual-audit.md" "$tmp/.factory/prompts/"
cp "$PROJECT_ROOT/.factory/visual-audit.toml" "$tmp/.factory/"
cp "$PROJECT_ROOT/.factory/visual-audit-inventory.json" "$tmp/.factory/"
cp "$PROJECT_ROOT/.factory/visual-audit-calibration.json" "$tmp/.factory/"
# C3c positive fixture: deterministic calibration controls created before the
# initial temp-repo commit. >= 2 distinct known-bad and >= 1 known-good PNGs;
# the known-good reference must not be byte-identical to any live capture
# (live good states are 40,180,40) or the overlap guard refuses calibration.
mkdir -p "$tmp/captures/calibration"
write_png "$tmp/captures/calibration/cal-known-bad-blank.png" 64 32 0,0,0
write_png "$tmp/captures/calibration/cal-known-bad-clipped.png" 64 32 255,255,255
write_png "$tmp/captures/calibration/cal-current-bad-regression.png" 64 32 120,0,120
write_png "$tmp/captures/calibration/cal-reviewed-good-reference.png" 64 32 30,150,90
# Rewrite the calibration declaration with the exact per-image sha256 and the
# pass/finding expectations the mock driver must satisfy; it is committed with
# the base commit so calibration binds tracked inputs.
python3 - "$tmp/captures/calibration" "$tmp/.factory/visual-audit-calibration.json" <<'PY'
import hashlib, json, sys
cal_dir, out = sys.argv[1], sys.argv[2]
def sha(name):
    return hashlib.sha256(open(f"{cal_dir}/{name}", "rb").read()).hexdigest()
controls = [
    {"id": "cal-known-bad-blank", "expectation": "finding",
     "note": "Uniform blank control; report missing content, not this note's `}` delimiter."},
    {"id": "cal-known-bad-clipped", "expectation": "finding",
     "note": "Clipped layout control with quoted label: \"summary\"."},
    {"id": "cal-current-bad-regression", "expectation": "finding",
     "note": "Materially incorrect rendering control."},
    {"id": "cal-reviewed-good-reference", "expectation": "pass",
     "note": "Human-reviewed reference with complete layout."},
]
for control in controls:
    control["sha256"] = sha(f"{control['id']}.png")
declared = {
    "schema": "ralph-visual-audit-calibration/v1",
    "description": "Deterministic mock-driver calibration controls for the visual-audit fixture.",
    "images": controls,
}
open(out, "w").write(json.dumps(declared, indent=2) + "\n")
PY
# The committed .factory/environment.toml is the environment-binding input.
cp "$PROJECT_ROOT/.factory/environment.toml" "$tmp/.factory/environment.toml"
# The framework under test is the committed tmp copy (tracked inputs).
PY3="$tmp/scripts/visual-audit-provenance.py"
REVIEW="$tmp/scripts/visual-audit-review.py"
CHECK="$tmp/scripts/check-visual-audit.py"
LEASE="$tmp/scripts/visual-audit-lease.py"
CAPTURE="$tmp/scripts/visual-audit-capture.py"
PROBE="$tmp/scripts/visual-audit-probe.sh"
# Test inventory: state ids that the mock driver's verdict mapping understands.
cat > "$tmp/.factory/visual-audit-inventory.json" <<'JSON'
{
  "schema": "ralph-visual-audit-inventory/v1",
  "states": [
    {"id": "good-main", "risk": "critical", "navigation": ["launch"], "expected": "good state", "crops": ["full-frame"]},
    {"id": "good-secondary", "risk": "high", "navigation": ["launch"], "expected": "good state", "crops": ["full-frame"]}
  ]
}
JSON
printf '# Spec\n' > "$tmp/docs/SPEC.md"
printf '.factory-state/\n' > "$tmp/.gitignore"

# Minimal deterministic mock reviewer (verdict controlled by state id). The mock
# implements the sealed C3b SDK contract: it parses the required --request-nonce,
# echoes every sealed task field (incl. the exact nonce) into the finding, writes
# the finding and the exact-key ralph-visual-audit-invocation/v1 receipt as
# regular 0600 files, and seals the finding file's real SHA-256 and a raw-response
# digest into the receipt.
write_mock_driver() {
cat > "$tmp/mock-driver.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
out_dir=""; state_id=""; role=""; sha=""; model=""; psha=""; ssha=""; nonce=""
prompt_file=""; expected_b64=""; calibration_expectation=""; expected_seen=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out-dir) out_dir=$2; shift 2 ;;
        --state-id) state_id=$2; shift 2 ;;
        --role) role=$2; shift 2 ;;
        --expected-sha256) sha=$2; shift 2 ;;
        --prompt-file) prompt_file=$2; shift 2 ;;
        --expected-description-base64) expected_b64=$2; expected_seen=1; shift 2 ;;
        --calibration-expectation) calibration_expectation=$2; shift 2 ;;
        --prompt-sha256) psha=$2; shift 2 ;;
        --schema-sha256) ssha=$2; shift 2 ;;
        --model) model=$2; shift 2 ;;
        --request-nonce) nonce=$2; shift 2 ;;
        *) shift ;;
    esac
done
[[ -n "$nonce" ]] || { echo "mock-driver: --request-nonce is required" >&2; exit 2; }
[[ $expected_seen -eq 1 ]] || { echo "mock-driver: --expected-description-base64 is required" >&2; exit 2; }
[[ -n "$prompt_file" && -n "$calibration_expectation" ]] \
    || { echo "mock-driver: task prompt binding inputs are required" >&2; exit 2; }
# Independently reproduce the production SDK's canonical binding. This proves
# the orchestrator passes the exact live inventory description and, for
# calibration, the exact note plus a separately bound classification.
python3 - "$prompt_file" "$expected_b64" "$calibration_expectation" "$psha" <<'PY'
import base64, hashlib, json, sys
prompt, expected_b64, calibration_expectation, supplied = sys.argv[1:]
expected = base64.b64decode(expected_b64, validate=True).decode("utf-8")
payload = {
    "schema": "ralph-visual-audit-task-prompt/v1",
    "prompt_template_sha256": hashlib.sha256(open(prompt, "rb").read()).hexdigest(),
    "expected_description": expected,
    "calibration_expectation": calibration_expectation,
}
actual = hashlib.sha256(json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode()).hexdigest()
assert actual == supplied, (actual, supplied)
PY
mkdir -p "$out_dir"
case "$state_id" in
    probe)
        # C3c positive fixture: the real probe command requires a PASS whose
        # observation explicitly describes the solid red known image.
        verdict="pass"; obs='[{"code":"PROBE_COLOR","severity":"info","description":"solid red rectangle"}]' ;;
    cal-known-bad-*|cal-current-bad-*)
        # Known-bad / current-bad calibration controls must be flagged.
        verdict="finding"; obs='[{"code":"CAL_KNOWN_BAD","severity":"high","description":"known bad calibration control"}]' ;;
    cal-reviewed-good-reference)
        # Reviewed-good calibration control must pass.
        verdict="pass"; obs='[{"code":"CAL_REVIEWED_GOOD","severity":"info","description":"reviewed good reference"}]' ;;
    good-*) verdict="pass"; obs='[{"code":"MOCK_GOOD","severity":"info","description":"ok"}]' ;;
    bad-*)  verdict="finding"; obs='[{"code":"MOCK_BAD","severity":"high","description":"defect"}]' ;;
    *)      verdict="pass"; obs='[{"code":"MOCK_GOOD","severity":"info","description":"ok"}]' ;;
esac
python3 - "$out_dir" "$state_id" "$role" "$sha" "$model" "$psha" "$ssha" "$nonce" "$verdict" "$obs" <<'PY'
import hashlib, json, os, sys, time
out_dir, state_id, role, sha, model, psha, ssha, nonce, verdict, obs = sys.argv[1:]
finding = {
  "schema": "ralph-visual-audit-review/v1", "state_id": state_id,
  "image_sha256": sha, "role": role, "model": model,
  "prompt_sha256": psha, "schema_sha256": ssha,
  "request_nonce": nonce, "verdict": verdict,
  "observations": json.loads(obs)}
finding_path = f"{out_dir}/finding-{state_id}-{role}.json"
receipt_path = f"{out_dir}/receipt-{state_id}-{role}.json"
started = int(time.time() * 1000)
finding_bytes = (json.dumps(finding, indent=2) + "\n").encode("utf-8")
open(finding_path, "wb").write(finding_bytes)
os.chmod(finding_path, 0o600)
# Raw response commitment: the deterministic mock stream hashed before parse.
raw = json.dumps(finding)
finished = int(time.time() * 1000)
receipt = {
    "schema": "ralph-visual-audit-invocation/v1", "request_nonce": nonce,
    "state_id": state_id, "role": role, "image_sha256": sha,
    "prompt_sha256": psha, "schema_sha256": ssha, "model": model,
    "raw_response_sha256": hashlib.sha256(raw.encode()).hexdigest(),
    "finding_sha256": hashlib.sha256(finding_bytes).hexdigest(),
    "started_at_ms": started, "finished_at_ms": finished,
    "elapsed_ms": finished - started}
receipt_bytes = (json.dumps(receipt, indent=2, sort_keys=True) + "\n").encode("utf-8")
open(receipt_path, "wb").write(receipt_bytes)
os.chmod(receipt_path, 0o600)
PY
EOF
chmod +x "$tmp/mock-driver.sh"
}
write_mock_driver

# Config override for the temp repo (enabled, temp dirs, mock driver).
sed -e 's/enabled = false/enabled = true/' \
    -e 's|vision_model = "ollama/kimi-k2.7-code"|vision_model = "test-model"|' \
    -e "s|capture_dir = \".factory/artifacts/visual-audit/captures\"|capture_dir = \"$tmp/captures\"|" \
    -e "s|review_dir = \".factory/artifacts/visual-audit/reviews\"|review_dir = \"$tmp/reviews\"|" \
    -e "s|lease_file = \".factory-state/visual-audit.lease\"|lease_file = \"$tmp/lease\"|" \
    -e "s|sdk_driver = \"scripts/visual-audit-review-sdk.mjs\"|sdk_driver = \"$tmp/mock-driver.sh\"|" \
    "$tmp/.factory/visual-audit.toml" > "$tmp/va.toml"

cd "$tmp"
git init -q -b develop
git config user.name test
git config user.email test@example.invalid
git add .factory docs scripts .gitignore
git commit -qm base
HEAD=$(git rev-parse HEAD)
# The whole file is a testing context: test-only execution overrides
# (VISUAL_AUDIT_CONFIG, VISUAL_AUDIT_SDK_DRIVER, fake models) are honored.
# Production-rejection cases below explicitly unset the marker.
export RALPH_VISUAL_AUDIT_TESTING=1

fail() { echo "test-visual-audit: $*" >&2; exit 1; }
expect_rc() { local want=$1 got=$2 label=$3; [[ $got -eq $want ]] || fail "$label (expected rc=$want got rc=$got)"; }

# Assert a process is gone using pid + /proc starttime identity (a raw pid
# liveness check alone is PID-reuse-prone; when /proc is available the
# recorded starttime must either be absent (process gone) or differ (pid was
# reused by a different process).
assert_gone() { # pid starttime label
    local pid=$1 start=$2 label=$3 now=""
    if [[ -d "/proc/$pid" ]]; then
        now=$(awk '{print $22}' "/proc/$pid/stat" 2>/dev/null || true)
        [[ -n "$start" && -n "$now" && "$now" != "$start" ]] \
            || fail "$label pid $pid still alive (starttime $start -> $now)"
    fi
}

# Build an enabled capture config from the tracked template.
capture_config() { # out_toml mock_driver cap_dir lease_file inventory timeout grace
    sed -e 's/enabled = false/enabled = true/' \
        -e "s|capture_dir = \".factory/artifacts/visual-audit/captures\"|capture_dir = \"$3\"|" \
        -e "s|lease_file = \".factory-state/visual-audit.lease\"|lease_file = \"$4\"|" \
        -e "s|capture_driver = \"scripts/visual-capture-driver.sh\"|capture_driver = \"$2\"|" \
        -e "s|inventory = \".factory/visual-audit-inventory.json\"|inventory = \"$5\"|" \
        "$tmp/.factory/visual-audit.toml" > "$1"
    printf '\ncapture_timeout_seconds = %s\ncapture_cleanup_grace_seconds = %s\n' "$6" "$7" >> "$1"
}

write_capture_inv() { # path state-id
    cat > "$1" <<JSON
{"schema":"ralph-visual-audit-inventory/v1","states":[{"id":"$2","risk":"high","navigation":["launch"],"expected":"x","crops":["full-frame"]}]}
JSON
}

# --- C3c positive fixture: probe + calibration before the first review -------
# Production now requires a real passing probe and then a passing calibration
# before any live review. Both are exercised through the real commands with the
# mock SDK driver (never by hand-writing receipts): the probe writes its
# durable 0600 receipt + finding artifact, calibrate validates the probe receipt
# and writes the 0600 calibration receipt. Assert both succeed and that every
# durable receipt is a regular 0600 file.
set +e
VISUAL_AUDIT_SDK_DRIVER="$tmp/mock-driver.sh" VISUAL_AUDIT_VISION_MODEL=test-model \
    "$PROBE" >"$tmp/c3c-probe.out" 2>&1
rc=$?
set -e
expect_rc 0 $rc "mock probe before first review"
grep -q "PASS" "$tmp/c3c-probe.out" || fail "probe must report PASS"
for f in probe-receipt.json probe-finding.json; do
    [[ -f "$tmp/.factory-state/visual-audit/$f" ]] || fail "probe must write $f"
    [[ "$(stat -c %a "$tmp/.factory-state/visual-audit/$f")" == "600" ]] \
        || fail "$f must be 0600"
done
set +e
VISUAL_AUDIT_SDK_DRIVER="$tmp/mock-driver.sh" python3 "$REVIEW" \
    --config "$tmp/va.toml" calibrate >"$tmp/c3c-calibrate.out" 2>&1
rc=$?
set -e
expect_rc 0 $rc "mock calibration before first review"
grep -q "calibration passed" "$tmp/c3c-calibrate.out" \
    || fail "calibration must report success"
[[ -f "$tmp/.factory-state/visual-audit/calibration-receipt.json" ]] \
    || fail "calibration must write its receipt"
[[ "$(stat -c %a "$tmp/.factory-state/visual-audit/calibration-receipt.json")" == "600" ]] \
    || fail "calibration receipt must be 0600"

# --- capture: two deterministic states ---------------------------------------
mkdir -p "$tmp/captures"
chmod 700 "$tmp/captures"
write_png "$tmp/captures/good-main.png" 64 32 40,180,40
write_png "$tmp/captures/good-secondary.png" 64 32 40,180,40
python3 "$PY3" manifest --out "$tmp/captures" --commit "$HEAD" --tree "$(git rev-parse 'HEAD^{tree}')" >/dev/null
python3 "$PY3" verify --out "$tmp/captures" || fail "provenance verify failed"

# --- 1. review run with deterministic mock -----------------------------------
set +e
VISUAL_AUDIT_SDK_DRIVER="$tmp/mock-driver.sh" python3 "$REVIEW" --config "$tmp/va.toml" run >/dev/null 2>&1
rc=$?
set -e
expect_rc 0 $rc "review run"

# --- 2. gate passes on the valid report ---------------------------------------
set +e
python3 "$CHECK" --config "$tmp/va.toml" --report "$tmp/reviews/report.json" --current-commit "$HEAD" >/dev/null 2>&1
rc=$?
set -e
expect_rc 0 $rc "gate pass"

# --- 2b. a report containing a finding is reported (never elevated) ------------
python3 - "$tmp/reviews/report.json" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
r['findings'][0]['verdict'] = 'finding'
r['findings'][0]['observations'] = [{'code':'REAL_DEFECT','severity':'high','description':'material visual defect'}]
json.dump(r, open(sys.argv[1], 'w'))
PY
set +e
python3 "$CHECK" --config "$tmp/va.toml" --report "$tmp/reviews/report.json" --current-commit "$HEAD" >/dev/null 2>&1
rc=$?
set -e
expect_rc 1 $rc "finding report rejected"
# restore a clean pass report for subsequent cases
VISUAL_AUDIT_SDK_DRIVER="$tmp/mock-driver.sh" python3 "$REVIEW" --config "$tmp/va.toml" run >/dev/null 2>&1

# ============================================================================
# C3b adversarial cases (receipt/finding binding, one tampered property at a
# time against a valid receipt-aware baseline). The baseline is snapshotted
# right after a deterministic mock review run; every case restores it, tampers
# exactly one property, and asserts BOTH the report-check gate and the
# check-visual-audit.py aggregate gate fail closed with the intended
# diagnostic. Re-sealing digests is part of the helper so a case can only fail
# for the tampered property, never for unrelated digest bookkeeping.
# ============================================================================
snapshot_baseline() { rm -rf "$tmp/baseline-reviews"; cp -a "$tmp/reviews" "$tmp/baseline-reviews"; }
restore_baseline() { rm -rf "$tmp/reviews"; cp -a "$tmp/baseline-reviews" "$tmp/reviews"; }
snapshot_baseline

# Run both gates against the current (possibly tampered) reviews dir.
# Asserts: nonzero exit from every gate and the intended diagnostic printed.
c3b_gates() { # label check-diag report-diag outprefix
    local label=$1 want_check=$2 want_report=$3 out=$4 rc1 rc2
    set +e
    python3 "$CHECK" --config "$tmp/va.toml" --report "$tmp/reviews/report.json" \
        --current-commit "$HEAD" >"$out.check" 2>&1
    rc1=$?
    python3 "$REVIEW" --config "$tmp/va.toml" report-check \
        --report "$tmp/reviews/report.json" --current-commit "$HEAD" >"$out.report-check" 2>&1
    rc2=$?
    set -e
    expect_rc 1 $rc1 "$label: check-visual-audit"
    expect_rc 1 $rc2 "$label: report-check"
    grep -q "$want_check" "$out.check" \
        || fail "$label: check-visual-audit missing diagnostic: $want_check"
    grep -q "$want_report" "$out.report-check" \
        || fail "$label: report-check missing diagnostic: $want_report"
}

# One-property-at-a-time tamper against the valid baseline. Re-seals the
# dependent digest fields (receipt finding_sha256, report entry digests) so
# only the tampered property can fail the gate.
tamper_one() { # mode
    python3 - "$tmp/reviews" "$1" <<'PY'
import hashlib, json, os, shutil, sys
rd = sys.argv[1]
mode = sys.argv[2]

def sha(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()

def rdj(p):
    return json.load(open(p, encoding="utf-8"))

def wrj(p, o):
    open(p, "w").write(json.dumps(o, indent=2, sort_keys=True) + "\n")

def entry(rep, sid, role):
    for e in rep["task_receipts"]:
        if e["state_id"] == sid and e["role"] == role:
            return e
    raise KeyError((sid, role))

def fp(sid, role):
    return f"{rd}/finding-{sid}-{role}.json"

def rp(sid, role):
    return f"{rd}/receipt-{sid}-{role}.json"

rep_path = f"{rd}/report.json"
rep = rdj(rep_path)

def reseal_finding(sid, role):
    """Re-seal the receipt+report finding digest for a rewritten finding file."""
    rec = rdj(rp(sid, role))
    rec["finding_sha256"] = sha(fp(sid, role))
    wrj(rp(sid, role), rec)
    e = entry(rep, sid, role)
    e["finding_sha256"] = sha(fp(sid, role))
    e["receipt_sha256"] = sha(rp(sid, role))

if mode == "finding-wrong-nonce":
    f = rdj(fp("good-main", "diagram"))
    f["request_nonce"] = "f" * 32
    wrj(fp("good-main", "diagram"), f)
    reseal_finding("good-main", "diagram")
elif mode == "finding-missing-nonce":
    f = rdj(fp("good-main", "diagram"))
    del f["request_nonce"]
    wrj(fp("good-main", "diagram"), f)
    reseal_finding("good-main", "diagram")
    for fnd in rep["findings"]:
        if fnd.get("state_id") == "good-main" and fnd.get("role") == "diagram":
            del fnd["request_nonce"]
elif mode == "finding-model-wrong":
    f = rdj(fp("good-main", "diagram"))
    f["model"] = "evil-model"
    wrj(fp("good-main", "diagram"), f)
    reseal_finding("good-main", "diagram")
elif mode == "receipt-wrong-nonce":
    rec = rdj(rp("good-main", "diagram"))
    rec["request_nonce"] = "e" * 32
    wrj(rp("good-main", "diagram"), rec)
    entry(rep, "good-main", "diagram")["receipt_sha256"] = sha(rp("good-main", "diagram"))
elif mode == "receipt-wrong-prompt-binding":
    rec = rdj(rp("good-main", "diagram"))
    rec["prompt_sha256"] = "d" * 64
    wrj(rp("good-main", "diagram"), rec)
    entry(rep, "good-main", "diagram")["receipt_sha256"] = sha(rp("good-main", "diagram"))
elif mode == "report-stale-expected-binding":
    entry(rep, "good-main", "diagram")["prompt_sha256"] = "c" * 64
elif mode == "report-inventory-binding":
    rep["inventory_sha256"] = "b" * 64
elif mode == "receipt-replay-state":
    shutil.copyfile(rp("good-main", "diagram"), rp("good-secondary", "diagram"))
    entry(rep, "good-secondary", "diagram")["receipt_sha256"] = sha(rp("good-secondary", "diagram"))
elif mode == "finding-swap-state":
    shutil.copyfile(fp("good-main", "diagram"), fp("good-secondary", "diagram"))
    reseal_finding("good-secondary", "diagram")
elif mode == "finding-modified-after-receipt":
    # No re-seal: the receipt/report still carry the sealed digest of the
    # original finding, so the digest mismatch is the intended diagnostic.
    with open(fp("good-main", "diagram"), "ab") as stream:
        stream.write(b"\nTAMPERED-AFTER-RECEIPT\n")
elif mode == "receipt-modified-after-report":
    # No re-seal: the report entry still carries the sealed receipt digest.
    rec = rdj(rp("good-main", "diagram"))
    rec["elapsed_ms"] = rec["elapsed_ms"] + 1
    wrj(rp("good-main", "diagram"), rec)
elif mode == "receipt-missing":
    os.unlink(rp("good-main", "diagram"))
elif mode == "finding-missing":
    os.unlink(fp("good-main", "diagram"))
elif mode == "both-missing":
    os.unlink(fp("good-main", "diagram"))
    os.unlink(rp("good-main", "diagram"))
else:
    raise SystemExit("unknown tamper mode " + mode)
wrj(rep_path, rep)
PY
}

# --- C3b-1: finding carries a stale/wrong request nonce ---------------------
restore_baseline
tamper_one finding-wrong-nonce
c3b_gates "finding wrong nonce" "request_nonce mismatch for task good-main/diagram" "request_nonce mismatch for task good-main/diagram" "$tmp/c3b1"

# --- C3b-2: finding missing the request nonce -------------------------------
restore_baseline
tamper_one finding-missing-nonce
c3b_gates "finding missing nonce" "finding request nonce invalid" "finding missing keys" "$tmp/c3b2"

# --- C3b-3: receipt file missing ---------------------------------------------
restore_baseline
tamper_one receipt-missing
c3b_gates "receipt missing" "receipt file missing or unsafe: receipt-good-main-diagram.json" "receipt file missing or unsafe: receipt-good-main-diagram.json" "$tmp/c3b3"

# --- C3b-4: receipt carries a wrong nonce ------------------------------------
restore_baseline
tamper_one receipt-wrong-nonce
c3b_gates "receipt wrong nonce" "receipt request_nonce mismatch for task good-main/diagram" "receipt request_nonce mismatch for task good-main/diagram" "$tmp/c3b4"

# --- C3b-5: receipt prompt/expectation binding mismatch ----------------------
restore_baseline
tamper_one receipt-wrong-prompt-binding
c3b_gates "receipt expectation binding mismatch" "receipt prompt_sha256 mismatch for task good-main/diagram" "receipt prompt_sha256 mismatch for task good-main/diagram" "$tmp/c3b5"

# --- C3b-6: report carries a stale expected-state binding --------------------
restore_baseline
tamper_one report-stale-expected-binding
c3b_gates "stale expected-state binding" "expected-description binding mismatch for good-main/diagram" "expected-description binding mismatch for good-main/diagram" "$tmp/c3b6"

# --- C3b-7: aggregate report inventory binding is tamper-evident -------------
restore_baseline
tamper_one report-inventory-binding
c3b_gates "report inventory binding tamper" "inventory drift" "inventory drift" "$tmp/c3b7"

# --- C3b-8: receipt replayed/copied to another state -------------------------
restore_baseline
tamper_one receipt-replay-state
c3b_gates "receipt replayed to another state" "receipt state_id mismatch for task good-secondary/diagram" "receipt state_id mismatch for task good-secondary/diagram" "$tmp/c3b8"

# --- C3b-6: finding replayed/swapped across states ---------------------------
restore_baseline
tamper_one finding-swap-state
c3b_gates "finding swapped across states" "state_id 'good-main' != task 'good-secondary'" "state_id 'good-main' != task 'good-secondary'" "$tmp/c3b6"

# --- C3b-7: finding file modified after its receipt was sealed ---------------
restore_baseline
tamper_one finding-modified-after-receipt
c3b_gates "finding modified after receipt" "finding digest mismatch for good-main/diagram" "finding digest mismatch for good-main/diagram" "$tmp/c3b7"

# --- C3b-8: receipt file modified after the report was written ---------------
restore_baseline
tamper_one receipt-modified-after-report
c3b_gates "receipt modified after report" "receipt digest mismatch for good-main/diagram" "receipt digest mismatch for good-main/diagram" "$tmp/c3b8"

# --- C3b-9: finding missing during report-check/check-visual-audit -----------
restore_baseline
tamper_one finding-missing
c3b_gates "finding missing" "finding file missing or unsafe: finding-good-main-diagram.json" "finding file missing or unsafe: finding-good-main-diagram.json" "$tmp/c3b9"

# --- C3b-10: receipt AND finding missing during both gates -------------------
restore_baseline
tamper_one both-missing
c3b_gates "receipt and finding missing" "finding file missing or unsafe: finding-good-main-diagram.json" "finding file missing or unsafe: finding-good-main-diagram.json" "$tmp/c3b10"

# --- C3b-11: finding model field tampered ------------------------------------
restore_baseline
tamper_one finding-model-wrong
c3b_gates "finding model mismatch" "model mismatch for task good-main/diagram" "model 'evil-model' != task" "$tmp/c3b11"

# --- positive baseline remains green after all tampering ---------------------
restore_baseline
set +e
python3 "$CHECK" --config "$tmp/va.toml" --report "$tmp/reviews/report.json" \
    --current-commit "$HEAD" >"$tmp/c3b-pos.check" 2>&1
rc=$?
python3 "$REVIEW" --config "$tmp/va.toml" report-check \
    --report "$tmp/reviews/report.json" --current-commit "$HEAD" >"$tmp/c3b-pos.report-check" 2>&1
rc2=$?
set -e
expect_rc 0 $rc "C3b positive baseline check-visual-audit"
expect_rc 0 $rc2 "C3b positive baseline report-check"

echo "test-visual-audit: C3b adversarial cases passed"

# ============================================================================
# C3c adversarial cases: calibration/probe/gate fail-closed hardening. Every
# case starts from the valid baseline (real mock probe + calibration receipts,
# valid reviews, clean tracked inputs) and mutates exactly ONE property; each
# case restores the valid baseline. `run` rejections go through the counting
# SDK driver so a case can only pass when the fail-closed gate fires BEFORE
# any reviewer/model invocation (the invocation sentinel must stay empty).
# ============================================================================
cat > "$tmp/counting-driver.sh" <<EOF
#!/usr/bin/env bash
echo "invoked" >> "$tmp/sdk-count"
exec "$tmp/mock-driver.sh" "\$@"
EOF
chmod +x "$tmp/counting-driver.sh"
: > "$tmp/sdk-count"
CAL_RECEIPT="$tmp/.factory-state/visual-audit/calibration-receipt.json"

# Rebuild the enabled review config from the tracked template (restore point).
write_va_config() {
    sed -e 's/enabled = false/enabled = true/' \
        -e 's|vision_model = "ollama/kimi-k2.7-code"|vision_model = "test-model"|' \
        -e "s|capture_dir = \".factory/artifacts/visual-audit/captures\"|capture_dir = \"$tmp/captures\"|" \
        -e "s|review_dir = \".factory/artifacts/visual-audit/reviews\"|review_dir = \"$tmp/reviews\"|" \
        -e "s|lease_file = \".factory-state/visual-audit.lease\"|lease_file = \"$tmp/lease\"|" \
        -e "s|sdk_driver = \"scripts/visual-audit-review-sdk.mjs\"|sdk_driver = \"$tmp/mock-driver.sh\"|" \
                    "$tmp/.factory/visual-audit.toml" > "$tmp/va.toml"
}

# Regenerate the durable calibration receipt with the real production command.
restore_calibration() {
    VISUAL_AUDIT_SDK_DRIVER="$tmp/mock-driver.sh" python3 "$REVIEW" --config "$tmp/va.toml" calibrate >/dev/null 2>&1
}

# `run` must fail closed with the intended diagnostic BEFORE any SDK driver
# invocation (the invocation sentinel must stay empty).
reject_run_before_reviewer() { # label want-diag
    local label="$1" want="$2" rc
    : > "$tmp/sdk-count"
    set +e
    VISUAL_AUDIT_SDK_DRIVER="$tmp/counting-driver.sh" python3 "$REVIEW" --config "$tmp/va.toml" run \
        >"$tmp/c3c-run.out" 2>&1
    rc=$?
    set -e
    expect_rc 1 "$rc" "$label: run fails closed"
    grep -q "$want" "$tmp/c3c-run.out" \
        || fail "$label: run missing diagnostic: $want"
    [[ "$(wc -l < "$tmp/sdk-count")" == "0" ]] \
        || fail "$label: SDK/reviewer invoked before rejection"
}

# calibrate must fail closed and must never write/replace the durable receipt.
reject_calibrate() { # label [want-diag]
    local label="$1" want="${2:-}" rc before after
    before=$(sha256sum "$CAL_RECEIPT")
    set +e
    VISUAL_AUDIT_SDK_DRIVER="$tmp/mock-driver.sh" python3 "$REVIEW" --config "$tmp/va.toml" calibrate \
        >"$tmp/c3c-cal.out" 2>&1
    rc=$?
    set -e
    expect_rc 1 "$rc" "$label: calibrate fails closed"
    if [[ -n "$want" ]]; then
        grep -q "$want" "$tmp/c3c-cal.out" \
            || fail "$label: calibrate missing diagnostic: $want"
    fi
    after=$(sha256sum "$CAL_RECEIPT")
    [[ "$after" == "$before" ]] || fail "$label: failed calibration wrote a receipt"
}

# Both aggregate gates must fail closed with their intended diagnostics.
reject_gates() { # label diag-check diag-report [current-commit]
    local label="$1" want_check="$2" want_report="$3" cur="${4:-$HEAD}" rc1 rc2
    set +e
    python3 "$CHECK" --config "$tmp/va.toml" --report "$tmp/reviews/report.json" \
        --current-commit "$cur" >"$tmp/c3c-check.out" 2>&1
    rc1=$?
    python3 "$REVIEW" --config "$tmp/va.toml" report-check \
        --report "$tmp/reviews/report.json" --current-commit "$cur" >"$tmp/c3c-rc.out" 2>&1
    rc2=$?
    set -e
    expect_rc 1 "$rc1" "$label: check-visual-audit fails closed"
    expect_rc 1 "$rc2" "$label: report-check fails closed"
    grep -q "$want_check" "$tmp/c3c-check.out" \
        || fail "$label: check-visual-audit missing diagnostic: $want_check"
    grep -q "$want_report" "$tmp/c3c-rc.out" \
        || fail "$label: report-check missing diagnostic: $want_report"
}

# --- C3c-1: live run before any calibration receipt fails before SDK ---------
restore_baseline
rm -f "$CAL_RECEIPT"
reject_run_before_reviewer "run with no calibration receipt" "calibration receipt required"
reject_gates "run with no calibration receipt" "calibration receipt required" "calibration receipt required"
restore_calibration

# --- C3c-2: declared calibration set with zero known-good controls -------------
restore_baseline
python3 - "$tmp/.factory/visual-audit-calibration.json" <<'PY'
import json, sys
p = sys.argv[1]
m = json.load(open(p))
m["images"] = [i for i in m["images"] if i["expectation"] != "pass"]
json.dump(m, open(p, "w"), indent=2)
PY
reject_calibrate "calibration set with zero known-good" "at least one known-good"
git checkout -- .factory/visual-audit-calibration.json

# --- C3c-3: declared calibration set with fewer than two known-bad -------------
restore_baseline
python3 - "$tmp/.factory/visual-audit-calibration.json" <<'PY'
import json, sys
p = sys.argv[1]
m = json.load(open(p))
bad = [i for i in m["images"] if i["expectation"] == "finding"]
good = [i for i in m["images"] if i["expectation"] == "pass"]
m["images"] = bad[:1] + good
json.dump(m, open(p, "w"), indent=2)
PY
reject_calibrate "calibration set with one known-bad" "at least two distinct known-bad"
git checkout -- .factory/visual-audit-calibration.json

# --- C3c-4: duplicated calibration control ids ---------------------------------
restore_baseline
python3 - "$tmp/.factory/visual-audit-calibration.json" <<'PY'
import json, sys
p = sys.argv[1]
m = json.load(open(p))
m["images"] = m["images"] + [dict(m["images"][0])]
json.dump(m, open(p, "w"), indent=2)
PY
reject_calibrate "duplicate calibration control ids" "calibration control id is duplicated"
git checkout -- .factory/visual-audit-calibration.json

# --- C3c-5: byte-identical calibration controls (duplicate hashes) ------------
restore_baseline
cp "$tmp/captures/calibration/cal-known-bad-blank.png" "$tmp/captures/calibration/cal-known-bad-twin.png"
python3 - "$tmp/.factory/visual-audit-calibration.json" <<'PY'
import json, sys
p = sys.argv[1]
m = json.load(open(p))
m["images"] = m["images"] + [{"id": "cal-known-bad-twin", "expectation": "finding",
                              "sha256": m["images"][0]["sha256"]}]
json.dump(m, open(p, "w"), indent=2)
PY
reject_calibrate "duplicate calibration control hashes"
rm -f "$tmp/captures/calibration/cal-known-bad-twin.png"
git checkout -- .factory/visual-audit-calibration.json

# --- C3c-6: declared sha256 that does not match the control image -------------
restore_baseline
python3 - "$tmp/.factory/visual-audit-calibration.json" <<'PY'
import json, sys
p = sys.argv[1]
m = json.load(open(p))
for i in m["images"]:
    if i["id"] == "cal-known-bad-blank":
        i["sha256"] = "0" * 64
json.dump(m, open(p, "w"), indent=2)
PY
reject_calibrate "wrong declared calibration sha256"
git checkout -- .factory/visual-audit-calibration.json

# --- C3c-7: calibration control byte-overlaps a live capture -------------------
restore_baseline
write_png "$tmp/captures/calibration/cal-reviewed-good-reference.png" 64 32 40,180,40
reject_calibrate "live-image overlap" "overlaps a live capture image"
write_png "$tmp/captures/calibration/cal-reviewed-good-reference.png" 64 32 30,150,90

# --- C3c-8: known-good false positive blocks the calibration receipt ------------
# (case 11 below covers the known-bad false-negative counterpart).
restore_baseline
cat > "$tmp/fp-good-driver.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
out_dir=""; state_id=""; role=""; sha=""; model=""; psha=""; ssha=""; nonce=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out-dir) out_dir=$2; shift 2 ;;
        --state-id) state_id=$2; shift 2 ;;
        --role) role=$2; shift 2 ;;
        --expected-sha256) sha=$2; shift 2 ;;
        --prompt-sha256) psha=$2; shift 2 ;;
        --schema-sha256) ssha=$2; shift 2 ;;
        --model) model=$2; shift 2 ;;
        --request-nonce) nonce=$2; shift 2 ;;
        *) shift ;;
    esac
 done
[[ -n "$nonce" ]] || { echo "fp-good-driver: --request-nonce is required" >&2; exit 2; }
mkdir -p "$out_dir"
case "$state_id" in
    cal-reviewed-good-reference)
        verdict="finding"; obs='[{"code":"CAL_FALSE_POSITIVE","severity":"high","description":"false positive on known-good"}]' ;;
    cal-known-bad-*|cal-current-bad-*)
        verdict="finding"; obs='[{"code":"CAL_KNOWN_BAD","severity":"high","description":"known bad calibration control"}]' ;;
    probe)
        verdict="pass"; obs='[{"code":"PROBE_COLOR","severity":"info","description":"solid red rectangle"}]' ;;
    *) verdict="pass"; obs='[{"code":"MOCK_GOOD","severity":"info","description":"ok"}]' ;;
esac
python3 - "$out_dir" "$state_id" "$role" "$sha" "$model" "$psha" "$ssha" "$nonce" "$verdict" "$obs" <<'PY'
import hashlib, json, os, sys, time
out_dir, state_id, role, sha, model, psha, ssha, nonce, verdict, obs = sys.argv[1:]
finding = {"schema": "ralph-visual-audit-review/v1", "state_id": state_id,
  "image_sha256": sha, "role": role, "model": model,
  "prompt_sha256": psha, "schema_sha256": ssha, "request_nonce": nonce,
  "verdict": verdict, "observations": json.loads(obs)}
finding_path = f"{out_dir}/finding-{state_id}-{role}.json"
receipt_path = f"{out_dir}/receipt-{state_id}-{role}.json"
started = int(time.time() * 1000)
finding_bytes = (json.dumps(finding, indent=2) + "\n").encode("utf-8")
open(finding_path, "wb").write(finding_bytes)
os.chmod(finding_path, 0o600)
raw = json.dumps(finding)
finished = int(time.time() * 1000)
receipt = {"schema": "ralph-visual-audit-invocation/v1", "request_nonce": nonce,
  "state_id": state_id, "role": role, "image_sha256": sha,
  "prompt_sha256": psha, "schema_sha256": ssha, "model": model,
  "raw_response_sha256": hashlib.sha256(raw.encode()).hexdigest(),
  "finding_sha256": hashlib.sha256(finding_bytes).hexdigest(),
  "started_at_ms": started, "finished_at_ms": finished, "elapsed_ms": finished - started}
receipt_bytes = (json.dumps(receipt, indent=2, sort_keys=True) + "\n").encode("utf-8")
open(receipt_path, "wb").write(receipt_bytes)
os.chmod(receipt_path, 0o600)
PY
EOF
chmod +x "$tmp/fp-good-driver.sh"
before=$(sha256sum "$CAL_RECEIPT")
set +e
VISUAL_AUDIT_SDK_DRIVER="$tmp/fp-good-driver.sh" python3 "$REVIEW" --config "$tmp/va.toml" calibrate >"$tmp/c3c-fp.out" 2>&1
rc=$?
set -e
expect_rc 1 "$rc" "known-good false positive blocks calibration"
grep -q "false-positive known-good" "$tmp/c3c-fp.out" \
    || fail "known-good false positive missing diagnostic"
after=$(sha256sum "$CAL_RECEIPT")
[[ "$before" == "$after" ]] || fail "known-good false-positive calibration wrote a receipt"

# --- C3c-9: tampered calibration receipt (failed control record) ---------------
restore_baseline
python3 - "$CAL_RECEIPT" <<'PY'
import json, sys
p = sys.argv[1]
r = json.load(open(p))
r["controls"][0]["ok"] = False
json.dump(r, open(p, "w"), indent=2, sort_keys=True)
PY
reject_run_before_reviewer "tampered calibration receipt" "records a failed control"
reject_gates "tampered calibration receipt" "records a failed control" "records a failed control"
restore_calibration

# --- C3c-10: calibration note/classification task binding cannot replay -------
restore_baseline
python3 - "$CAL_RECEIPT" <<'PY'
import json, sys
p = sys.argv[1]
r = json.load(open(p))
r["controls"][0]["task_prompt_sha256"] = "a" * 64
json.dump(r, open(p, "w"), indent=2, sort_keys=True)
PY
reject_run_before_reviewer "tampered calibration expected binding" "expected-description binding mismatch"
reject_gates "tampered calibration expected binding" "expected-description binding mismatch" "expected-description binding mismatch"
restore_calibration

# --- C3c-11: model drift invalidates calibration before any reviewer -----------
restore_baseline
sed -i 's/vision_model = "test-model"/vision_model = "drift-model"/' "$tmp/va.toml"
reject_run_before_reviewer "model drift" "calibration receipt model drift"
reject_gates "model drift" "calibration receipt model drift" "calibration receipt model drift"
write_va_config

# --- C3c-11: prompt drift -------------------------------------------------------
restore_baseline
echo "# drift" >> "$tmp/.factory/prompts/visual-audit.md"
reject_run_before_reviewer "prompt drift" "calibration receipt prompt drift"
reject_gates "prompt drift" "prompt drift" "prompt drift"
git checkout -- .factory/prompts/visual-audit.md

# --- C3c-12: schema drift -------------------------------------------------------
restore_baseline
printf '\n{"extra":true}\n' >> "$tmp/.factory/schemas/visual-audit-review.schema.json"
reject_run_before_reviewer "schema drift" "calibration receipt schema drift"
reject_gates "schema drift" "schema drift" "schema drift"
git checkout -- .factory/schemas/visual-audit-review.schema.json

# --- C3c-13: calibration manifest drift -----------------------------------------
restore_baseline
python3 - "$tmp/.factory/visual-audit-calibration.json" <<'PY'
import json, sys
p = sys.argv[1]
m = json.load(open(p))
m["description"] = m["description"] + " (drift)"
json.dump(m, open(p, "w"), indent=2)
PY
reject_run_before_reviewer "calibration manifest drift" "calibration drift"
reject_gates "calibration manifest drift" "calibration drift" "calibration drift"
git checkout -- .factory/visual-audit-calibration.json

# --- C3c-14: framework commit drift ---------------------------------------------
restore_baseline
git commit --allow-empty -qm drift
reject_run_before_reviewer "commit drift" "calibration receipt commit stale"
reject_gates "commit drift" "report replay/commit mismatch" "report replay/commit mismatch" "$(git rev-parse HEAD)"
git reset --hard HEAD~1

# --- C3c-15: tree drift (receipt bound to a stale tree) -------------------------
restore_baseline
python3 - "$CAL_RECEIPT" <<'PY'
import json, sys
p = sys.argv[1]
r = json.load(open(p))
r["tree"] = "0" * 40
json.dump(r, open(p, "w"), indent=2, sort_keys=True)
PY
reject_run_before_reviewer "tree drift" "calibration receipt tree mismatch"
reject_gates "tree drift" "calibration receipt tree mismatch" "calibration receipt tree mismatch"
restore_calibration

# --- C3c baseline restore + tracked-clean guard ---------------------------------
restore_baseline
write_va_config
restore_calibration
dirty=$(git status --porcelain --untracked-files=no)
[[ -z "$dirty" ]] || fail "C3c left tracked inputs dirty: $dirty"
echo "test-visual-audit: C3c adversarial cases passed"

# --- 3. replay: wrong current commit is rejected ------------------------------
set +e
python3 "$CHECK" --config "$tmp/va.toml" --report "$tmp/reviews/report.json" \
    --current-commit 0000000000000000000000000000000000000000 >/dev/null 2>&1
rc=$?
set -e
expect_rc 1 $rc "replay rejected"

# --- 4. prompt drift: touching the frozen template invalidates findings -------
echo "# drift" >> "$tmp/.factory/prompts/visual-audit.md"
set +e
python3 "$CHECK" --config "$tmp/va.toml" --report "$tmp/reviews/report.json" --current-commit "$HEAD" >/dev/null 2>&1
rc=$?
set -e
expect_rc 1 $rc "prompt drift rejected"
git checkout -- .factory/prompts/visual-audit.md

# --- 5. schema drift ----------------------------------------------------------
cp "$tmp/.factory/schemas/visual-audit-review.schema.json" "$tmp/schema.bak"
printf '\n{"extra":true}\n' >> "$tmp/.factory/schemas/visual-audit-review.schema.json"
set +e
python3 "$CHECK" --config "$tmp/va.toml" --report "$tmp/reviews/report.json" --current-commit "$HEAD" >/dev/null 2>&1
rc=$?
set -e
expect_rc 1 $rc "schema drift rejected"
mv "$tmp/schema.bak" "$tmp/.factory/schemas/visual-audit-review.schema.json"

# --- 6. tamper: finding image not in provenance -------------------------------
python3 - "$tmp/reviews/report.json" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
r['images'] = [{'state_id': 'x', 'file': 'x.png', 'sha256': '0'*64, 'size': 1}]
json.dump(r, open(sys.argv[1], 'w'))
PY
set +e
python3 "$CHECK" --config "$tmp/va.toml" --report "$tmp/reviews/report.json" --current-commit "$HEAD" >/dev/null 2>&1
rc=$?
set -e
expect_rc 1 $rc "tamper rejected"

# --- 7. malformed output: schema rejection via verify-json --------------------
cat > "$tmp/bad-finding.json" <<'EOF'
{"schema":"ralph-visual-audit-review/v1","state_id":"x","image_sha256":"not-a-hash",
 "role":"diagram","model":"m","prompt_sha256":"bad","schema_sha256":"bad","verdict":"finding",
 "observations":[{"code":"LOWER_case","severity":"x","description":""}]}
EOF
set +e
python3 "$REVIEW" --config "$tmp/va.toml" verify-json --finding "$tmp/bad-finding.json" >/dev/null 2>&1
rc=$?
set -e
expect_rc 1 $rc "malformed finding rejected"

# --- 8. missing report fails closed -------------------------------------------
set +e
python3 "$CHECK" --config "$tmp/va.toml" --report "$tmp/nope.json" >/dev/null 2>&1
rc=$?
set -e
expect_rc 1 $rc "missing report fails closed"

# --- 9. shared-session race: concurrent lease holder refuses second capture ---
# The holder keeps the fd open while sleeping so the lock stays held.
flock "$tmp/lease2" -c 'sleep 5' &
holder_pid=$!
sleep 2
set +e
python3 "$LEASE" acquire --lock "$tmp/lease2" --owner contender >/dev/null 2>&1
rc=$?
set -e
expect_rc 1 $rc "lease race refused"
kill "$holder_pid" 2>/dev/null || true
wait "$holder_pid" 2>/dev/null || true

# --- 10. mutable state lives under the ignored .factory-state/visual-audit/ ---
grep -q '^capture_dir = ".factory/artifacts/visual-audit/captures"' "$tmp/.factory/visual-audit.toml" \
    || fail "config capture_dir must retain Controller artifact path"
grep -q '^review_dir = ".factory/artifacts/visual-audit/reviews"' "$tmp/.factory/visual-audit.toml" \
    || fail "config review_dir must retain Controller artifact path"
grep -q '^lease_file = ".factory-state/visual-audit.lease"' "$tmp/.factory/visual-audit.toml" \
    || fail "config lease_file must retain Controller state path"
grep -q '^enabled = false' "$tmp/.factory/visual-audit.toml" || fail "config must default disabled"
grep -q '^vision_model = "ollama/kimi-k2.7-code"' "$tmp/.factory/visual-audit.toml" || fail "config vision_model must retain configured Kimi model"
git check-ignore -q .factory-state/visual-audit/captures/good-main.png \
    || fail "capture dir is not git-ignored"
git check-ignore -q .factory-state/visual-audit/reviews/report.json \
    || fail "review dir is not git-ignored"
git check-ignore -q .factory-state/visual-audit/lease \
    || fail "lease file is not git-ignored"

# --- 11. calibration blocks a blind (always-pass) model -----------------------
mkdir -p "$tmp/captures/calibration"
write_png "$tmp/captures/calibration/cal-known-bad-blank.png" 64 32 0,0,0
write_png "$tmp/captures/calibration/cal-known-bad-clipped.png" 64 32 255,255,255
write_png "$tmp/captures/calibration/cal-current-bad-regression.png" 64 32 120,0,120
write_png "$tmp/captures/calibration/cal-reviewed-good-reference.png" 64 32 40,180,40
cat > "$tmp/blind-driver.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
out_dir=""; state_id=""; role=""; sha=""; model=""; psha=""; ssha=""; nonce=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out-dir) out_dir=$2; shift 2 ;;
        --state-id) state_id=$2; shift 2 ;;
        --role) role=$2; shift 2 ;;
        --expected-sha256) sha=$2; shift 2 ;;
        --prompt-sha256) psha=$2; shift 2 ;;
        --schema-sha256) ssha=$2; shift 2 ;;
        --model) model=$2; shift 2 ;;
        --request-nonce) nonce=$2; shift 2 ;;
        *) shift ;;
    esac
done
[[ -n "$nonce" ]] || { echo "blind-driver: --request-nonce is required" >&2; exit 2; }
mkdir -p "$out_dir"
python3 - "$out_dir" "$state_id" "$role" "$sha" "$model" "$psha" "$ssha" "$nonce" <<'PY'
import hashlib, json, os, sys, time
out_dir, state_id, role, sha, model, psha, ssha, nonce = sys.argv[1:]
finding = {"schema":"ralph-visual-audit-review/v1","state_id":state_id,"image_sha256":sha,
 "role":role,"model":model,"prompt_sha256":psha,"schema_sha256":ssha,
 "request_nonce":nonce,"verdict":"pass",
 "observations":[{"code":"ALWAYS_PASS","severity":"info","description":"pass"}]}
finding_path = f"{out_dir}/finding-{state_id}-{role}.json"
receipt_path = f"{out_dir}/receipt-{state_id}-{role}.json"
started = int(time.time() * 1000)
finding_bytes = (json.dumps(finding, indent=2) + "\n").encode("utf-8")
open(finding_path, "wb").write(finding_bytes)
os.chmod(finding_path, 0o600)
raw = json.dumps(finding)
finished = int(time.time() * 1000)
receipt = {
    "schema": "ralph-visual-audit-invocation/v1", "request_nonce": nonce,
    "state_id": state_id, "role": role, "image_sha256": sha,
    "prompt_sha256": psha, "schema_sha256": ssha, "model": model,
    "raw_response_sha256": hashlib.sha256(raw.encode()).hexdigest(),
    "finding_sha256": hashlib.sha256(finding_bytes).hexdigest(),
    "started_at_ms": started, "finished_at_ms": finished,
    "elapsed_ms": finished - started}
receipt_bytes = (json.dumps(receipt, indent=2, sort_keys=True) + "\n").encode("utf-8")
open(receipt_path, "wb").write(receipt_bytes)
os.chmod(receipt_path, 0o600)
PY
EOF
chmod +x "$tmp/blind-driver.sh"
set +e
VISUAL_AUDIT_SDK_DRIVER="$tmp/blind-driver.sh" python3 "$REVIEW" --config "$tmp/va.toml" calibrate >/dev/null 2>&1
rc=$?
set -e
expect_rc 1 $rc "blind model calibration blocked"

# --- 12. model outage: driver missing fails closed ----------------------------
rm -f "$tmp/mock-driver.sh"
set +e
VISUAL_AUDIT_SDK_DRIVER="$tmp/gone-driver.sh" python3 "$REVIEW" --config "$tmp/va.toml" run >/dev/null 2>&1
rc=$?
set -e
expect_rc 1 $rc "model outage fails closed"

# --- 13. product capture adapter retains its strict argv contract --------------
# The Controller adapter is real product WIP, not the generic fail-closed
# placeholder; invoking it here would launch display/product processes.
set +e
"$PROJECT_ROOT/scripts/visual-capture-driver.sh" state-primary >"$tmp/driver.out" 2>&1
rc=$?
set -e
expect_rc 64 $rc "capture driver usage gate"

# --- 13b. product adapter invariants: correct window/mode/cleanup ------------
# Regression guards for the previously-diagnosed adapter defects (BUG-0018
# installed visual capture): the adapter must poll (boundedly) for the real
# production window titles, pass --overlay-service (never the legacy invalid
# flag), select a private collision-free display (never a fixed :N with a
# global pkill), and clean up its whole owned process group (never a
# single-PID kill). These source-level invariants keep the fix from regressing
# without needing the installed binary or a display server.
DRIVER_SRC="$PROJECT_ROOT/scripts/visual-capture-driver.sh"
grep -q 'Controller-Box Manager' "$DRIVER_SRC" \
    || fail "adapter must poll for the manager title 'Controller-Box Manager'"
grep -q 'Controller-Box Overlay' "$DRIVER_SRC" \
    || fail "adapter must poll for the overlay title 'Controller-Box Overlay'"
grep -q -- '--overlay-service' "$DRIVER_SRC" \
    || fail "adapter must use --overlay-service for the overlay state"
bad_mode=$(grep -oE -- '--overlay[a-z-]*' "$DRIVER_SRC" | sort -u \
              | grep -v -- '^--overlay-service$' || true)
[[ -z "$bad_mode" ]] || fail "adapter must only pass --overlay-service, found: $bad_mode"
grep -q 'WINDOW_TIMEOUT' "$DRIVER_SRC" \
    || fail "adapter window polling must be time-bounded"
grep -vE '^[[:space:]]*#' "$DRIVER_SRC" | grep -q 'search --sync' \
    && fail "adapter must not use unbounded xdotool search --sync"
grep -qE 'pkill[[:space:]].*Xvfb' "$DRIVER_SRC" \
    && fail "adapter must not globally pkill Xvfb"
grep -q 'pick_display' "$DRIVER_SRC" \
    || fail "adapter must pick a private collision-free display"
grep -q 'setsid' "$DRIVER_SRC" \
    || fail "adapter must run children in their own process group"
grep -qF -- 'kill -- "-$' "$DRIVER_SRC" \
    || fail "adapter must TERM/KILL the whole owned process group"
# Wrong-state hardening (BUG-0018 remediation): the adapter must navigate via
# deterministic coordinate clicks (never fragile Right/Return that left the
# profile list on screen while claiming the editor), reacquire the exact window
# after navigation, and fail closed unless the captured frame semantically
# matches the requested state.
grep -q 'xdotool mousemove' "$DRIVER_SRC" \
    || fail "adapter must navigate via deterministic coordinate clicks"
grep -q 'validate_state' "$DRIVER_SRC" \
    || fail "adapter must semantically validate the captured state"
grep -q 'wrong-state capture' "$DRIVER_SRC" \
    || fail "adapter must fail closed on a wrong-state capture"
grep -vE '^[[:space:]]*#' "$DRIVER_SRC" | grep -q 'xdotool key Right' \
    && fail "adapter must not rely on keyboard Right navigation (wrong-state risk)"
grep -vE '^[[:space:]]*#' "$DRIVER_SRC" | grep -q 'xdotool key Return' \
    && fail "adapter must not rely on keyboard Return navigation (wrong-state risk)"
grep -q 'controller diagram absent' "$DRIVER_SRC" \
    || fail "adapter must reject a manager-editor frame without the controller diagram"
grep -q 'convert' "$DRIVER_SRC" \
    || fail "adapter must depend on convert for semantic frame validation"
# Blank-overlay + unselected-list fail-closed hardening (the two additional
# false-positive states found by the retained-capture audit): the adapter must
# reject a uniform/blank overlay frame and a profile list with no selected row.
grep -q 'overlay frame is blank/uniform' "$DRIVER_SRC" \
    || fail "adapter must reject a blank overlay frame for overlay-active"
grep -q 'no selected profile row' "$DRIVER_SRC" \
    || fail "adapter must reject an unselected profile list for manager-profiles"
grep -q 'row_selected' "$DRIVER_SRC" \
    || fail "adapter must implement the selected-row semantic check"

# --- 13c. product adapter hanging-child: bounded poll + owned group cleanup ---
# Runs the real adapter against a mock installed prefix (no build tree). The
# mock installed binary never opens the expected window and ignores TERM, so
# the adapter's bounded window polling must fail closed within a bounded wall
# time and its owned process-group cleanup must reap both the mock app (the
# "hanging child") and the Xvfb it started. Skips only when the display tools
# are unavailable.
if command -v Xvfb >/dev/null 2>&1 && command -v xdotool >/dev/null 2>&1 \
        && command -v import >/dev/null 2>&1; then
    MOCK_PREFIX="$tmp/mock-install"
    mkdir -p "$MOCK_PREFIX/bin" "$MOCK_PREFIX/share/controller-box/icons/svg"
    printf 'placeholder' > "$MOCK_PREFIX/share/controller-box/icons/svg/generic-gamepad.svg"
    cat > "$MOCK_PREFIX/bin/controller-box" <<'EOF'
#!/usr/bin/env bash
# Mock installed binary that never opens the expected window and ignores TERM,
# so the adapter's bounded poll must fail closed and its owned-group cleanup
# must SIGKILL this process after the TERM grace. Records its pid/starttime for
# the test's gone-assertion.
proc_st() {
    awk '{print $22}' "/proc/$$/stat" 2>/dev/null || true
}
printf 'mock app started (pid %s)\n' "$$"
echo "MOCKPID=$$ self_st=$(proc_st)" >> "${CAPB_DRIVER_APPPID:-/dev/null}"
trap '' TERM
while :; do sleep 300; done
EOF
    chmod +x "$MOCK_PREFIX/bin/controller-box"
    : > "$tmp/capdrv.pids"
    export CAPB_DRIVER_APPPID="$tmp/capdrv.pids"
    CAP_HEAD=$(git -C "$PROJECT_ROOT" rev-parse HEAD)
    xvfb_before=$(pgrep -fc 'Xvfb :9' 2>/dev/null || true)
    start=$(date +%s)
    set +e
    VISUAL_AUDIT_INSTALL_PREFIX="$MOCK_PREFIX" \
    VISUAL_AUDIT_WINDOW_TIMEOUT=4 \
    "$PROJECT_ROOT/scripts/visual-capture-driver.sh" manager-main \
        "$tmp/no-window.png" "$CAP_HEAD" >"$tmp/capdrv.out" 2>&1
    rc=$?
    set -e
    elapsed=$(( $(date +%s) - start ))
    [[ $elapsed -le 45 ]] || fail "adapter did not bound the window wait (${elapsed}s)"
    expect_rc 1 $rc "missing-window capture fails closed"
    grep -qi 'not found' "$tmp/capdrv.out" || fail "adapter must report the missing window"    [[ ! -e "$tmp/no-window.png" ]] || fail "failed adapter left a partial image"
    app_pid=$(grep -o 'MOCKPID=[0-9]*' "$tmp/capdrv.pids" | cut -d= -f2)
    app_st=$(grep -o 'self_st=[0-9]*' "$tmp/capdrv.pids" | cut -d= -f2)
    [[ -n "$app_pid" && -n "$app_st" ]] || fail "mock app did not record its identity"
    assert_gone "$app_pid" "$app_st" "adapter-owned mock app (hanging child)"
    xvfb_after=$(pgrep -fc 'Xvfb :9' 2>/dev/null || true)
    [[ "$xvfb_after" -le "$xvfb_before" ]] \
        || fail "adapter leaked Xvfb processes (before=$xvfb_before after=$xvfb_after)"
    unset CAPB_DRIVER_APPPID
else
    echo "SKIP: Xvfb/xdotool/import unavailable for the hanging-child adapter test"
fi

# --- 13d. product adapter wrong-state: real installed + semantic validation ---
# Runs the REAL installed controller-box under Xvfb, drives each manager state
# through the adapter's deterministic coordinate clicks, and asserts the
# adapter's semantic validation: the editor capture must genuinely contain the
# controller diagram (not the profile list), and the validator must REJECT a
# wrong-state frame (the BUG-0018 defect: the profile list captured as the
# editor). Skips only when the display tools or an installed production binary
# are unavailable. This is the installed-adapter regression for the wrong-state
# capture defect.
if command -v Xvfb >/dev/null 2>&1 && command -v xdotool >/dev/null 2>&1 \
        && command -v import >/dev/null 2>&1 && command -v convert >/dev/null 2>&1; then
    CAP_PREFIX=""
    for cand in "${VISUAL_AUDIT_INSTALL_PREFIX:-}" "$PROJECT_ROOT/.test-install/usr" \
            "$PROJECT_ROOT/.test-install"; do
        if [[ -n "$cand" && -x "$cand/bin/controller-box" ]]; then
            CAP_PREFIX="$cand"; break
        fi
    done
    if [[ -z "$CAP_PREFIX" && -x "$PROJECT_ROOT/build-check/controller-box" ]]; then
        # Install from an existing build dir to a test-owned prefix so the
        # adapter exercises the real installed artifact.
        INSTP="$tmp/installed-prefix"
        if cmake --install "$PROJECT_ROOT/build-check" --prefix "$INSTP" >/dev/null 2>&1 \
                && [[ -x "$INSTP/bin/controller-box" ]]; then
            CAP_PREFIX="$INSTP"
        fi
    fi
    if [[ -n "$CAP_PREFIX" ]]; then
        CAP_HEAD=$(git -C "$PROJECT_ROOT" rev-parse HEAD)
        for st in manager-main manager-profiles manager-editor; do
            set +e
            VISUAL_AUDIT_INSTALL_PREFIX="$CAP_PREFIX" \
            "$PROJECT_ROOT/scripts/visual-capture-driver.sh" "$st" \
                "$tmp/inst-$st.png" "$CAP_HEAD" >"$tmp/inst-$st.out" 2>&1
            rc=$?
            set -e
            expect_rc 0 $rc "installed adapter captures $st with real semantic validation"
            [[ -s "$tmp/inst-$st.png" ]] || fail "installed adapter produced no image for $st"
            grep -q "captured $st" "$tmp/inst-$st.out" \
                || fail "installed adapter did not report a successful $st capture"
        done
        # The wrong-state frame (profile list) must be REJECTED as the editor:
        # feed the freshly captured profiles frame through the validator as
        # manager-editor via the test-only hook and assert it fails closed.
        set +e
        RALPH_VISUAL_AUDIT_TESTING=1 CBX_VISUAL_VALIDATE_ONLY="$tmp/inst-manager-profiles.png" \
        "$PROJECT_ROOT/scripts/visual-capture-driver.sh" manager-editor \
            "$tmp/inst-wrong.png" "$CAP_HEAD" >"$tmp/inst-wrong.out" 2>&1
        rc=$?
        set -e
        [[ $rc -ne 0 ]] || fail "adapter accepted the wrong-state (profile-list-as-editor) capture"
        grep -qi "state validation FAILED" "$tmp/inst-wrong.out" \
            || fail "wrong-state rejection must report the failing semantic check"
        echo "test-visual-audit: installed-adapter wrong-state regression passed (13d)"
    else
        echo "SKIP: no installed production binary available for the installed-adapter test"
    fi
else
    echo "SKIP: Xvfb/xdotool/import/convert unavailable for the installed-adapter test"
fi

# --- 13e. non-skipping negative regressions: blank overlay + unselected list --
# The exact-commit retained-capture audit found two more false-positive states:
# a uniform/blank overlay frame (rc=0 for a 250-byte uniform black PNG despite
# the inventory requiring active icon/status content) and a profile list with no
# visibly selected/highlighted row (validator only checked the button row). The
# validator must fail closed on both. These run through the test-only
# validation hook, which needs only ImageMagick `convert` (no display server,
# no installed binary). Under the declared Nix project environment
# (shell.nix provides ImageMagick) they are genuinely non-skipping: a missing
# convert fails the run rather than skipping. At host level (outside
# nix-shell) boilerplate verification stays usable by reporting an explicit
# tool-unavailable skip, consistent with the 13c/13d installed-adapter skip
# policy. scripts/verify-project.sh re-executes under nix-shell and asserts
# convert is present, so this negative regression can never silently skip in
# the complete project verification.
if command -v convert >/dev/null 2>&1; then
NEG_HEAD=$(git -C "$PROJECT_ROOT" rev-parse HEAD)
NEG="$tmp/negatives"; mkdir -p "$NEG"
# Uniform black overlay frame must be rejected as overlay-active.
convert -size 1280x720 xc:black "$NEG/ov-blank.png"
set +e
RALPH_VISUAL_AUDIT_TESTING=1 CBX_VISUAL_VALIDATE_ONLY="$NEG/ov-blank.png" \
    "$PROJECT_ROOT/scripts/visual-capture-driver.sh" overlay-active \
    "$NEG/out.png" "$NEG_HEAD" >"$tmp/neg-ov.out" 2>&1
rc=$?
set -e
[[ $rc -ne 0 ]] || fail "validator accepted a blank overlay frame as overlay-active"
grep -qi "blank/uniform" "$tmp/neg-ov.out" \
    || fail "blank overlay rejection must report the blank/uniform frame"
# Profile list with the button row but no selected/highlighted row must be
# rejected as manager-profiles (button-row presence alone is not enough).
convert -size 1280x720 xc:"rgb(30,30,42)" \
    -fill "rgb(20,20,30)" -draw "rectangle 16,500 115,543" \
    -fill "rgb(240,240,250)" -draw "rectangle 116,500 215,543" \
    -fill "rgb(50,50,70)" -draw "rectangle 216,500 315,543" \
    -fill "rgb(255,255,255)" -draw "rectangle 316,500 415,543" \
    "$NEG/unselected.png"
set +e
RALPH_VISUAL_AUDIT_TESTING=1 CBX_VISUAL_VALIDATE_ONLY="$NEG/unselected.png" \
    "$PROJECT_ROOT/scripts/visual-capture-driver.sh" manager-profiles \
    "$NEG/out.png" "$NEG_HEAD" >"$tmp/neg-up.out" 2>&1
rc=$?
set -e
[[ $rc -ne 0 ]] || fail "validator accepted an unselected profile list as manager-profiles"
grep -qi "no selected profile row" "$tmp/neg-up.out" \
    || fail "unselected list rejection must report the missing selected row"
# The same button row with row 0 genuinely highlighted must still PASS, so the
# selection check is not rejecting every list.
convert -size 1280x720 xc:"rgb(30,30,42)" \
    -fill "rgb(42,42,58)" -draw "rectangle 16,64 1264,96" \
    -fill "rgb(20,20,30)" -draw "rectangle 16,500 115,543" \
    -fill "rgb(240,240,250)" -draw "rectangle 116,500 215,543" \
    -fill "rgb(50,50,70)" -draw "rectangle 216,500 315,543" \
    -fill "rgb(255,255,255)" -draw "rectangle 316,500 415,543" \
    "$NEG/selected.png"
set +e
RALPH_VISUAL_AUDIT_TESTING=1 CBX_VISUAL_VALIDATE_ONLY="$NEG/selected.png" \
    "$PROJECT_ROOT/scripts/visual-capture-driver.sh" manager-profiles \
    "$NEG/out.png" "$NEG_HEAD" >"$tmp/neg-sel.out" 2>&1
rc=$?
set -e
expect_rc 0 $rc "validator accepts a profile list with a selected row"
echo "test-visual-audit: non-skipping blank-overlay + unselected-list negative regressions passed (13e)"
elif [[ ${CBX_VERIFY_IN_NIX_SHELL:-0} == 1 || -n ${IN_NIX_SHELL:-} ]]; then
    fail "ImageMagick convert is required for the visual validator (Nix project environment)"
else
    echo "SKIP: ImageMagick convert unavailable for the non-skipping negative regressions (13e)"
fi

# --- 13f. atomic capture publication: only validated frames reach OUTPUT -------
# The driver must capture to an exclusively-owned temp in the SAME directory as
# OUTPUT, validate there, and rename to OUTPUT only after semantic success. A
# rejected capture (wrong state), an import failure, or a terminating signal
# must leave NO partial bytes at OUTPUT and no orphaned temp/receipt, and must
# never delete a pre-existing/unowned OUTPUT (including one reached via a
# symlink/hardlink). To make the capture deterministic we override `import` on
# PATH to write a chosen PNG to its target path while the REAL installed binary
# still opens the real window; the real-capture path is covered by 13d.
if command -v Xvfb >/dev/null 2>&1 && command -v xdotool >/dev/null 2>&1 \
        && command -v convert >/dev/null 2>&1; then
    ATOM_PREFIX=""
    for cand in "${VISUAL_AUDIT_INSTALL_PREFIX:-}" "$PROJECT_ROOT/.test-install/usr" \
            "$PROJECT_ROOT/.test-install"; do
        if [[ -n "$cand" && -x "$cand/bin/controller-box" ]]; then
            ATOM_PREFIX="$cand"; break
        fi
    done
    if [[ -z "$ATOM_PREFIX" && -x "$PROJECT_ROOT/build-check/controller-box" ]]; then
        ATOM_INSTP="$tmp/atomic-prefix"
        if cmake --install "$PROJECT_ROOT/build-check" --prefix "$ATOM_INSTP" >/dev/null 2>&1 \
                && [[ -x "$ATOM_INSTP/bin/controller-box" ]]; then
            ATOM_PREFIX="$ATOM_INSTP"
        fi
    fi
    if [[ -n "$ATOM_PREFIX" ]]; then
        ATOM_HEAD=$(git -C "$PROJECT_ROOT" rev-parse HEAD)
        ATOM_DIR="$tmp/atomic"
        mkdir -p "$ATOM_DIR"
        # Valid manager-main frame (uniform: btn/std and diag/std both <=0.09).
        convert -size 1280x720 xc:"rgb(30,30,42)" "$ATOM_DIR/frame-ok.png"
        # Wrong-state manager-main frame (profile-list button row present =>
        # btn std ~0.41 > 0.09, fails the main-view guard).
        convert -size 1280x720 xc:"rgb(30,30,42)" \
            -fill "rgb(20,20,30)" -draw "rectangle 16,500 115,543" \
            -fill "rgb(240,240,250)" -draw "rectangle 116,500 215,543" \
            -fill "rgb(50,50,70)" -draw "rectangle 216,500 315,543" \
            -fill "rgb(255,255,255)" -draw "rectangle 316,500 415,543" \
            "$ATOM_DIR/frame-wrong.png"
        # Mock `import` on PATH: writes MOCK_IMPORT_SRC to its target path (the
        # last arg, stripping an optional png: prefix). MOCK_IMPORT_FAIL makes
        # it fail like a real capture error; MOCK_IMPORT_HOLD makes it park so a
        # terminating signal can be delivered mid-capture.
        mkdir -p "$tmp/fakebin"
        cat > "$tmp/fakebin/import" <<'EOF'
#!/usr/bin/env bash
if [ "${MOCK_IMPORT_FAIL:-0}" = 1 ]; then echo "mock import failed" >&2; exit 1; fi
target=""
for a in "$@"; do target="$a"; done
target="${target#png:}"
if [ -n "${MOCK_IMPORT_HOLD:-}" ]; then
    touch "$MOCK_IMPORT_MARKER"
    for _i in $(seq 1 100); do [ -e "$MOCK_IMPORT_RELEASE" ] && break; sleep 0.1; done
    [ -e "$MOCK_IMPORT_RELEASE" ] && exit 1
    exit 0
fi
cp -- "$MOCK_IMPORT_SRC" "$target" 2>/dev/null || exit 1
EOF
        chmod +x "$tmp/fakebin/import"
        export PATH="$tmp/fakebin:$PATH"

        # (1) semantic rejection: wrong-state frame must leave NO output, no
        # receipt, no temp.
        set +e
        MOCK_IMPORT_SRC="$ATOM_DIR/frame-wrong.png" \
        VISUAL_AUDIT_INSTALL_PREFIX="$ATOM_PREFIX" \
        "$PROJECT_ROOT/scripts/visual-capture-driver.sh" manager-main \
            "$ATOM_DIR/reject.png" "$ATOM_HEAD" >"$tmp/atom-reject.out" 2>&1
        rc=$?
        set -e
        [[ $rc -ne 0 ]] || fail "atomic publish accepted a wrong-state capture"
        [[ ! -e "$ATOM_DIR/reject.png" ]] \
            || fail "rejected capture left partial bytes at OUTPUT"
        [[ ! -e "$ATOM_DIR/reject.png.receipt.json" ]] \
            || fail "rejected capture left a receipt sidecar"
        [[ -z "$(find "$ATOM_DIR" -maxdepth 1 -name '.cbx-capture.*' -o -maxdepth 1 -name '.cbx-receipt.*' | head -n 1)" ]] \
            || fail "rejected capture left an owned temp behind"
        grep -qi "wrong-state capture" "$tmp/atom-reject.out" \
            || fail "semantic rejection must report the wrong-state capture"

        # (2) import failure: no output, no receipt, no temp.
        set +e
        MOCK_IMPORT_FAIL=1 VISUAL_AUDIT_INSTALL_PREFIX="$ATOM_PREFIX" \
        "$PROJECT_ROOT/scripts/visual-capture-driver.sh" manager-main \
            "$ATOM_DIR/importerr.png" "$ATOM_HEAD" >"$tmp/atom-import.out" 2>&1
        rc=$?
        set -e
        [[ $rc -ne 0 ]] || fail "atomic publish survived an import failure"
        [[ ! -e "$ATOM_DIR/importerr.png" ]] \
            || fail "import failure left partial bytes at OUTPUT"
        [[ ! -e "$ATOM_DIR/importerr.png.receipt.json" ]] \
            || fail "import failure left a receipt sidecar"
        [[ -z "$(find "$ATOM_DIR" -maxdepth 1 -name '.cbx-capture.*' -o -maxdepth 1 -name '.cbx-receipt.*' | head -n 1)" ]] \
            || fail "import failure left an owned temp behind"

        # (3) pre-existing output protection: a rejected capture must never
        # delete or overwrite a pre-existing (unowned) OUTPUT.
        printf 'PREEXISTING-SENTINEL' > "$ATOM_DIR/sentinel.png"
        set +e
        MOCK_IMPORT_SRC="$ATOM_DIR/frame-wrong.png" \
        VISUAL_AUDIT_INSTALL_PREFIX="$ATOM_PREFIX" \
        "$PROJECT_ROOT/scripts/visual-capture-driver.sh" manager-main \
            "$ATOM_DIR/sentinel.png" "$ATOM_HEAD" >"$tmp/atom-sentinel.out" 2>&1
        rc=$?
        set -e
        [[ $rc -ne 0 ]] || fail "rejected capture unexpectedly succeeded against a pre-existing output"
        grep -q 'PREEXISTING-SENTINEL' "$ATOM_DIR/sentinel.png" \
            || fail "rejected capture deleted or overwrote a pre-existing OUTPUT"

        # (4) symlink/hardlink protection: OUTPUT as a symlink/hardlink to a
        # target with sentinel content; a rejected capture must not clobber it.
        printf 'TARGET-SENTINEL' > "$ATOM_DIR/target.png"
        ln -s "$ATOM_DIR/target.png" "$ATOM_DIR/symlink.png"
        ln "$ATOM_DIR/target.png" "$ATOM_DIR/hardlink.png"
        for link in symlink.png hardlink.png; do
            set +e
            MOCK_IMPORT_SRC="$ATOM_DIR/frame-wrong.png" \
            VISUAL_AUDIT_INSTALL_PREFIX="$ATOM_PREFIX" \
            "$PROJECT_ROOT/scripts/visual-capture-driver.sh" manager-main \
                "$ATOM_DIR/$link" "$ATOM_HEAD" >"$tmp/atom-$link.out" 2>&1
            rc=$?
            set -e
            [[ $rc -ne 0 ]] || fail "rejected capture unexpectedly succeeded on $link"
            grep -q 'TARGET-SENTINEL' "$ATOM_DIR/target.png" \
                || fail "rejected capture clobbered the target behind $link"
            [[ -L "$ATOM_DIR/symlink.png" ]] || fail "rejected capture replaced the symlink with a regular file"
        done

        # (5) signals: a terminating signal delivered mid-capture must remove
        # the owned temp and preserve a pre-existing OUTPUT.
        printf 'SIGNAL-SENTINEL' > "$ATOM_DIR/signal.png"
        : > "$ATOM_DIR/hold.marker"
        rm -f "$ATOM_DIR/release"
        set +e
        MOCK_IMPORT_SRC="$ATOM_DIR/frame-ok.png" MOCK_IMPORT_HOLD=1 \
        MOCK_IMPORT_MARKER="$ATOM_DIR/hold.marker" \
        MOCK_IMPORT_RELEASE="$ATOM_DIR/release" \
        VISUAL_AUDIT_INSTALL_PREFIX="$ATOM_PREFIX" \
        "$PROJECT_ROOT/scripts/visual-capture-driver.sh" manager-main \
            "$ATOM_DIR/signal.png" "$ATOM_HEAD" >"$tmp/atom-signal.out" 2>&1 &
        DRV_PID=$!
        # wait for the mock import to park mid-capture, then deliver TERM.
        for _i in $(seq 1 100); do
            [ -e "$ATOM_DIR/hold.marker" ] && break
            kill -0 "$DRV_PID" 2>/dev/null || break
            sleep 0.1
        done
        [ -e "$ATOM_DIR/hold.marker" ] || fail "driver never reached the capture park point"
        kill -TERM "$DRV_PID" 2>/dev/null || true
        wait "$DRV_PID" 2>/dev/null; rc=$?
        set -e
        # Release the parked mock import so it does not linger.
        touch "$ATOM_DIR/release"
        [[ $rc -ne 0 ]] || fail "driver did not exit non-zero on a terminating signal"
        grep -q 'SIGNAL-SENTINEL' "$ATOM_DIR/signal.png" \
            || fail "terminated capture deleted or overwrote a pre-existing OUTPUT"
        [[ -z "$(find "$ATOM_DIR" -maxdepth 1 -name '.cbx-capture.*' -o -maxdepth 1 -name '.cbx-receipt.*' | head -n 1)" ]] \
            || fail "terminated capture left an owned temp behind"

        # (6) successful atomic publish: a validated capture is published with
        # its receipt, and no temp remains.
        set +e
        MOCK_IMPORT_SRC="$ATOM_DIR/frame-ok.png" \
        VISUAL_AUDIT_INSTALL_PREFIX="$ATOM_PREFIX" \
        "$PROJECT_ROOT/scripts/visual-capture-driver.sh" manager-main \
            "$ATOM_DIR/publish.png" "$ATOM_HEAD" >"$tmp/atom-publish.out" 2>&1
        rc=$?
        set -e
        expect_rc 0 $rc "atomic publish of a validated capture"
        cmp -s "$ATOM_DIR/publish.png" "$ATOM_DIR/frame-ok.png" \
            || fail "published OUTPUT does not match the validated frame"
        [[ -f "$ATOM_DIR/publish.png.receipt.json" ]] \
            || fail "successful publish did not write a receipt sidecar"
        python3 -c "import json,sys; json.load(open('$ATOM_DIR/publish.png.receipt.json'))" \
            || fail "published receipt is not valid JSON"
        [[ -z "$(find "$ATOM_DIR" -maxdepth 1 -name '.cbx-capture.*' -o -maxdepth 1 -name '.cbx-receipt.*' | head -n 1)" ]] \
            || fail "successful publish left an owned temp behind"
        echo "test-visual-audit: atomic capture publication passed (13f)"
        PATH="${PATH#"$tmp/fakebin:"}"
    else
        echo "SKIP: no installed production binary available for the atomic-publish test"
    fi
else
    echo "SKIP: Xvfb/xdotool/convert unavailable for the atomic-publish test"
fi

# --- 14. capture: ok driver succeeds and is deterministic ---------------------
cat > "$tmp/mock-cap-ok.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
python3 - "$2" <<'PY'
import struct, sys, zlib
path = sys.argv[1]
w, h = 8, 8
def chunk(t, d):
    c = t + d
    return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
raw = b''.join(b'\x00' + bytes((40, 180, 40)) * w for _ in range(h))
data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
open(path, 'wb').write(data)
PY
EOF
chmod +x "$tmp/mock-cap-ok.sh"
mkdir -p "$tmp/cap-captures"
chmod 700 "$tmp/cap-captures"
write_capture_inv "$tmp/cap-ok-inventory.json" ok-state
capture_config "$tmp/cap-ok.toml" "$tmp/mock-cap-ok.sh" "$tmp/cap-captures" "$tmp/cap-lease" "$tmp/cap-ok-inventory.json" 30 5
set +e
VISUAL_AUDIT_CONFIG="$tmp/cap-ok.toml" python3 "$CAPTURE" >"$tmp/cap-ok.out" 2>&1
rc=$?
set -e
expect_rc 0 $rc "capture succeeds with an ok mock driver"
[[ -f "$tmp/cap-captures/ok-state.png" ]] || fail "capture did not produce the ok-state image"
set +e
VISUAL_AUDIT_CONFIG="$tmp/cap-ok.toml" python3 "$CAPTURE" >"$tmp/cap-ok2.out" 2>&1
rc=$?
set -e
expect_rc 0 $rc "deterministic capture rerun"
grep -q "already captured; skipping" "$tmp/cap-ok2.out" || fail "deterministic skip must be reported"

# --- 15. capture hang: driver spawns child then hangs; group reaped ------------
cat > "$tmp/cap-hang-inventory.json" <<'JSON'
{"schema":"ralph-visual-audit-inventory/v1","states":[{"id":"hang-state","risk":"high","navigation":["launch"],"expected":"hang","crops":["full-frame"]}]}
JSON
cat > "$tmp/mock-cap-hang.sh" <<'EOF'
#!/usr/bin/env bash
# Mock capture driver that spawns a child in the same session/group, writes a
# partial image, then hangs while ignoring TERM, so the runner must TERM then
# KILL the whole group. Records self/child pids + /proc starttimes for the test.
set -euo pipefail
state=$1; output=$2; commit=$3
proc_st() {
    local p=$1 s=""
    for _ in 1 2 3 4 5; do
        s=$(awk '{print $22}' "/proc/$p/stat" 2>/dev/null || true)
        [[ -n "$s" ]] && break
        sleep 0.05
    done
    printf '%s' "$s"
}
echo "self=$$ self_st=$(proc_st $$)" >> "${CAP_DRIVER_PIDFILE:-/dev/null}"
sleep 300 &
child=$!
echo "child=$child child_st=$(proc_st "$child")" >> "${CAP_DRIVER_PIDFILE:-/dev/null}"
printf 'partial-not-a-png' > "$output"
trap '' TERM
while :; do sleep 300; done
EOF
chmod +x "$tmp/mock-cap-hang.sh"
capture_config "$tmp/cap-hang.toml" "$tmp/mock-cap-hang.sh" "$tmp/cap-captures" "$tmp/cap-lease" "$tmp/cap-hang-inventory.json" 3 1
: > "$tmp/cap.pids"
export CAP_DRIVER_PIDFILE="$tmp/cap.pids"
start=$(date +%s)
set +e
VISUAL_AUDIT_CONFIG="$tmp/cap-hang.toml" python3 "$CAPTURE" >"$tmp/cap-hang.out" 2>&1
rc=$?
set -e
elapsed=$(( $(date +%s) - start ))
[[ $elapsed -le 20 ]] || fail "hang capture not bounded (${elapsed}s)"
expect_rc 1 $rc "hang capture fails closed"
grep -qi "timed out" "$tmp/cap-hang.out" || fail "capture must report the timeout"
[[ ! -e "$tmp/cap-captures/hang-state.png" ]] || fail "partial capture image left behind"
self=$(grep -o 'self=[0-9]*' "$tmp/cap.pids" | cut -d= -f2)
self_st=$(grep -o 'self_st=[0-9]*' "$tmp/cap.pids" | cut -d= -f2)
child=$(grep -o 'child=[0-9]*' "$tmp/cap.pids" | cut -d= -f2)
child_st=$(grep -o 'child_st=[0-9]*' "$tmp/cap.pids" | cut -d= -f2)
[[ -n "$self" && -n "$child" ]] || fail "hang driver did not record its pids"
assert_gone "$self" "$self_st" "capture driver"
assert_gone "$child" "$child_st" "capture driver child"
# The dedicated lease is immediately reacquirable after the failure.
python3 "$LEASE" acquire --lock "$tmp/cap-lease" --owner retest >/dev/null
python3 "$LEASE" release --lock "$tmp/cap-lease" >/dev/null
unset CAP_DRIVER_PIDFILE

# --- 16. capture: nonzero driver removes its partial and fails cleanly --------
cat > "$tmp/mock-cap-fail.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'partial-before-exit' > "$2"
echo "capture exploded for state $1" >&2
exit 7
EOF
chmod +x "$tmp/mock-cap-fail.sh"
write_capture_inv "$tmp/cap-fail-inventory.json" fail-state
capture_config "$tmp/cap-fail.toml" "$tmp/mock-cap-fail.sh" "$tmp/cap-captures" "$tmp/cap-lease" "$tmp/cap-fail-inventory.json" 10 2
set +e
VISUAL_AUDIT_CONFIG="$tmp/cap-fail.toml" python3 "$CAPTURE" >"$tmp/cap-fail.out" 2>&1
rc=$?
set -e
expect_rc 1 $rc "nonzero capture driver fails closed"
grep -q "driver failed" "$tmp/cap-fail.out" || fail "capture must report the driver failure"
[[ ! -e "$tmp/cap-captures/fail-state.png" ]] || fail "nonzero driver left a partial image behind"

# --- 17. malformed and missing images fail closed and are removed -------------
cat > "$tmp/mock-cap-badpng.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'this is definitely not a png' > "$2"
EOF
chmod +x "$tmp/mock-cap-badpng.sh"
write_capture_inv "$tmp/cap-badpng-inventory.json" badpng-state
capture_config "$tmp/cap-badpng.toml" "$tmp/mock-cap-badpng.sh" "$tmp/cap-captures" "$tmp/cap-lease" "$tmp/cap-badpng-inventory.json" 10 2
set +e
VISUAL_AUDIT_CONFIG="$tmp/cap-badpng.toml" python3 "$CAPTURE" >"$tmp/cap-badpng.out" 2>&1
rc=$?
set -e
expect_rc 1 $rc "malformed image fails closed"
grep -q "malformed" "$tmp/cap-badpng.out" || fail "capture must report the malformed image"
[[ ! -e "$tmp/cap-captures/badpng-state.png" ]] || fail "malformed image left behind"
cat > "$tmp/mock-cap-noimg.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
exit 0
EOF
chmod +x "$tmp/mock-cap-noimg.sh"
write_capture_inv "$tmp/cap-noimg-inventory.json" noimg-state
capture_config "$tmp/cap-noimg.toml" "$tmp/mock-cap-noimg.sh" "$tmp/cap-captures" "$tmp/cap-lease" "$tmp/cap-noimg-inventory.json" 10 2
set +e
VISUAL_AUDIT_CONFIG="$tmp/cap-noimg.toml" python3 "$CAPTURE" >"$tmp/cap-noimg.out" 2>&1
rc=$?
set -e
expect_rc 1 $rc "missing image fails closed"
grep -q "produced no image" "$tmp/cap-noimg.out" || fail "capture must report the missing image"

# --- 18. invalid state ids are rejected before any path is built --------------
cat > "$tmp/cap-badid-inventory.json" <<'JSON'
{"schema":"ralph-visual-audit-inventory/v1","states":[{"id":"../../escape","risk":"high","navigation":[],"expected":"x","crops":[]}]}
JSON
capture_config "$tmp/cap-badid.toml" "$tmp/mock-cap-ok.sh" "$tmp/cap-captures" "$tmp/cap-lease" "$tmp/cap-badid-inventory.json" 10 2
set +e
VISUAL_AUDIT_CONFIG="$tmp/cap-badid.toml" python3 "$CAPTURE" >"$tmp/cap-badid.out" 2>&1
rc=$?
set -e
expect_rc 1 $rc "invalid state id fails closed"
grep -q "invalid state id" "$tmp/cap-badid.out" || fail "capture must report the invalid state id"
[[ ! -e "$tmp/cap-captures/escape.png" ]] || fail "escape path must not be constructed"

# --- 19. capture timeout/grace ceilings are enforced --------------------------
write_capture_inv "$tmp/cap-ok-inventory.json" ok-state
capture_config "$tmp/cap-badtimeout.toml" "$tmp/mock-cap-ok.sh" "$tmp/cap-captures" "$tmp/cap-lease" "$tmp/cap-ok-inventory.json" 121 2
set +e
VISUAL_AUDIT_CONFIG="$tmp/cap-badtimeout.toml" python3 "$CAPTURE" >"$tmp/cap-badtimeout.out" 2>&1
rc=$?
set -e
expect_rc 1 $rc "capture timeout ceiling enforced"
grep -q "capture_timeout_seconds" "$tmp/cap-badtimeout.out" || fail "capture must report the bad timeout config"
capture_config "$tmp/cap-badgrace.toml" "$tmp/mock-cap-ok.sh" "$tmp/cap-captures" "$tmp/cap-lease" "$tmp/cap-ok-inventory.json" 10 9
set +e
VISUAL_AUDIT_CONFIG="$tmp/cap-badgrace.toml" python3 "$CAPTURE" >"$tmp/cap-badgrace.out" 2>&1
rc=$?
set -e
expect_rc 1 $rc "capture grace ceiling enforced"
grep -q "capture_cleanup_grace_seconds" "$tmp/cap-badgrace.out" || fail "capture must report the grace ceiling"

# --- 20. probe: model-less fails closed; hard timeout and pass paths ----------
sed -i 's|vision_model = "ollama/kimi-k2.7-code"|vision_model = ""|' "$tmp/.factory/visual-audit.toml"
set +e
VISUAL_AUDIT_VISION_MODEL="" "$PROBE" >"$tmp/probe-nomodel.out" 2>&1
rc=$?
set -e
sed -i 's|vision_model = ""|vision_model = "ollama/kimi-k2.7-code"|' "$tmp/.factory/visual-audit.toml"
expect_rc 64 $rc "probe without a model fails closed"
grep -q "VISUAL_AUDIT_VISION_MODEL" "$tmp/probe-nomodel.out" || fail "probe must name the model requirement"
cat > "$tmp/pass-sdk.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
out_dir=""; state_id=""; role=""; sha=""; model=""; psha=""; ssha=""; nonce=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out-dir) out_dir=$2; shift 2 ;;
        --state-id) state_id=$2; shift 2 ;;
        --role) role=$2; shift 2 ;;
        --expected-sha256) sha=$2; shift 2 ;;
        --prompt-sha256) psha=$2; shift 2 ;;
        --schema-sha256) ssha=$2; shift 2 ;;
        --model) model=$2; shift 2 ;;
        --request-nonce) nonce=$2; shift 2 ;;
        *) shift ;;
    esac
done
[[ -n "$nonce" ]] || { echo "pass-sdk: --request-nonce is required" >&2; exit 2; }
mkdir -p "$out_dir"
python3 - "$out_dir" "$state_id" "$role" "$sha" "$model" "$psha" "$ssha" "$nonce" <<'PY'
import hashlib, json, os, sys, time
out_dir, state_id, role, sha, model, psha, ssha, nonce = sys.argv[1:]
finding = {"schema":"ralph-visual-audit-review/v1","state_id":state_id,"image_sha256":sha,
 "role":role,"model":model,"prompt_sha256":psha,"schema_sha256":ssha,
 "request_nonce":nonce,"verdict":"pass",
 "observations":[{"code":"PROBE_COLOR","severity":"info","description":"solid red rectangle"}]}
finding_path = f"{out_dir}/finding-{state_id}-{role}.json"
receipt_path = f"{out_dir}/receipt-{state_id}-{role}.json"
started = int(time.time() * 1000)
finding_bytes = (json.dumps(finding, indent=2) + "\n").encode("utf-8")
open(finding_path, "wb").write(finding_bytes)
os.chmod(finding_path, 0o600)
raw = json.dumps(finding)
finished = int(time.time() * 1000)
receipt = {
    "schema": "ralph-visual-audit-invocation/v1", "request_nonce": nonce,
    "state_id": state_id, "role": role, "image_sha256": sha,
    "prompt_sha256": psha, "schema_sha256": ssha, "model": model,
    "raw_response_sha256": hashlib.sha256(raw.encode()).hexdigest(),
    "finding_sha256": hashlib.sha256(finding_bytes).hexdigest(),
    "started_at_ms": started, "finished_at_ms": finished,
    "elapsed_ms": finished - started}
receipt_bytes = (json.dumps(receipt, indent=2, sort_keys=True) + "\n").encode("utf-8")
open(receipt_path, "wb").write(receipt_bytes)
os.chmod(receipt_path, 0o600)
PY
EOF
chmod +x "$tmp/pass-sdk.sh"
set +e
VISUAL_AUDIT_VISION_MODEL=test/model VISUAL_AUDIT_MODEL_TIMEOUT=30 \
VISUAL_AUDIT_SDK_DRIVER="$tmp/pass-sdk.sh" "$PROBE" >"$tmp/probe-pass.out" 2>&1
rc=$?
set -e
expect_rc 0 $rc "probe passes with a mock model"
grep -q "PASS" "$tmp/probe-pass.out" || fail "probe must report PASS"
cat > "$tmp/hang-sdk.sh" <<'EOF'
#!/usr/bin/env bash
# Mock SDK driver that ignores TERM and hangs forever.
trap '' TERM
while :; do sleep 300; done
EOF
chmod +x "$tmp/hang-sdk.sh"
start=$(date +%s)
set +e
VISUAL_AUDIT_VISION_MODEL=test/model VISUAL_AUDIT_MODEL_TIMEOUT=3 \
VISUAL_AUDIT_SDK_DRIVER="$tmp/hang-sdk.sh" "$PROBE" >"$tmp/probe-hang.out" 2>&1
rc=$?
set -e
elapsed=$(( $(date +%s) - start ))
[[ $elapsed -le 20 ]] || fail "probe model timeout not bounded (${elapsed}s)"
expect_rc 124 $rc "probe model hard timeout fails closed"
grep -q "timeout" "$tmp/probe-hang.out" || fail "probe must report the model timeout"
probe_dir="$tmp/.factory-state/visual-audit"
if [[ ! -d "$probe_dir" ]]; then
    leftover=0
else
    leftover=$(find "$probe_dir" -maxdepth 1 -name 'probe.*' 2>/dev/null | wc -l)
fi
[[ $leftover -eq 0 ]] || fail "probe left work dirs behind"

# ============================================================================
# C3c visual-audit-gate.sh completion-gate cases. The production gate is
# check-only: a disabled config exits 0 without touching anything; an enabled
# config with a valid current report exits 0; a missing report, a report with
# findings, a stale-commit report, and any test-only execution override must
# exit non-zero. The gate must never capture, review, or invoke the vision
# model, so a before/after snapshot of the capture dir, the review dir, and
# the SDK invocation sentinel must be byte-identical across every gate run.
# ============================================================================
GATE="$tmp/scripts/visual-audit-gate.sh"
[[ -f "$GATE" ]] || fail "visual-audit-gate.sh must be copied into the temp repo"
write_mock_driver   # the earlier model-outage case removed the mock driver

gate_run() { # label want-rc [want-diag]
    local label="$1" want_rc="$2" want_diag="${3:-}" rc
    set +e
    env -u RALPH_VISUAL_AUDIT_TESTING -u VISUAL_AUDIT_CONFIG \
        -u VISUAL_AUDIT_SDK_DRIVER -u VISUAL_AUDIT_VISION_MODEL \
        "$GATE" >"$tmp/gate.out" 2>&1
    rc=$?
    set -e
    expect_rc "$want_rc" "$rc" "$label"
    if [[ -n "$want_diag" ]]; then
        grep -q "$want_diag" "$tmp/gate.out" \
            || fail "$label: missing diagnostic: $want_diag"
    fi
}

# --- C3c-g1: disabled config exits 0 ------------------------------------------
gate_run "gate disabled config" 0 "disabled"

# --- build an enabled tracked config with a valid current report -------------
# Install the deterministic counting/mock SDK driver as a tracked repo-relative
# script under $tmp/scripts, then commit an enabled config that references that
# tracked driver and keeps every mutable path under the ignored
# .factory-state/visual-audit/ (captures/reviews/calibration/probe receipts) so
# probe/calibrate/capture/review bind the committed inputs.
cp "$tmp/mock-driver.sh" "$tmp/scripts/mock-driver.sh"
chmod +x "$tmp/scripts/mock-driver.sh"
cat > "$tmp/scripts/counting-driver.sh" <<EOF
#!/usr/bin/env bash
echo "counting" >> "$tmp/sdk-count"
exec "$tmp/scripts/mock-driver.sh" "\$@"
EOF
chmod +x "$tmp/scripts/counting-driver.sh"
sed -e 's/enabled = false/enabled = true/' \
    -e 's|vision_model = "ollama/kimi-k2.7-code"|vision_model = "test-model"|' \
    -e 's|sdk_driver = "scripts/visual-audit-review-sdk.mjs"|sdk_driver = "scripts/counting-driver.sh"|' \
    -e 's|capture_dir = ".factory/artifacts/visual-audit/captures"|capture_dir = ".factory-state/visual-audit/captures"|' \
    -e 's|review_dir = ".factory/artifacts/visual-audit/reviews"|review_dir = ".factory-state/visual-audit/reviews"|' \
    -e 's|lease_file = ".factory-state/visual-audit.lease"|lease_file = ".factory-state/visual-audit/lease"|' \
    "$tmp/.factory/visual-audit.toml" > "$tmp/gate-config.toml"
mv "$tmp/gate-config.toml" "$tmp/.factory/visual-audit.toml"
git add .factory/visual-audit.toml scripts/counting-driver.sh scripts/mock-driver.sh
git commit -qm gate-config
GATE_HEAD=$(git rev-parse HEAD)
GATE_DRIVER="$tmp/scripts/counting-driver.sh"

# The default capture fixtures and their calibration controls must exist at the
# committed config's repo-relative capture_dir BEFORE probe/calibrate: the
# calibration binds the declared set's exact per-image sha256 (these PNGs are
# byte-identical to the base-commit calibration manifest, so the hashes match).
gcap="$tmp/.factory-state/visual-audit/captures"
mkdir -p "$gcap/calibration"
chmod 700 "$gcap" "$gcap/calibration"
write_png "$gcap/calibration/cal-known-bad-blank.png" 64 32 0,0,0
write_png "$gcap/calibration/cal-known-bad-clipped.png" 64 32 255,255,255
write_png "$gcap/calibration/cal-current-bad-regression.png" 64 32 120,0,120
write_png "$gcap/calibration/cal-reviewed-good-reference.png" 64 32 30,150,90

# Real production probe/calibrate/capture/review for a valid current report
# under the tracked gate configuration (no hand-written bypass receipts).
VISUAL_AUDIT_SDK_DRIVER="$GATE_DRIVER" VISUAL_AUDIT_VISION_MODEL=test-model \
    "$PROBE" >"$tmp/gate-probe.out" 2>&1
grep -q "PASS" "$tmp/gate-probe.out" || fail "gate fixture probe must PASS"
VISUAL_AUDIT_SDK_DRIVER="$GATE_DRIVER" python3 "$REVIEW" \
    --config .factory/visual-audit.toml calibrate >/dev/null 2>&1
[ -f "$tmp/.factory-state/visual-audit/calibration-receipt.json" ] \
    || fail "gate fixture calibration did not produce its receipt"
write_png "$gcap/good-main.png" 64 32 40,180,40
write_png "$gcap/good-secondary.png" 64 32 40,180,40
python3 "$PY3" manifest --out "$gcap" --commit "$GATE_HEAD" --tree "$(git rev-parse 'HEAD^{tree}')" >/dev/null
python3 "$PY3" verify --out "$gcap" || fail "gate fixture provenance verify failed"
VISUAL_AUDIT_SDK_DRIVER="$GATE_DRIVER" python3 "$REVIEW" \
    --config .factory/visual-audit.toml run >/dev/null 2>&1
[ -f "$tmp/.factory-state/visual-audit/reviews/report.json" ] \
    || fail "gate fixture did not produce its report"

# Fingerprint of every capture/review file plus the SDK invocation sentinel.
gate_snapshot() {
    { find "$tmp/.factory-state/visual-audit" -type f -print0 2>/dev/null \
        | sort -z | xargs -0 sha256sum 2>/dev/null
      [[ -f "$tmp/sdk-count" ]] && wc -l < "$tmp/sdk-count" || true; } \
    | md5sum | cut -d' ' -f1
}
SNAP_BEFORE=$(gate_snapshot)

# --- C3c-g2: enabled config with a valid current report exits 0 ---------------
gate_run "gate enabled valid report" 0 "report valid"

# --- C3c-g3: enabled config with a missing report fails closed -----------
cp "$tmp/.factory-state/visual-audit/reviews/report.json" "$tmp/gate-report-valid.json"
rm "$tmp/.factory-state/visual-audit/reviews/report.json"
gate_run "gate enabled missing report" 1 "required review report missing"
cp "$tmp/gate-report-valid.json" "$tmp/.factory-state/visual-audit/reviews/report.json"

# --- C3c-g4: enabled config with a finding report fails closed -------------
python3 - "$tmp/.factory-state/visual-audit/reviews/report.json" <<'PY'
import json, sys
p = sys.argv[1]
r = json.load(open(p))
r["findings"][0]["verdict"] = "finding"
r["findings"][0]["observations"] = [{"code": "REAL_DEFECT", "severity": "high",
                                      "description": "material visual defect"}]
json.dump(r, open(p, "w"))
PY
gate_run "gate enabled finding report" 1 "finding(s)/error(s) reported"
cp "$tmp/gate-report-valid.json" "$tmp/.factory-state/visual-audit/reviews/report.json"

# --- C3c-g5: stale-commit report fails closed -------------------------------
git commit --allow-empty -qm gate-stale
gate_run "gate stale commit report" 1 "report replay/commit mismatch"
git reset --hard HEAD~1

# --- C3c-g6: every test-only execution override is rejected -------------------
for var in VISUAL_AUDIT_CONFIG VISUAL_AUDIT_SDK_DRIVER VISUAL_AUDIT_VISION_MODEL \
        RALPH_VISUAL_AUDIT_TESTING; do
    set +e
    env "$var=x" "$GATE" >"$tmp/gate-ov.out" 2>&1
    rc=$?
    set -e
    expect_rc 1 "$rc" "gate rejects override $var"
    grep -q "test-only execution override" "$tmp/gate-ov.out" \
        || fail "gate must reject the $var override"
done

# --- C3c-g: the gate never captures/reviews/invokes the model -------------------
[[ "$(gate_snapshot)" == "$SNAP_BEFORE" ]] \
    || fail "gate mutated capture/review state or invoked the model"
echo "test-visual-audit: C3c visual-audit-gate.sh cases passed"

echo "test-visual-audit: all adversarial cases passed"
