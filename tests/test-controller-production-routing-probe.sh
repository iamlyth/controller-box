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
# corrupt the log hash to exercise the missing/mismatched-hash scenarios.
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

facts = {
    "schema": "iprunner-controller-production-routing-facts/v1",
    "home": {
        "isolated": True,
        "virtual_controllers": {"count": 4, "types": ["xb360"] * 4},
    },
    "binary": {
        "realpath_inside_prefix": bool(scenario.get("realpath_inside", 1)),
        "assets_inside_prefix": bool(scenario.get("assets_inside", 1)),
    },
    "topology": {
        "expected": 4,
        "observed": scenario.get("observed", 4),
        "target_paths": scenario.get("target_paths", 4),
        "kernel_nodes": scenario.get("kernel_nodes", 4),
        "cardinality": scenario.get("cardinality", "exact"),
    },
    "observer": {
        "event_stream": "event-stream",
        "direct_injection": bool(scenario.get("direct_injection", 0)),
        "physical_name": scenario.get("physical_name", ""),
        "target_name": scenario.get("target_name", ""),
    },
    "artifacts": artifacts,
    "cleanup": {
        "termination": scenario.get("termination", "ok"),
        "target_cleanup": scenario.get("target_cleanup", "ok"),
    },
}
with open(os.path.join(base, "facts.json"), "w", encoding="utf-8") as f:
    json.dump(facts, f, indent=2)
    f.write("\n")
PY
}

# Fixture builders for each scenario. The default is the full pass.
scen_json() { local s=$1; shift; printf '%s' "$1" > "$tmp/$s.json"; echo "$tmp/$s.json"; }

full_pass() { build_fixture "$1" "$(scen_json pass "{\"event_stream\": \"physical event 1 304 1 ts 1500\\ntarget event 1 304 1 ts 1600\\n\"}")"; }
zero_targets() { build_fixture "$1" "$(scen_json zero "{\"observed\": 0, \"target_paths\": 0, \"kernel_nodes\": 0, \"event_stream\": \"\"}")"; }
partial_targets() { build_fixture "$1" "$(scen_json partial "{\"observed\": $2, \"target_paths\": $2, \"kernel_nodes\": $2, \"event_stream\": \"\"}")"; }
ambiguous() { build_fixture "$1" "$(scen_json ambiguous "{\"observed\": 4, \"target_paths\": 4, \"kernel_nodes\": 4, \"cardinality\": \"ambiguous\", \"event_stream\": \"\"}")"; }
wrong_cardinality() { build_fixture "$1" "$(scen_json wrong "{\"observed\": 4, \"target_paths\": 4, \"kernel_nodes\": 3, \"cardinality\": \"exact\", \"event_stream\": \"\"}")"; }
source_fallback() { build_fixture "$1" "$(scen_json source "{\"realpath_inside\": 0, \"assets_inside\": 1, \"event_stream\": \"\"}")"; }
direct_injection() { build_fixture "$1" "$(scen_json injection "{\"direct_injection\": 1, \"event_stream\": \"target event 1 304 1 ts 1600\\n\"}")"; }
stale_event() { build_fixture "$1" "$(scen_json stale "{\"event_stream\": \"physical event 1 304 1 ts 500\\ntarget event 1 304 1 ts 600\\n\"}")"; }
unrouted_event() { build_fixture "$1" "$(scen_json unrouted "{\"event_stream\": \"physical event 1 304 1 ts 1500\\n\"}")"; }
cleanup_failure() { build_fixture "$1" "$(scen_json cleanup "{\"target_cleanup\": \"fail\", \"event_stream\": \"physical event 1 304 1 ts 1500\\ntarget event 1 304 1 ts 1600\\n\"}")"; }
missing_artifact() { build_fixture "$1" "$(scen_json missart "{\"omit_artifact\": \"screenshot\", \"event_stream\": \"physical event 1 304 1 ts 1500\\ntarget event 1 304 1 ts 1600\\n\"}")"; }
corrupt_hash() { build_fixture "$1" "$(scen_json corrupt "{\"corrupt_hash\": \"log\", \"event_stream\": \"physical event 1 304 1 ts 1500\\ntarget event 1 304 1 ts 1600\\n\"}")"; }


# ---------------------------------------------------------------------------
# Compile the observer once to prove the C helper builds cleanly.
# ---------------------------------------------------------------------------
OBSERVER="$tmp/routing_observer"
cc -O2 -o "$OBSERVER" "$OBSERVER_SOURCE"

# ---------------------------------------------------------------------------
# Full pass: 4/4 exact + fresh physical->target routed event + cleanup.
# ---------------------------------------------------------------------------
full_pass "$tmp/pass"
must_pass "full 4+fresh physical+routed event pass" bash "$PROBE" --fixture "$tmp/pass"
grep -q 'production-routing-probe: PASS' "$tmp/last.out"
grep -q 'topology confirmed: 4 of 4' "$tmp/last.out"
grep -q 'cleanup: termination=ok target-cleanup=ok' "$tmp/pass/cleanup.log"

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

echo "test: controller-production-routing probe adversarial checks passed"
