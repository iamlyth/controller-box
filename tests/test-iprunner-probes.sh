#!/usr/bin/env bash
# Adversarial iprunner probe checks: the inputplumber-system-dbus,
# physical-controller, and target-consumer probes must fail closed against
# private buses, wrong versions/interfaces, no fresh events, direct uinput
# injection, fabricated aliases, skipped consumers, stale devices, and
# incomplete cleanup. Fixtures replicate the exact state the live probes
# validate; a fixture that could produce a false pass is itself a failure.
set -euo pipefail
export FACTORY_CAMPAIGN_ID=synthetic-iprunner-probes
export FACTORY_READINESS_NONCE=3333333333333333333333333333333333333333333333333333333333333333

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

IP_VALIDATOR="$PROJECT_ROOT/scripts/iprunner-probes/validate-inputplumber-facts.py"
IP_PROBE="$PROJECT_ROOT/scripts/probe-inputplumber-system-dbus.sh"
PHYSICAL_PROBE="$PROJECT_ROOT/scripts/probe-physical-controller.sh"
TARGET_PROBE="$PROJECT_ROOT/scripts/probe-target-consumer.sh"
PHYSICAL_SOURCE="$PROJECT_ROOT/scripts/iprunner-probes/physical_controller_probe.c"
EXPECTATIONS="$PROJECT_ROOT/scripts/iprunner-probes/inputplumber-expectations.json"
DECODER="$PROJECT_ROOT/scripts/iprunner-probes/unwrap_variant.py"

command -v nix-shell >/dev/null || {
    echo "test: nix-shell required for the iprunner probe fixtures" >&2
    exit 1
}

must_fail() {
    local label=$1
    shift
    set +e
    "$@" >/dev/null 2>&1
    local rc=$?
    set -e
    [[ $rc -ne 0 ]] || { echo "test: $label unexpectedly passed" >&2; exit 1; }
}

must_pass() {
    local label=$1
    shift
    "$@" >/dev/null 2>&1
}

# ---------------------------------------------------------------------------
# inputplumber-system-dbus probe: fixture facts
# ---------------------------------------------------------------------------
good_facts() {
    python3 - "$EXPECTATIONS" <<'PY'
import json, sys
exp = json.load(open(sys.argv[1], encoding="utf-8"))
facts = {
    "schema": "iprunner-inputplumber-facts/v1",
    "dbus_system_bus_address_env": "unix:path=/run/factory/dbus/system_bus_socket",
    "bus_address_effective": "unix:path=/run/factory/dbus/system_bus_socket",
    "package": {"name": "inputplumber", "version": "0.78.0-1", "installed": True},
    "binary": {"path": "/usr/bin/inputplumber", "exists": True, "executable": True, "owned_by_package": True},
    "service": {"unit": "inputplumber.service", "active": True, "type": "dbus", "exec_start_binary": "/usr/bin/inputplumber"},
    "name_owner": {"has_owner": True, "unique_owner": ":1.42"},
    "manager": {
        "path": "/org/shadowblip/InputPlumber/Manager",
        "interface": "org.shadowblip.InputManager",
        "version": "0.78.0",
        "methods": {
            "CreateCompositeDevice": {"in": "s", "out": "s"},
            "CreateTargetDevice": {"in": "s", "out": "s"},
            "AttachTargetDevice": {"in": "ss", "out": ""},
            "StopTargetDevice": {"in": "s", "out": ""},
        },
        "gamepad_order_signature": "as",
        "gamepad_order_writable": True,
        "supported_target_device_ids": ["xb360", "deck"],
    },
    "interfaces_present": [
        "org.shadowblip.InputManager",
        "org.shadowblip.Input.Target",
        "org.shadowblip.Input.CompositeDevice",
        "org.shadowblip.Input.Source.EventDevice",
    ],
    "object_manager": {
        "objects": 3,
        "path": "/org/shadowblip/InputPlumber",
        "interface": "org.freedesktop.DBus.ObjectManager",
    },
}
print(json.dumps(facts))
PY
}

