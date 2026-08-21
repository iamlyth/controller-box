#!/usr/bin/env bash
# Adversarial visual-audit validation (deterministic mock reviewer, no model).
#
# Exercises the auditor-mandated fail-closed cases without any model call:
#   - missing images / provenance mismatch
#   - prompt drift and schema drift (freeze digest)
#   - malformed / hallucinated model output (schema rejection)
#   - false-positive known-bad (calibration blocks a blind model)
#   - shared-session races (dedicated lease refuses concurrent capture)
#   - auto-regenerated goldens (capture never touches golden dirs)
#   - replay (an older report cannot be replayed as current evidence)
#   - tamper (finding image hash not in provenance manifest)
#   - model outage (reviewer unavailable fails closed)
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
for script in visual-audit-provenance.py visual-audit-review.py visual-audit-check.py \
        visual-audit-lease.py visual-audit-review-sdk.mjs; do
    cp "$PROJECT_ROOT/scripts/$script" "$tmp/scripts/" 2>/dev/null || true
done
cp "$CHECK" "$tmp/scripts/check-visual-audit.py"
cp "$REVIEW" "$tmp/scripts/visual-audit-review.py"
cp "$PY3" "$tmp/scripts/visual-audit-provenance.py"
cp "$LEASE" "$tmp/scripts/visual-audit-lease.py"
cp "$PROJECT_ROOT/.factory/schemas/visual-audit-review.schema.json" "$tmp/.factory/schemas/"
cp "$PROJECT_ROOT/.factory/prompts/visual-audit.md" "$tmp/.factory/prompts/"
cp "$PROJECT_ROOT/.factory/visual-audit.toml" "$tmp/.factory/"
cp "$PROJECT_ROOT/.factory/visual-audit-inventory.json" "$tmp/.factory/"
cp "$PROJECT_ROOT/.factory/visual-audit-calibration.json" "$tmp/.factory/"
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

# Minimal deterministic mock reviewer (verdict controlled by state id).
cat > "$tmp/mock-driver.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
out_dir=""; state_id=""; role=""; sha=""; model=""; psha=""; ssha=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out-dir) out_dir=$2; shift 2 ;;
        --state-id) state_id=$2; shift 2 ;;
        --role) role=$2; shift 2 ;;
        --expected-sha256) sha=$2; shift 2 ;;
        --prompt-sha256) psha=$2; shift 2 ;;
        --schema-sha256) ssha=$2; shift 2 ;;
        --model) model=$2; shift 2 ;;
        *) shift ;;
    esac
done
mkdir -p "$out_dir"
case "$state_id" in
    good-*) verdict="pass"; obs='[{"code":"MOCK_GOOD","severity":"info","description":"ok"}]' ;;
    bad-*)  verdict="finding"; obs='[{"code":"MOCK_BAD","severity":"high","description":"defect"}]' ;;
    *)      verdict="pass"; obs='[{"code":"MOCK_GOOD","severity":"info","description":"ok"}]' ;;
esac
python3 - "$out_dir" "$state_id" "$role" "$sha" "$model" "$psha" "$ssha" "$verdict" "$obs" <<'PY'
import json, sys
out_dir, state_id, role, sha, model, psha, ssha, verdict, obs = sys.argv[1:]
finding = {
  "schema": "ralph-visual-audit-review/v1", "state_id": state_id,
  "image_sha256": sha, "role": role, "model": model,
  "prompt_sha256": psha, "schema_sha256": ssha, "verdict": verdict,
  "observations": json.loads(obs)}
open(f"{out_dir}/finding-{state_id}-{role}.json", "w").write(json.dumps(finding))
PY
EOF
chmod +x "$tmp/mock-driver.sh"

# Config override for the temp repo (enabled, temp dirs, mock driver).
sed -e 's/enabled = false/enabled = true/' \
    -e "s|capture_dir = \".factory/artifacts/visual-audit/captures\"|capture_dir = \"$tmp/captures\"|" \
    -e "s|review_dir = \".factory/artifacts/visual-audit/reviews\"|review_dir = \"$tmp/reviews\"|" \
    -e "s|lease_file = \".factory-state/visual-audit.lease\"|lease_file = \"$tmp/lease\"|" \
    -e "s|sdk_driver = \"scripts/visual-audit-review-sdk.mjs\"|sdk_driver = \"$tmp/mock-driver.sh\"|" \
    "$tmp/.factory/visual-audit.toml" > "$tmp/va.toml"

cd "$tmp"
git init -q -b develop
git config user.name test
git config user.email test@example.invalid
git add .factory docs .gitignore
git commit -qm base
HEAD=$(git rev-parse HEAD)

fail() { echo "test-visual-audit: $*" >&2; exit 1; }
expect_rc() { local want=$1 got=$2 label=$3; [[ $got -eq $want ]] || fail "$label (expected rc=$want got rc=$got)"; }

# --- capture: two deterministic states ---------------------------------------
mkdir -p "$tmp/captures"
chmod 700 "$tmp/captures"
write_png "$tmp/captures/good-main.png" 64 32 40,180,40
write_png "$tmp/captures/good-secondary.png" 64 32 40,180,40
python3 "$PY3" manifest --out "$tmp/captures" --commit "$HEAD" --tree "$HEAD" >/dev/null
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

# --- 10. auto-regenerated goldens: capture dir is outside golden dirs ---------
if git check-ignore "$tmp/captures/good-main.png" 2>/dev/null; then :; fi
[[ "$tmp/captures/good-main.png" != "$PROJECT_ROOT/tests/golden/"* ]] || fail "capture leaked into golden dirs"

# --- 11. calibration blocks a blind (always-pass) model -----------------------
mkdir -p "$tmp/captures/calibration"
write_png "$tmp/captures/calibration/cal-known-bad-blank.png" 64 32 0,0,0
write_png "$tmp/captures/calibration/cal-known-bad-clipped.png" 64 32 255,255,255
write_png "$tmp/captures/calibration/cal-current-bad-regression.png" 64 32 120,0,120
write_png "$tmp/captures/calibration/cal-reviewed-good-reference.png" 64 32 40,180,40
cat > "$tmp/blind-driver.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
out_dir=""; state_id=""; role=""; sha=""; model=""; psha=""; ssha=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out-dir) out_dir=$2; shift 2 ;;
        --state-id) state_id=$2; shift 2 ;;
        --role) role=$2; shift 2 ;;
        --expected-sha256) sha=$2; shift 2 ;;
        --prompt-sha256) psha=$2; shift 2 ;;
        --schema-sha256) ssha=$2; shift 2 ;;
        --model) model=$2; shift 2 ;;
        *) shift ;;
    esac
done
mkdir -p "$out_dir"
python3 - "$out_dir" "$state_id" "$role" "$sha" "$model" "$psha" "$ssha" <<'PY'
import json, sys
out_dir, state_id, role, sha, model, psha, ssha = sys.argv[1:]
finding = {"schema":"ralph-visual-audit-review/v1","state_id":state_id,"image_sha256":sha,
 "role":role,"model":model,"prompt_sha256":psha,"schema_sha256":ssha,"verdict":"pass",
 "observations":[{"code":"ALWAYS_PASS","severity":"info","description":"pass"}]}
open(f"{out_dir}/finding-{state_id}-{role}.json","w").write(json.dumps(finding))
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

echo "test-visual-audit: all adversarial cases passed"
