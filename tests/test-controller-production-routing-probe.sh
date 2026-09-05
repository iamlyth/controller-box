#!/usr/bin/env bash
# Adversarial checks for the controller-production-routing probe
# (scripts/probe-controller-production-routing.sh). Deterministic fixture
# tests only: each scenario is a committed JSON fact bundle with artifact
# sha256 hashes that the probe validates, then drives through the SAME
# decision logic used live. A fixture that could produce a false pass is
# itself a failure.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
PROBE="$PROJECT_ROOT/scripts/probe-controller-production-routing.sh"
VALIDATOR="$PROJECT_ROOT/scripts/iprunner-probes/validate-production-routing-facts.py"
OBSERVER_SOURCE="$PROJECT_ROOT/scripts/iprunner-probes/routing_observer.c"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

command -v cc >/dev/null || {
    echo "test: cc required to compile the routing observer" >&2
    exit 1
}

must_fail() {
    local label=$1
    shift
    set +e
    "$@" > "$tmp/last.out" 2>&1
    local rc=$?
    set -e
    [[ $rc -ne 0 ]] || { echo "test: $label unexpectedly passed" >&2; exit 1; }
}

must_pass() {
    local label=$1
    shift
    "$@" > "$tmp/last.out" 2>&1
}

# ---------------------------------------------------------------------------
# Deterministic fixture builder. Writes facts.json + artifact files (with
# committed sha256 hashes) + the routing_observer event stream for one
# scenario described by a JSON document on stdin.
# ---------------------------------------------------------------------------
build_fixture() {
    local dir=$1 scen=$2
    mkdir -p "$dir"
    python3 - "$dir" "$scen" <<'PY'
import hashlib, json, os, sys
base = sys.argv[1]
scenario = json.load(open(sys.argv[2], encoding="utf-8"))
os.makedirs(base, exist_ok=True)

def write(path, data):
    with open(path, "wb") as f:
        f.write(data)

def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()

# Artifacts (screenshot + log) are always referenced by facts.json with a
# committed sha256. The builder can omit the screenshot file on disk or
# corrupt the log hash to exercise the missing/mismatched-hash scenarios,
# or point the path at a traversal/symlink escape.
artifacts = {}
if scenario.get("omit_artifact") != "screenshot":
    write(os.path.join(base, "screenshot.png"), os.urandom(256))
    artifacts["screenshot"] = {
        "path": "screenshot.png",
        "sha256": sha(os.path.join(base, "screenshot.png")),
    }
else:
    # Referenced but never created on disk: the validator must fail on the
    # missing artifact file.
    artifacts["screenshot"] = {"path": "screenshot.png", "sha256": "0" * 64}
if scenario.get("artifact_traversal"):
    artifacts["screenshot"]["path"] = "../escape.png"
if scenario.get("artifact_symlink"):
    os.symlink("/etc/passwd", os.path.join(base, "escape-link"))
    artifacts["screenshot"] = {"path": "escape-link", "sha256": "0" * 64}
write(os.path.join(base, "observer.log"), os.urandom(128))
artifacts["log"] = {
    "path": "observer.log",
    "sha256": sha(os.path.join(base, "observer.log")),
}
if scenario.get("corrupt_hash") == "log":
    artifacts["log"]["sha256"] = "0" * 64

# Event stream for the read-only routing observer (baseline marker first).
event_stream = scenario.get("event_stream", "")
with open(os.path.join(base, "event-stream"), "w", encoding="utf-8") as f:
    f.write("baseline 1000\n")
    f.write(event_stream)

source_path = scenario.get("source_path", "/org/shadowblip/InputPlumber/devices/source/event12")
base_source_times = scenario.get("source_times", [1500, 2500, 3500, 4500])
targets = []
for i in range(4):
    target = {
        "slot": i,
        "dbus_path": f"/org/shadowblip/InputPlumber/devices/target/gamepad{i}",
        "kernel_node": f"/dev/input/event{20+i}",
        "device_type": "xb360",
        "name": f"Xbox 360 Controller {i}",
        "composite_path": "/org/shadowblip/InputPlumber/CompositeDevice0",
        "source_path": source_path,
        "source_vidpid": scenario.get("source_vidpid", "045e:028e"),
        "assignment_verified": True,
        "consumer_read_only": True,
        "selected_only": not bool(scenario.get("cross_target_leakage", 0)),
        "production_dispatch": not bool(scenario.get("missing_production_dispatch", 0)),
        "production_save": not bool(scenario.get("missing_production_save", 0)),
        "direct_assignment_dbus": bool(scenario.get("direct_assignment_dbus", 0)),
        "direct_injection": bool(scenario.get("direct_injection", 0)),
        "source_event_us": base_source_times[i],
        "target_event_us": base_source_times[i] + 100,
        "observation_id": f"human-press-{i}",
    }
    targets.append(target)
if "target_mutation" in scenario:
    m=scenario["target_mutation"]; targets[m.get("slot",3)].update(m.get("values",{}))
if scenario.get("duplicate_mapping"):
    targets[3]["dbus_path"] = targets[0]["dbus_path"]
if scenario.get("event_only_target0"):
    targets[1]["target_event_us"] = targets[0]["target_event_us"]
if scenario.get("reused_observation"):
    targets[1]["observation_id"] = targets[0]["observation_id"]

facts = {
    "schema": "iprunner-controller-production-routing-facts/v3",
    "production_assignment": {
        "path": "direct-busctl" if scenario.get("direct_assignment_dbus", 0) else "controller-box-overlay",
        "normal_event_dispatch": not bool(scenario.get("missing_production_dispatch", 0)),
        "save_persisted": not bool(scenario.get("missing_production_save", 0)),
        "direct_assignment_dbus": bool(scenario.get("direct_assignment_dbus", 0)),
    },
    "home": {
        "isolated": True,
        "virtual_controllers": {"count": 4, "types": ["xb360"] * 4},
    },
    "binary": {
        "realpath_inside_prefix": bool(scenario.get("realpath_inside", 1)),
        "assets_inside_prefix": bool(scenario.get("assets_inside", 1)),
        "launch_cwd_isolated": bool(scenario.get("launch_cwd_isolated", 1)),
    },
    "topology": {
        "expected": 4,
        "observed": scenario.get("observed", 4),
        "target_paths": scenario.get("target_paths", 4),
        "kernel_nodes": scenario.get("kernel_nodes", 4),
        "cardinality": scenario.get("cardinality", "exact"),
        "identities_match": bool(scenario.get("identities_match", 1)),
        "identity_method": scenario.get("identity_method", "controlled-create-observe"),
        "one_to_one": not bool(scenario.get("ambiguous_node_pairing", 0)),
    },
    "physical": {
        "vidpid": scenario.get("physical_vidpid", "045e:028e"),
        "transport": "usb", "node": "/dev/input/event12", "source_path": source_path,
    },
    "targets": targets,
    "observer": {
        "event_stream": "event-stream", "mode": "read-only-evdev",
        "human_generated": not bool(scenario.get("direct_injection", 0)),
        "synthetic": bool(scenario.get("direct_injection", 0)),
        "direct_injection": bool(scenario.get("direct_injection", 0)),
        "physical_name": scenario.get("physical_name", "Xbox 360 Controller"),
        "target_name": scenario.get("target_name", "Xbox 360 Controller 0"),
    },
    "bus": {
        "system_socket": "/run/dbus/system_bus_socket",
        "socket_root_owned": bool(scenario.get("socket_root_owned", 1)),
        "owner_pid": scenario.get("owner_pid", 1234),
        "owner_exe": scenario.get("owner_exe", "/usr/bin/inputplumber"),
        "owner_exe_pinned": bool(scenario.get("owner_exe_pinned", 1)),
    },
    "artifacts": artifacts,
    "cleanup": {
        "termination": scenario.get("termination", "ok"),
        "target_cleanup": scenario.get("target_cleanup", "ok"),
        "targets_absent": bool(scenario.get("targets_absent", 1)),
        "kernel_nodes_absent": bool(scenario.get("kernel_nodes_absent", 1)),
        "deadline_ms": scenario.get("cleanup_deadline_ms", 15000),
    },
}
with open(os.path.join(base, "facts.json"), "w", encoding="utf-8") as f:
    json.dump(facts, f, indent=2)
    f.write("\n")
PY
}