# Raw, realistic busctl/systemctl shapes used by the live collector. These
# are synthetic parser fixtures, never real capability acceptance.
[[ "$(printf '%s' '{"type":"s","data":[":1.42"]}' | python3 "$DECODER" --string-method)" == '":1.42"' ]]
[[ "$(printf '%s' '{"type":"s","data":"0.78.0"}' | python3 "$DECODER" --property-s)" == '"0.78.0"' ]]
[[ "$(printf '%s' '{"type":"as","data":["xb360","deck"]}' | python3 "$DECODER" --property-as)" == '["xb360", "deck"]' ]]
printf '%s' '{"type":"a{oa{sa{sv}}}","data":[{"/org/shadowblip/InputPlumber/Manager":{"org.shadowblip.InputManager":{}},"/org/shadowblip/InputPlumber/Target/0":{"org.shadowblip.Input.Target":{}}}]}' | \
    python3 "$DECODER" --object-manager > "$tmp/raw-om.json"
[[ "$(python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1]))))' "$tmp/raw-om.json")" -eq 2 ]]
structured_exec='{ path=/usr/bin/inputplumber ; argv[]=/usr/bin/inputplumber --log-level info ; ignore_errors=no ; start_time=[n/a] ; stop_time=[n/a] ; pid=0 ; code=(null) ; status=0/0 }'
[[ "$(printf '%s' "$structured_exec" | python3 "$DECODER" --exec-start)" == '"/usr/bin/inputplumber"' ]]
[[ "$(printf '%s' '/usr/bin/inputplumber --log-level info' | python3 "$DECODER" --exec-start)" == '"/usr/bin/inputplumber"' ]]
for entry in \
  '{"type":"s","data":[":1.42",":1.43"]}' \
  '{"type":"s","data":":1.42","extra":true}' \
  '{"type":"as","data":"xb360"}'; do
    must_fail "malformed raw busctl envelope" bash -c \
        "printf '%s' '$entry' | python3 '$DECODER' --string-method || printf '%s' '$entry' | python3 '$DECODER' --property-as"
done
must_fail "ambiguous structured ExecStart" bash -c \
    "printf '%s' '{ path=/usr/bin/inputplumber ; } { path=/usr/bin/evil ; }' | python3 '$DECODER' --exec-start"
must_fail "relative ExecStart" bash -c \
    "printf '%s' 'inputplumber --daemon' | python3 '$DECODER' --exec-start"

# The honest baseline: perfectly pinned facts pass the validator.
good_facts > "$tmp/good-facts.json"
must_pass "pinned inputplumber facts" python3 "$IP_VALIDATOR" --facts "$tmp/good-facts.json"

# A real/direct or unapproved private bus is rejected: only the broker proxy
# endpoint is accepted.
good_facts | python3 -c 'import json,sys; d=json.load(sys.stdin); d["dbus_system_bus_address_env"]="unix:path=/tmp/private"; print(json.dumps(d))' > "$tmp/private-env.json"
must_fail "private bus via env override" python3 "$IP_VALIDATOR" --facts "$tmp/private-env.json"
good_facts | python3 -c 'import json,sys; d=json.load(sys.stdin); d["bus_address_effective"]="unix:path=/tmp/private"; print(json.dumps(d))' > "$tmp/private-addr.json"
must_fail "private bus via effective address" python3 "$IP_VALIDATOR" --facts "$tmp/private-addr.json"

# Wrong version (manager and package) is rejected.
good_facts | python3 -c 'import json,sys; d=json.load(sys.stdin); d["manager"]["version"]="0.77.0"; print(json.dumps(d))' > "$tmp/wrong-version.json"
must_fail "wrong manager version" python3 "$IP_VALIDATOR" --facts "$tmp/wrong-version.json"
good_facts | python3 -c 'import json,sys; d=json.load(sys.stdin); d["package"]["version"]="0.78.0-0"; print(json.dumps(d))' > "$tmp/wrong-package.json"
must_fail "wrong package version" python3 "$IP_VALIDATOR" --facts "$tmp/wrong-package.json"

# Wrong interface: missing method, wrong signature, non-writable GamepadOrder.
good_facts | python3 -c 'import json,sys; d=json.load(sys.stdin); del d["manager"]["methods"]["AttachTargetDevice"]; print(json.dumps(d))' > "$tmp/missing-method.json"
must_fail "missing manager method" python3 "$IP_VALIDATOR" --facts "$tmp/missing-method.json"
good_facts | python3 -c 'import json,sys; d=json.load(sys.stdin); d["manager"]["methods"]["CreateTargetDevice"]["out"]="o"; print(json.dumps(d))' > "$tmp/wrong-sig.json"
must_fail "wrong manager signature" python3 "$IP_VALIDATOR" --facts "$tmp/wrong-sig.json"
good_facts | python3 -c 'import json,sys; d=json.load(sys.stdin); d["manager"]["gamepad_order_writable"]=False; print(json.dumps(d))' > "$tmp/not-writable.json"
must_fail "GamepadOrder not writable" python3 "$IP_VALIDATOR" --facts "$tmp/not-writable.json"