# Fixture builders for each scenario. The default is the full pass.
scen_json() { local s=$1; shift; printf '%s' "$1" > "$tmp/$s.json"; echo "$tmp/$s.json"; }

four_events='physical0 event 1 304 1 ts 1500\ntarget0 event 1 304 1 ts 1600\nphysical1 event 1 304 1 ts 2500\ntarget1 event 1 304 1 ts 2600\nphysical2 event 1 304 1 ts 3500\ntarget2 event 1 304 1 ts 3600\nphysical3 event 1 304 1 ts 4500\ntarget3 event 1 304 1 ts 4600\n'
full_pass() { build_fixture "$1" "$(scen_json pass "{\"event_stream\": \"$four_events\"}")"; }
zero_targets() { build_fixture "$1" "$(scen_json zero "{\"observed\": 0, \"target_paths\": 0, \"kernel_nodes\": 0, \"event_stream\": \"\"}")"; }
partial_targets() { build_fixture "$1" "$(scen_json partial "{\"observed\": $2, \"target_paths\": $2, \"kernel_nodes\": $2, \"event_stream\": \"\"}")"; }
ambiguous() { build_fixture "$1" "$(scen_json ambiguous "{\"observed\": 4, \"target_paths\": 4, \"kernel_nodes\": 4, \"cardinality\": \"ambiguous\", \"event_stream\": \"\"}")"; }
wrong_cardinality() { build_fixture "$1" "$(scen_json wrong "{\"observed\": 4, \"target_paths\": 4, \"kernel_nodes\": 3, \"cardinality\": \"exact\", \"event_stream\": \"\"}")"; }
source_fallback() { build_fixture "$1" "$(scen_json source "{\"realpath_inside\": 0, \"assets_inside\": 1, \"event_stream\": \"\"}")"; }
direct_injection() { build_fixture "$1" "$(scen_json injection "{\"direct_injection\": 1, \"event_stream\": \"target event 1 304 1 ts 1600\\n\"}")"; }
stale_event() { build_fixture "$1" "$(scen_json stale "{\"event_stream\": \"physical0 event 1 304 1 ts 500\\ntarget0 event 1 304 1 ts 600\\n\"}")"; }
unrouted_event() { build_fixture "$1" "$(scen_json unrouted "{\"event_stream\": \"physical event 1 304 1 ts 1500\\n\"}")"; }
cleanup_failure() { build_fixture "$1" "$(scen_json cleanup "{\"target_cleanup\": \"fail\", \"event_stream\": \"physical event 1 304 1 ts 1500\\ntarget event 1 304 1 ts 1600\\n\"}")"; }
missing_artifact() { build_fixture "$1" "$(scen_json missart "{\"omit_artifact\": \"screenshot\", \"event_stream\": \"physical event 1 304 1 ts 1500\\ntarget event 1 304 1 ts 1600\\n\"}")"; }
corrupt_hash() { build_fixture "$1" "$(scen_json corrupt "{\"corrupt_hash\": \"log\", \"event_stream\": \"physical event 1 304 1 ts 1500\\ntarget event 1 304 1 ts 1600\\n\"}")"; }
# Physical event present but does NOT match the expected type/code/value, then
# a matching target event: correlation must fail.
mismatched_physical() { build_fixture "$1" "$(scen_json misphys "{\"event_stream\": \"physical0 event 1 999 1 ts 1500\\ntarget0 event 1 304 1 ts 1600\\n\"}")"; }
# Target event arrives BEFORE any fresh physical event.
target_before_physical() { build_fixture "$1" "$(scen_json tbph "{\"event_stream\": \"target0 event 1 304 1 ts 1500\\nphysical0 event 1 304 1 ts 1600\\n\"}")"; }
# Counted target identities do not match (wrong target name/type).
wrong_target_identity() { build_fixture "$1" "$(scen_json wrongid "{\"identities_match\": 0, \"event_stream\": \"physical event 1 304 1 ts 1500\\ntarget event 1 304 1 ts 1600\\n\"}")"; }
# Targets were created but left over (never all stopped): cleanup must fail
# the probe even though the topology is also incomplete.
leftover_cleanup() { build_fixture "$1" "$(scen_json leftover "{\"target_cleanup\": \"ok\", \"targets_absent\": 0, \"kernel_nodes_absent\": 0, \"event_stream\": \"physical event 1 304 1 ts 1500\\ntarget event 1 304 1 ts 1600\\n\"}")"; }
# Fake/substituted InputPlumber bus owner.
fake_bus_owner() { build_fixture "$1" "$(scen_json fakebus "{\"owner_exe\": \"/usr/bin/evil\", \"owner_exe_pinned\": 0, \"event_stream\": \"physical event 1 304 1 ts 1500\\ntarget event 1 304 1 ts 1600\\n\"}")"; }
# Product launched from a source CWD (source fallback via relative lookup).
cwd_source_fallback() { build_fixture "$1" "$(scen_json cwdfb "{\"launch_cwd_isolated\": 0, \"event_stream\": \"physical event 1 304 1 ts 1500\\ntarget event 1 304 1 ts 1600\\n\"}")"; }
# Artifact path escapes the fixture dir via '..' traversal.
artifact_traversal() { build_fixture "$1" "$(scen_json trav "{\"artifact_traversal\": 1, \"event_stream\": \"physical event 1 304 1 ts 1500\\ntarget event 1 304 1 ts 1600\\n\"}")"; }
# Artifact path is a symlink escaping the fixture dir.
artifact_symlink() { build_fixture "$1" "$(scen_json symlink "{\"artifact_symlink\": 1, \"event_stream\": \"physical event 1 304 1 ts 1500\\ntarget event 1 304 1 ts 1600\\n\"}")"; }
# Boolean-typed topology count (bool is not an int).
bool_counts() { build_fixture "$1" "$(scen_json boolc "{\"observed\": true, \"target_paths\": true, \"kernel_nodes\": true, \"event_stream\": \"\"}")"; }
wrong_physical_vidpid() { build_fixture "$1" "$(scen_json badvid "{\"physical_vidpid\":\"1234:5678\",\"event_stream\":\"$four_events\"}")"; }
unrouted_slot() { build_fixture "$1" "$(scen_json unroutedslot "{\"target_mutation\":{\"slot\":2,\"values\":{\"assignment_verified\":false}},\"event_stream\":\"$four_events\"}")"; }
duplicate_mapping() { build_fixture "$1" "$(scen_json dupmap "{\"duplicate_mapping\":true,\"event_stream\":\"$four_events\"}")"; }
reused_observation() { build_fixture "$1" "$(scen_json reused "{\"reused_observation\":true,\"event_stream\":\"$four_events\"}")"; }
event_only_target0() { build_fixture "$1" "$(scen_json only0 "{\"event_only_target0\":true,\"event_stream\":\"physical0 event 1 304 1 ts 1500\\ntarget0 event 1 304 1 ts 1600\\n\"}")"; }
direct_assignment_dbus() { build_fixture "$1" "$(scen_json directdbus "{\"direct_assignment_dbus\":true,\"event_stream\":\"$four_events\"}")"; }
ambiguous_node_pairing() { build_fixture "$1" "$(scen_json nodepair "{\"ambiguous_node_pairing\":true,\"identity_method\":\"name-sort\",\"event_stream\":\"$four_events\"}")"; }
cross_target_leakage() { build_fixture "$1" "$(scen_json leakage "{\"cross_target_leakage\":true,\"event_stream\":\"$four_events\"}")"; }
missing_production_dispatch() { build_fixture "$1" "$(scen_json nodispatch "{\"missing_production_dispatch\":true,\"event_stream\":\"$four_events\"}")"; }
missing_production_save() { build_fixture "$1" "$(scen_json nosave "{\"missing_production_save\":true,\"event_stream\":\"$four_events\"}")"; }


# ---------------------------------------------------------------------------
# Compile the observer once to prove the C helper builds cleanly.
# ---------------------------------------------------------------------------
OBSERVER="$tmp/routing_observer"
cc -O2 -o "$OBSERVER" "$OBSERVER_SOURCE"

# ---------------------------------------------------------------------------
# Full pass: 4/4 exact + fresh physical->target routed event + cleanup.
# ---------------------------------------------------------------------------
full_pass "$tmp/pass"
# Run with a caller-controlled artifact dir so the fixture retains cleanup.log
# and prints its hash for the signer (finding: signed cleanup.log artifact).
must_pass "full 4+fresh physical+routed event pass" \
    env CONTROLLER_PRODUCTION_ROUTING_ARTIFACTS="$tmp/artifacts" bash "$PROBE" --fixture "$tmp/pass"
grep -q 'production-routing-probe: PASS' "$tmp/last.out"
grep -q 'topology confirmed: 4 of 4' "$tmp/last.out"
grep -q 'all-four-targets-functionally-consumable verified' "$tmp/last.out"
for slot in 0 1 2 3; do grep -q "target-$slot independent-read-only-consumer correlated fresh-event" "$tmp/last.out"; done
grep -q 'cleanup: termination=ok target-cleanup=ok targets-absent=true kernel-nodes-absent=true' "$tmp/pass/cleanup.log"
grep -q 'cleanup-postcondition verified: dbus-targets-absent kernel-event-nodes-absent' "$tmp/last.out"
# cleanup.log must be retained as a signed/hash-printed artifact.
grep -q 'artifact hash' "$tmp/last.out"
grep -q "$tmp/artifacts/cleanup.log" "$tmp/last.out"
[[ -f "$tmp/artifacts/cleanup.log" ]] || { echo "test: cleanup.log not retained" >&2; exit 1; }
hashline=$(grep 'artifact hash' "$tmp/last.out" | grep 'cleanup.log')
printed_hash=$(printf '%s' "$hashline" | sed -n 's/.*artifact hash \([0-9a-f]\{64\}\).*/\1/p')
[[ -n "$printed_hash" ]] || { echo "test: no cleanup.log hash printed" >&2; exit 1; }
actual_hash=$(sha256sum "$tmp/artifacts/cleanup.log" | cut -d' ' -f1)
[[ "$actual_hash" == "$printed_hash" ]] || {
    echo "test: retained cleanup.log hash mismatch" >&2
    exit 1
}

# ---------------------------------------------------------------------------
# Exact 0/4: distinct immediate marker + BUG-0015 negative control.
# ---------------------------------------------------------------------------
zero_targets "$tmp/zero"
must_fail "exact 0 of 4 expected failure marker" bash "$PROBE" --fixture "$tmp/zero"
grep -q 'production-topology-incomplete: 0 of 4' "$tmp/last.out"
grep -q 'BUG-0015-negative-control' "$tmp/last.out"

# ---------------------------------------------------------------------------
# Partial 1-3 of 4: incomplete marker, no negative control, no false pass.
# ---------------------------------------------------------------------------
for n in 1 2 3; do
    partial_targets "$tmp/partial-$n" "$n"
    must_fail "partial $n of 4 topology incomplete" bash "$PROBE" --fixture "$tmp/partial-$n"
    grep -q "production-topology-incomplete: $n of 4" "$tmp/last.out"
    if grep -q 'BUG-0015-negative-control' "$tmp/last.out"; then
        echo "test: partial $n of 4 must not emit negative control" >&2
        exit 1
    fi
done

# ---------------------------------------------------------------------------
# Private bus: the probe refuses a system-bus-address override even in
# fixture mode, before any fact is read.
# ---------------------------------------------------------------------------
full_pass "$tmp/private-bus"
must_fail "private bus via DBUS_SYSTEM_BUS_ADDRESS override" \
    env DBUS_SYSTEM_BUS_ADDRESS=unix:path=/tmp/private bash "$PROBE" --fixture "$tmp/private-bus"
grep -q 'refusing a private bus' "$tmp/last.out"

# ---------------------------------------------------------------------------
# Direct InputEvent/synthetic injection: target event with no physical event.
# ---------------------------------------------------------------------------
direct_injection "$tmp/injection"
must_fail "direct InputEvent/synthetic injection rejected" bash "$PROBE" --fixture "$tmp/injection"
grep -q 'direct InputEvent/synthetic injection recorded' "$tmp/last.out"

# ---------------------------------------------------------------------------
# Stale event: physical event before the baseline window start.
# ---------------------------------------------------------------------------
stale_event "$tmp/stale"
must_fail "stale event (before baseline) rejected" bash "$PROBE" --fixture "$tmp/stale"
grep -q 'stale' "$tmp/last.out"

# Physical event present but never routed to a target node.
unrouted_event "$tmp/unrouted"
must_fail "physical event without routed target event" bash "$PROBE" --fixture "$tmp/unrouted"

# ---------------------------------------------------------------------------
# Wrong / ambiguous target cardinality.
# ---------------------------------------------------------------------------
ambiguous "$tmp/ambiguous"
must_fail "ambiguous target cardinality rejected" bash "$PROBE" --fixture "$tmp/ambiguous"
grep -q 'ambiguous target cardinality' "$tmp/last.out"

wrong_cardinality "$tmp/wrong"
must_fail "target/kernel cardinality mismatch rejected" bash "$PROBE" --fixture "$tmp/wrong"

# ---------------------------------------------------------------------------
# Source/build binary or asset fallback.
# ---------------------------------------------------------------------------
source_fallback "$tmp/source-fb"
must_fail "source/build or asset fallback rejected" bash "$PROBE" --fixture "$tmp/source-fb"
grep -q 'source/build or asset fallback detected' "$tmp/last.out"

# ---------------------------------------------------------------------------
# Missing screenshot artifact and mismatched log hash.
# ---------------------------------------------------------------------------
missing_artifact "$tmp/missing-art"
must_fail "missing screenshot artifact hash" bash "$PROBE" --fixture "$tmp/missing-art"

corrupt_hash "$tmp/corrupt"
must_fail "mismatched log artifact hash" bash "$PROBE" --fixture "$tmp/corrupt"

# ---------------------------------------------------------------------------
# Cleanup failure fails the probe even with a full pass otherwise.
# ---------------------------------------------------------------------------
cleanup_failure "$tmp/cleanup-fail"
must_fail "cleanup failure fails the probe" bash "$PROBE" --fixture "$tmp/cleanup-fail"
grep -q 'cleanup failure' "$tmp/last.out"
grep -q 'cleanup: termination=ok target-cleanup=fail' "$tmp/cleanup-fail/cleanup.log"

# ---------------------------------------------------------------------------
# Mismatched physical-event correlation: a physical event that does not match
# the expected type/code/value cannot anchor a routed target event.
# ---------------------------------------------------------------------------
mismatched_physical "$tmp/misphys"
must_fail "mismatched physical event correlation rejected" bash "$PROBE" --fixture "$tmp/misphys"
grep -q 'does not match expected type/code/value' "$tmp/last.out"

# ---------------------------------------------------------------------------
# Target-before-physical: a matching target event with no preceding fresh
# physical event is direct injection/synthetic routing.
# ---------------------------------------------------------------------------
target_before_physical "$tmp/tbph"
must_fail "target event before any physical event rejected" bash "$PROBE" --fixture "$tmp/tbph"
grep -q 'direct injection/synthetic routing' "$tmp/last.out"

# ---------------------------------------------------------------------------
# Wrong target name/type: counted target identities do not match.
# ---------------------------------------------------------------------------
wrong_target_identity "$tmp/wrongid"
must_fail "wrong target name/type identity rejected" bash "$PROBE" --fixture "$tmp/wrongid"
grep -q 'target/kernel identities do not match' "$tmp/last.out"

# ---------------------------------------------------------------------------
# Successful StopTargetDevice replies are not proof: leftover DBus objects
# and event nodes must fail after otherwise-complete routing evidence.
# ---------------------------------------------------------------------------
leftover_cleanup "$tmp/leftover"
must_fail "successful stops with leftover object/node fail the probe" bash "$PROBE" --fixture "$tmp/leftover"
grep -q 'cleanup failure' "$tmp/last.out"
grep -q 'target-cleanup=ok targets-absent=false kernel-nodes-absent=false' "$tmp/leftover/cleanup.log"
if grep -q 'cleanup-postcondition verified' "$tmp/last.out"; then
    echo "test: leftover cleanup emitted verified marker" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Fake/substituted InputPlumber bus owner: no routing evidence may be claimed.
# ---------------------------------------------------------------------------
fake_bus_owner "$tmp/fakebus"
must_fail "fake bus owner rejected" bash "$PROBE" --fixture "$tmp/fakebus"
grep -q 'must be pinned' "$tmp/last.out"

# ---------------------------------------------------------------------------
# CWD source fallback: product launched from a source working directory.
# ---------------------------------------------------------------------------
cwd_source_fallback "$tmp/cwdfb"
must_fail "CWD source fallback rejected" bash "$PROBE" --fixture "$tmp/cwdfb"
grep -q 'source CWD' "$tmp/last.out"

# ---------------------------------------------------------------------------
# Path traversal and symlink escapes must be rejected by the validator.
# ---------------------------------------------------------------------------
artifact_traversal "$tmp/trav"
must_fail "artifact '..' traversal rejected" bash "$PROBE" --fixture "$tmp/trav"
grep -q 'must not traverse' "$tmp/last.out"

artifact_symlink "$tmp/symlink"
must_fail "artifact symlink escape rejected" bash "$PROBE" --fixture "$tmp/symlink"
grep -q 'escapes the fixture directory' "$tmp/last.out"

# ---------------------------------------------------------------------------
# Boolean-typed counts (bool is not an int) must be rejected.
# ---------------------------------------------------------------------------
bool_counts "$tmp/boolc"
must_fail "bool-typed topology counts rejected" bash "$PROBE" --fixture "$tmp/boolc"
grep -q 'must be int' "$tmp/last.out"

# ---------------------------------------------------------------------------
# Per-target adversarial evidence: every slot needs a unique assignment,
# observation and fresh event from the exact physical source.
# ---------------------------------------------------------------------------
wrong_physical_vidpid "$tmp/badvid"
must_fail "wrong physical VID:PID rejected" bash "$PROBE" --fixture "$tmp/badvid"
unrouted_slot "$tmp/unrouted-slot"
must_fail "one unrouted target rejected" bash "$PROBE" --fixture "$tmp/unrouted-slot"
duplicate_mapping "$tmp/duplicate-map"
must_fail "ambiguous duplicate target mapping rejected" bash "$PROBE" --fixture "$tmp/duplicate-map"
reused_observation "$tmp/reused-observation"
must_fail "reused stale observation rejected" bash "$PROBE" --fixture "$tmp/reused-observation"
event_only_target0 "$tmp/target0-only"
must_fail "event only on target0 rejected" bash "$PROBE" --fixture "$tmp/target0-only"
direct_assignment_dbus "$tmp/direct-dbus"
must_fail "direct assignment DBus substitute rejected" bash "$PROBE" --fixture "$tmp/direct-dbus"
ambiguous_node_pairing "$tmp/ambiguous-node-pairing"
must_fail "index/name-sorted ambiguous nodes rejected" bash "$PROBE" --fixture "$tmp/ambiguous-node-pairing"
cross_target_leakage "$tmp/cross-target-leakage"
must_fail "cross-target event leakage rejected" bash "$PROBE" --fixture "$tmp/cross-target-leakage"
missing_production_dispatch "$tmp/missing-production-dispatch"
must_fail "missing production dispatch artifact rejected" bash "$PROBE" --fixture "$tmp/missing-production-dispatch"
missing_production_save "$tmp/missing-production-save"
must_fail "missing production save artifact rejected" bash "$PROBE" --fixture "$tmp/missing-production-save"

# ---------------------------------------------------------------------------
# Unknown argument and validator safety.
# ---------------------------------------------------------------------------
must_fail "probe refuses unknown arguments" bash "$PROBE" --bogus
must_fail "probe refuses a symlinked fixture dir" bash "$PROBE" --fixture /etc/passwd

# A malformed bundle (wrong schema) must be rejected by the validator.
full_pass "$tmp/vbad"
python3 - "$tmp/vbad" <<'PY'
import json, sys, os
p = os.path.join(sys.argv[1], "facts.json")
d = json.load(open(p, encoding="utf-8"))
d["schema"] = "wrong"
json.dump(d, open(p, "w", encoding="utf-8"))
PY
must_fail "validator rejects wrong schema" \
    python3 "$VALIDATOR" --facts "$tmp/vbad/facts.json" --fixture-dir "$tmp/vbad"

# ---------------------------------------------------------------------------
# unwrap_variant.py: busctl `data` and `body` shapes, dict/list/scalar forms.
# ---------------------------------------------------------------------------
UW="$PROJECT_ROOT/scripts/iprunner-probes/unwrap_variant.py"
unwrap_eq() {
    local input=$1 expected=$2
    local got
    got=$(printf '%s' "$input" | python3 "$UW" --check)
    [[ "$got" == "$expected" ]] || {
        echo "test: unwrap_variant: got '$got' expected '$expected'" >&2
        exit 1
    }
}
# get-property `data` shape.
unwrap_eq '{"type":"s","data":"xb360"}' '"xb360"'
# GetManagedObjects `body` variant shape.
unwrap_eq '{"type":"v","body":{"type":"s","data":"Xbox 360 Controller"}}' '"Xbox 360 Controller"'
# nested variant with `data`.
unwrap_eq '{"type":"v","data":{"type":"s","data":"/org/x"}}' '"/org/x"'
# scalar passthrough.
unwrap_eq '"plain"' '"plain"'
unwrap_eq '7' '7'
# list passthrough.
unwrap_eq '["a","b"]' '["a", "b"]'
# list-valued `body`.
unwrap_eq '{"type":"v","body":["a","b"]}' '["a", "b"]'
# plain dict (struct) with neither data nor body is preserved as-is.
unwrap_eq '{"a":1}' '{"a": 1}'

# ---------------------------------------------------------------------------
# extract_om_targets.py: new xb360 targets parsed from both data/body shapes.
# ---------------------------------------------------------------------------
EXTRACT_OM="$PROJECT_ROOT/scripts/iprunner-probes/extract_om_targets.py"
: > "$tmp/om-baseline-empty"
cat > "$tmp/om-data-body.json" <<'EOF'
{"type":"a{oa{sa{sv}}}","data":[{
  "/org/shadowblip/InputPlumber/Target/1": {
    "org.shadowblip.Input.Target": {
      "DeviceType": {"type":"v","body":{"type":"s","data":"xb360"}},
      "Name": {"type":"v","body":{"type":"s","data":"Xbox 360 Controller"}}
    }
  },
  "/org/shadowblip/InputPlumber/Target/2": {
    "org.shadowblip.Input.Target": {
      "DeviceType": {"type":"s","data":"xb360"},
      "Name": {"type":"s","data":"Xbox 360 Controller"}
    }
  }
}]}
EOF
python3 "$EXTRACT_OM" "$tmp/om-data-body.json" "$tmp/om-baseline-empty" "org.shadowblip.Input.Target" > "$tmp/om-out"
grep -q '^/org/shadowblip/InputPlumber/Target/1	Xbox 360 Controller$' "$tmp/om-out"
grep -q '^/org/shadowblip/InputPlumber/Target/2	Xbox 360 Controller$' "$tmp/om-out"

# A target whose DeviceType is not xb360 must fail (both shapes).
cat > "$tmp/om-wrongtype.json" <<'EOF'
{"type":"a{oa{sa{sv}}}","data":[{
  "/org/shadowblip/InputPlumber/Target/3": {
    "org.shadowblip.Input.Target": {
      "DeviceType": {"type":"v","body":{"type":"s","data":"keyboard"}},
      "Name": {"type":"s","data":"Keyboard"}
    }
  }
}]}
EOF
must_fail "extractor rejects non-xb360 DeviceType" \
    python3 "$EXTRACT_OM" "$tmp/om-wrongtype.json" "$tmp/om-baseline-empty" "org.shadowblip.Input.Target"

# A target whose Name is a non-string variant (list body) must fail.
cat > "$tmp/om-noname.json" <<'EOF'
{"type":"a{oa{sa{sv}}}","data":[{
  "/org/shadowblip/InputPlumber/Target/4": {
    "org.shadowblip.Input.Target": {
      "DeviceType": {"type":"s","data":"xb360"},
      "Name": {"type":"v","body":["Xbox 360 Controller"]}
    }
  }
}]}
EOF
must_fail "extractor rejects non-string Name" \
    python3 "$EXTRACT_OM" "$tmp/om-noname.json" "$tmp/om-baseline-empty" "org.shadowblip.Input.Target"

# Top-level busctl envelopes fail closed on naked, ambiguous, extra-field,
# wrong-signature, and empty ObjectManager results.
printf '%s\n' '{"/org/x":{"org.shadowblip.Input.Target":{}}}' > "$tmp/om-naked.json"
must_fail "naked ObjectManager map rejected" python3 "$EXTRACT_OM" "$tmp/om-naked.json" "$tmp/om-baseline-empty" "org.shadowblip.Input.Target"
for bad in \
  '{"type":"a{oa{sa{sv}}}","data":[{},{}]}' \
  '{"type":"a{oa{sa{sv}}}","data":[{}],"extra":1}' \
  '{"type":"s","data":[{}]}' \
  '{"type":"a{oa{sa{sv}}}","data":[{}]}'; do
    must_fail "malformed ObjectManager envelope rejected" bash -c \
        "printf '%s' '$bad' | python3 '$UW' --object-manager"
done

# CreateTargetDevice accepts exactly the real object-path envelope.
[[ "$(printf '%s' '{"type":"o","data":["/org/shadowblip/InputPlumber/Target/9"]}' | python3 "$UW" --object-path)" == '"/org/shadowblip/InputPlumber/Target/9"' ]]
for bad in \
  '{"type":"o","data":["/org/a","/org/b"]}' \
  '{"type":"o","data":"/org/a"}' \
  '{"type":"s","data":["/org/a"]}' \
  '{"type":"o","data":["created /org/a"]}'; do
    must_fail "malformed CreateTargetDevice envelope rejected" bash -c \
        "printf '%s' '$bad' | python3 '$UW' --object-path"
done

# ---------------------------------------------------------------------------
# create-xb360-targets.sh: deterministic fixture validation of the
# DBus Target.Name <-> EVIOCGNAME set/cardinality assumption.
# ---------------------------------------------------------------------------
CREATE="$PROJECT_ROOT/scripts/create-xb360-targets.sh"
mkdir -p "$tmp/cb-pass"
printf 'Xbox 360 Controller\nXbox 360 Controller\nXbox 360 Controller\nXbox 360 Controller\n' > "$tmp/cb-pass/target-names"
cp "$tmp/cb-pass/target-names" "$tmp/cb-pass/node-names"
must_pass "create-xb360-targets collective name/cardinality pass" bash "$CREATE" --fixture "$tmp/cb-pass"
grep -q 'PASS (collective name/cardinality validated)' "$tmp/last.out"

mkdir -p "$tmp/cb-mismatch"
printf 'Xbox 360 Controller\nXbox 360 Controller\nXbox 360 Controller\nXbox 360 Controller\n' > "$tmp/cb-mismatch/target-names"
printf 'Xbox 360 Controller\nXbox 360 Controller\nXbox 360 Controller\nOther Pad\n' > "$tmp/cb-mismatch/node-names"
must_fail "create-xb360-targets name mismatch" bash "$CREATE" --fixture "$tmp/cb-mismatch"

mkdir -p "$tmp/cb-extra"
cp "$tmp/cb-pass/target-names" "$tmp/cb-extra/target-names"
printf 'Xbox 360 Controller\nXbox 360 Controller\nXbox 360 Controller\nXbox 360 Controller\nXbox 360 Controller\n' > "$tmp/cb-extra/node-names"
must_fail "create-xb360-targets extra node rejected" bash "$CREATE" --fixture "$tmp/cb-extra"

mkdir -p "$tmp/cb-few"
printf 'Xbox 360 Controller\nXbox 360 Controller\nXbox 360 Controller\n' > "$tmp/cb-few/target-names"
cp "$tmp/cb-few/target-names" "$tmp/cb-few/node-names"
must_fail "create-xb360-targets wrong count rejected" bash "$CREATE" --fixture "$tmp/cb-few"

must_fail "create-xb360-targets private bus refused" \
    env DBUS_SYSTEM_BUS_ADDRESS=unix:path=/tmp/private bash "$CREATE" --fixture "$tmp/cb-pass"
grep -q 'private bus' "$tmp/last.out"

must_fail "create-xb360-targets unknown arguments" bash "$CREATE" --bogus
must_fail "create-xb360-targets symlink fixture dir" bash "$CREATE" --fixture /etc/passwd

echo "test: controller-production-routing probe adversarial checks passed"