# ObjectManager missing and fabricated alias are rejected.
good_facts | python3 -c 'import json,sys; d=json.load(sys.stdin); d["object_manager"]["objects"]=0; print(json.dumps(d))' > "$tmp/no-om.json"
must_fail "empty ObjectManager" python3 "$IP_VALIDATOR" --facts "$tmp/no-om.json"
good_facts | python3 -c 'import json,sys; d=json.load(sys.stdin); d["name_owner"]["unique_owner"]="org.shadowblip.InputPlumber"; print(json.dumps(d))' > "$tmp/alias.json"
must_fail "fabricated unique alias" python3 "$IP_VALIDATOR" --facts "$tmp/alias.json"

# The probe script refuses to run with DBUS_SYSTEM_BUS_ADDRESS set.
must_fail "probe refuses a private-bus env" env DBUS_SYSTEM_BUS_ADDRESS=unix:path=/tmp/private \
    bash "$IP_PROBE" --fixture-facts "$tmp/good-facts.json"

# ---------------------------------------------------------------------------
# physical-controller probe: SDL helper fixture mode (compiled in nix-shell)
# ---------------------------------------------------------------------------
PHYSICAL="$tmp/physical_probe"
nix-shell --run "cc -O2 -o '$PHYSICAL' '$PHYSICAL_SOURCE' \$(pkg-config --cflags --libs sdl2)"

cat > "$tmp/phys-pass.txt" <<'EOF'
device-present 1
instance 3
baseline 1000
event button 3 ts 1500 name A value 1.0
EOF
cat > "$tmp/phys-no-fresh.txt" <<'EOF'
device-present 1
instance 3
baseline 1000
event button 3 ts 500 name A value 1.0
EOF
cat > "$tmp/phys-injected.txt" <<'EOF'
device-present 1
instance 3
baseline 1000
event button 9 ts 1500 name A value 1.0
EOF
cat > "$tmp/phys-absent.txt" <<'EOF'
device-present 0
instance 3
baseline 1000
EOF

must_pass "fresh physical event fixture" "$PHYSICAL" --fixture "$tmp/phys-pass.txt"
must_fail "no fresh event (presence alone)" "$PHYSICAL" --fixture "$tmp/phys-no-fresh.txt"
must_fail "stale event before baseline" "$PHYSICAL" --fixture "$tmp/phys-no-fresh.txt"
must_fail "direct uinput injection (wrong instance)" "$PHYSICAL" --fixture "$tmp/phys-injected.txt"
must_fail "device absent" "$PHYSICAL" --fixture "$tmp/phys-absent.txt"

# The live SDL path with no physical pad must fail (never skip): presence is
# required AND a fresh event is required.
must_fail "live SDL path with no physical pad" nix-shell --run \
    "timeout 20 '$PHYSICAL' --vendor 0x045e --product 0x028e --event-path /dev/input/event5 --window 1"

# The probe wrapper refuses root and unknown arguments.
must_fail "probe refuses unknown arguments" bash "$PHYSICAL_PROBE" --bogus
# The wrapper compiles the helper through nix-shell and its fixture mode
# reaches the same helper logic.
cat > "$tmp/wrap-pass.txt" <<'EOF'
device-present 1
instance 3
baseline 1000
event axis 3 ts 1500 name LeftX value 0.5
EOF
must_pass "wrapper fixture pass" nix-shell --run \
    "bash '$PROJECT_ROOT/scripts/probe-physical-controller.sh' --fixture '$tmp/wrap-pass.txt' --window 1"

# ---------------------------------------------------------------------------
# target-consumer probe: fixture scenarios
# ---------------------------------------------------------------------------
tc_fixture() {
    local dir=$1
    mkdir -p "$dir"
    echo "/org/shadowblip/InputPlumber/devices/target/xb360-1" > "$dir/create-response"
    printf 'event0\nevent1\n' > "$dir/device-baseline"
    printf 'event0\nevent1\nevent5\n' > "$dir/device-after"
    printf 'Microsoft X-Box 360 pad\txb360\n' > "$dir/target-identity"
    printf 'event5\tMicrosoft X-Box 360 pad\tinput/input42\n' > "$dir/device-identities"
    printf 'event 1 304 1\n' > "$dir/event-stream"
    echo ok > "$dir/input-event-result"
    echo ok > "$dir/stop-result"
    echo ok > "$dir/restore-result"
    echo say > "$dir/input-event-signature"
}

tc_fixture "$tmp/tc-pass"
must_pass "target-consumer full fixture pass" bash "$TARGET_PROBE" --fixture "$tmp/tc-pass"
grep -q 'cleanup: stop-target=ok restore-gamepad-order=ok' "$tmp/tc-pass/cleanup.log"

# A failing InputEvent must fail the probe AND still run complete cleanup.
tc_fixture "$tmp/tc-eventfail"
echo fail > "$tmp/tc-eventfail/input-event-result"
must_fail "InputEvent API failure" bash "$TARGET_PROBE" --fixture "$tmp/tc-eventfail"
grep -q 'cleanup: stop-target=ok restore-gamepad-order=ok' "$tmp/tc-eventfail/cleanup.log"

# Missing cleanup (StopTargetDevice failure) fails the probe.
tc_fixture "$tmp/tc-noclean"
echo fail > "$tmp/tc-noclean/stop-result"
must_fail "missing target cleanup" bash "$TARGET_PROBE" --fixture "$tmp/tc-noclean"
grep -q 'stop-target=fail' "$tmp/tc-noclean/cleanup.log"

# Stale devices: no new kernel event device appears — rejected.
tc_fixture "$tmp/tc-stale"
printf 'event0\nevent1\n' > "$tmp/tc-stale/device-after"
must_fail "stale device tree (no new event device)" bash "$TARGET_PROBE" --fixture "$tmp/tc-stale"

# The exact DBus target identity must match one and only one newly appeared
# kernel event device. Wrong identities and duplicate matches are rejected;
# an unrelated concurrent node is ignored rather than selected first.
tc_fixture "$tmp/tc-wrong-identity"
printf 'event5\tUnrelated virtual input\tinput/input42\n' > "$tmp/tc-wrong-identity/device-identities"
must_fail "wrong target device identity" bash "$TARGET_PROBE" --fixture "$tmp/tc-wrong-identity"

tc_fixture "$tmp/tc-ambiguous"
printf 'event0\nevent1\nevent5\nevent6\n' > "$tmp/tc-ambiguous/device-after"
printf 'event5\tMicrosoft X-Box 360 pad\tinput/input42\nevent6\tMicrosoft X-Box 360 pad\tinput/input43\n' > "$tmp/tc-ambiguous/device-identities"
must_fail "ambiguous target device identity" bash "$TARGET_PROBE" --fixture "$tmp/tc-ambiguous"

tc_fixture "$tmp/tc-unrelated"
printf 'event0\nevent1\nevent4\nevent5\n' > "$tmp/tc-unrelated/device-after"
printf 'event4\tConcurrent unrelated input\tinput/input41\nevent5\tMicrosoft X-Box 360 pad\tinput/input42\n' > "$tmp/tc-unrelated/device-identities"
must_pass "unrelated concurrent device is not mistaken for target" bash "$TARGET_PROBE" --fixture "$tmp/tc-unrelated"

tc_fixture "$tmp/tc-wrong-type"
printf 'Microsoft X-Box 360 pad\tkeyboard\n' > "$tmp/tc-wrong-type/target-identity"
must_fail "wrong target DeviceType" bash "$TARGET_PROBE" --fixture "$tmp/tc-wrong-type"

# Creation failure still runs the cleanup path (nothing to stop, but the
# probe must fail and the journal must record the attempted path).
tc_fixture "$tmp/tc-createfail"
echo FAIL > "$tmp/tc-createfail/create-response"
must_fail "target creation failure" bash "$TARGET_PROBE" --fixture "$tmp/tc-createfail"

# A wrong InputEvent signature (API mismatch) fails honestly and undeclared.
tc_fixture "$tmp/tc-wrongsig"
echo s > "$tmp/tc-wrongsig/input-event-signature"
must_fail "wrong InputEvent signature" bash "$TARGET_PROBE" --fixture "$tmp/tc-wrongsig"

# NEGATIVE CONTROL (known-broken product, BUG-0015): the current product can
# hold the InputPlumber name, introspect the Manager, create a target, and
# accept an InputEvent call, yet delivers no kernel event to any consumer
# ("Topology incomplete: 0 of 4 virtual controllers active"). That state is
# NOT acceptance: introspection + API acceptance without an observable event
# must fail the probe. The full chain (create -> kernel device -> InputEvent
# -> exact event on an independent consumer fd) is required.
tc_fixture "$tmp/tc-noevent"
printf 'event 1 305 1\n' > "$tmp/tc-noevent/event-stream"
must_fail "API accepts InputEvent but no matching kernel event reaches the consumer (BUG-0015 negative control)" \
    bash "$TARGET_PROBE" --fixture "$tmp/tc-noevent"
grep -q 'cleanup: stop-target=ok restore-gamepad-order=ok' "$tmp/tc-noevent/cleanup.log"

# ---------------------------------------------------------------------------
# Skipped consumer: a receipt that shows a skipped consumer is unevidenced.
# Simulates the promotion state (capability declared + contract declared)
# against the real evidence checker with a skip token in the probe scope.
# ---------------------------------------------------------------------------
mkdir -p "$tmp/skipped-repo/scripts" "$tmp/skipped-repo/docs" "$tmp/skipped-repo/.factory/artifacts" \
    "$tmp/skipped-repo/.factory-state/runner-evidence/probe-runner"
cp "$PROJECT_ROOT/scripts/check-capability-contracts.py" \
   "$PROJECT_ROOT/scripts/check-capability-evidence.py" "$tmp/skipped-repo/scripts/"
chmod +x "$tmp/skipped-repo/scripts/"*.py
cat > "$tmp/skipped-repo/.factory/environment.toml" <<'EOF'
schema_version = 1
[[runners]]
name = "probe-runner"
transport = "ssh"
ssh_config_alias = "probe-runner"
working_directory = "/srv/dev-runner/workspaces/probe"
capabilities = ["target-consumer"]
verify_argv = ["./scripts/verify-project.sh"]
EOF
printf '#!/usr/bin/env bash\nexit 0\n' > "$tmp/skipped-repo/scripts/verify-project.sh"
chmod +x "$tmp/skipped-repo/scripts/verify-project.sh"
printf '# Spec\n' > "$tmp/skipped-repo/docs/SPEC.md"
printf '%s\n' ".factory-state/" > "$tmp/skipped-repo/.gitignore"
cat > "$tmp/skipped-repo/.factory/capability-contracts.json" <<'EOF'
{
  "schema": "ralph-capability-contract/v1",
  "capabilities": [
    {
      "name": "target-consumer",
      "status": "declared",
      "probe_argv": ["nix-shell", "--run", "bash scripts/probe-target-consumer.sh"],
      "probe_marker": "--- target-consumer capability contract (candidate) ---",
      "probe_stage": "post",
      "probe_stdout_contains": ["target-consumer-probe: PASS"],
      "probe_is_verify_run": false,
      "must_execute": true,
      "must_not_skip": ["Skipped", "Not Run", "skip", "OPTIONAL", "consumer skipped"],
      "deny_simulated_markers": ["private bus", "fixture-only", "simulated", "synthetic", "staged"]
    }
  ]
}
EOF
git -C "$tmp/skipped-repo" init -q -b develop
git -C "$tmp/skipped-repo" config user.name test
git -C "$tmp/skipped-repo" config user.email test@example.invalid
git -C "$tmp/skipped-repo" add .
git -C "$tmp/skipped-repo" commit -qm base
head=$(git -C "$tmp/skipped-repo" rev-parse HEAD)
mkdir -p "$tmp/skipped-repo/.factory-state/runner-evidence/probe-runner/$head"
printf '%s\n' '{"schema":"factory-runner-receipt/v1","result":"pass","exit_code":0}' > \
    "$tmp/skipped-repo/.factory-state/runner-evidence/probe-runner/$head/manifest.json"
cat > "$tmp/skipped-repo/.factory-state/runner-evidence/probe-runner/$head/stdout.log" <<'LOG'
--- target-consumer capability contract (candidate) ---
target-consumer-probe: consumer skipped
LOG
: > "$tmp/skipped-repo/.factory-state/runner-evidence/probe-runner/$head/stderr.log"
cat > "$tmp/skipped-repo/.factory-state/runner-evidence.json" <<AG
{
  "schema": "factory-runner-aggregate/v2",
  "campaign_id": "synthetic-iprunner-probes",
  "readiness_nonce": "3333333333333333333333333333333333333333333333333333333333333333",
  "commit": "$head",
  "runners": [
    {"name": "probe-runner", "manifest": ".factory-state/runner-evidence/probe-runner/$head/manifest.json", "capabilities": ["target-consumer"]}
  ]
}
AG
must_fail "skipped consumer receipt is unevidenced" \
    bash -c "cd '$tmp/skipped-repo' && ./scripts/check-capability-evidence.py"

echo "test: iprunner probe adversarial checks passed"
