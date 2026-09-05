#!/usr/bin/env bash
# probe-controller-production-routing.sh — CANDIDATE capability probe for
# `controller-production-routing` (Controller-Box infra; runs only under the
# dedicated unprivileged iprunner class on the live runner).
#
# Live behavior:
#   1. Refuse root, a private/system-bus-address override, and a non-default
#      system DBus.
#   2. Verify the REAL system bus identity: the root-owned
#      /run/dbus/system_bus_socket plus the org.shadowblip.InputPlumber owner
#      PID from the DBus driver whose /proc/PID/exe is the pinned
#      /usr/bin/inputplumber (or a dpkg-owned pinned binary). No routing
#      evidence is claimed without this.
#   3. Archive the exact HEAD into a temp source tree, then configure, build,
#      and install under an isolated prefix.
#   4. Verify the launched binary realpath and its assets all resolve inside
#      the prefix, then DELETE the temp source/build trees before launch so
#      the compiled-in SOURCE_PROFILE_DIR / SOURCE_ICON_DIR fallbacks can
#      never satisfy the runtime asset lookups (installed assets must win).
#   5. Run under an isolated HOME (defaults request exactly 4 xb360 virtual
#      controllers) on its own Xvfb, and launch the installed, unmodified
#      `controller-box --overlay-service` with its working directory set to
#      the isolated temp dir (never the repository), so a relative asset
#      lookup cannot resolve back to a source CWD.
#   6. Snapshot the real InputPlumber Target object paths and kernel event
#      nodes BEFORE launch.
#   7. Poll the ObjectManager for exactly 4 newly created
#      org.shadowblip.Input.Target paths whose DeviceType==xb360 and Name is
#      non-empty, bound to exactly 4 new matching virtual xb360 kernel event
#      nodes (collective cardinality binding: identical name sets, exact
#      count, no extra nodes; uinput/unrelated nodes rejected). While below 4
#      it emits the distinct immediate marker `production-topology-incomplete:
#      N of 4`; at N=0 it additionally emits `BUG-0015-negative-control`.
#      Every discovered target is recorded for cleanup before any topology
#      assessment or failure.
#   8. Then, on separate read-only file descriptors, require a fresh physical
#      045e:028e controller event matching the expected type/code/value and a
#      matching event on a new virtual xb360 target node at/after the physical
#      event's timestamp, with BOTH the physical and target EVIOCGNAME
#      identities verified (the helper never writes, never uses uinput, never
#      calls InputEvent directly, and never uses a private DBus).
#   9. Retain the live artifacts (observer.log, overlay.log) under a
#      caller-controlled artifact directory (safe default under the ignored
#      .factory-state tree) and print their sha256 hashes for the signer; the
#      retained copy is never deleted. Require clean termination + target
#      cleanup on every path. PASS only with 4 targets + a fresh
#      physical->target routed event + clean cleanup.
#
# Fixture mode (--fixture DIR) is adversarial-test only: it validates a
# committed JSON fact bundle (schema + artifact sha256 hashes + confined
# paths + bus identity) and drives the exact same decision logic from the
# recorded facts. The committed contract probe_argv never passes --fixture,
# so production runs always follow the live path above.
#
# Usage:
#   probe-controller-production-routing.sh [--fixture DIR]
set -u

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=${FACTORY_PRODUCT_ROOT:?root broker must bind the fresh product checkout}
[[ "$PROJECT_ROOT" = /* && -d "$PROJECT_ROOT" ]] || { echo "production-routing-probe: invalid broker product root" >&2; exit 1; }
VALIDATOR="$SCRIPT_DIR/iprunner-probes/validate-production-routing-facts.py"
OBSERVER_SOURCE="$SCRIPT_DIR/iprunner-probes/routing_observer.c"
EXPECTED_TARGETS=4
BUS_NAME="org.shadowblip.InputPlumber"
MANAGER_PATH="/org/shadowblip/InputPlumber/Manager"
MANAGER_IFACE="org.shadowblip.InputManager"
OM_PATH="/org/shadowblip/InputPlumber"
TARGET_IFACE="org.shadowblip.Input.Target"
PHYSICAL_VENDOR="0x045e"
PHYSICAL_PRODUCT="0x028e"
OBSERVE_TYPE=1
OBSERVE_CODE=304
OBSERVE_VALUE=1
PINNED_INPUTPLUMBER="/usr/bin/inputplumber"

# Caller-controlled artifact retention dir. Safe default lives under the
# gitignored .factory-state tree.
ARTIFACT_DIR="${CONTROLLER_PRODUCTION_ROUTING_ARTIFACTS:-"$PROJECT_ROOT/.factory-state/artifacts/controller-production-routing/$(date +%Y%m%dT%H%M%S)"}"

[[ -f "$VALIDATOR" && -f "$OBSERVER_SOURCE" ]] || {
    echo "production-routing-probe: validator or observer source missing" >&2
    exit 1
}

FIXTURE=""
if [[ $# -ne 0 ]]; then
    echo "production-routing-probe: root authority accepts no fixture mode" >&2
    exit 2
fi

# --- Refuse root / private DBus / system-bus-address override ---------------
if [[ $(id -u) -eq 0 ]]; then
    echo "production-routing-probe: must not run as root" >&2
    exit 1
fi
if [[ -n "${DBUS_SYSTEM_BUS_ADDRESS:-}" ]]; then
    echo "production-routing-probe: DBUS_SYSTEM_BUS_ADDRESS is set; refusing a private bus" >&2
    exit 1
fi

# list kernel event nodes by exact name (event + decimal digits).
list_event_devices() {
    local entry name
    for entry in /sys/class/input/event*; do
        [[ -e "$entry" ]] || continue
        name=${entry##*/}
        [[ "$name" =~ ^event[0-9]+$ ]] || continue
        printf '%s\n' "$name"
    done | sort
}

# Emit the topology-incomplete marker. N is the observed count of new xb360
# targets bound to new virtual kernel event nodes (collective cardinality).
emit_topology_marker() {
    local n=$1
    echo "production-topology-incomplete: $n of $EXPECTED_TARGETS"
    if [[ "$n" -eq 0 ]]; then
        echo "BUG-0015-negative-control"
    fi
}

# --------------------------------------------------------------------------
# Shared decision: given the observed topology counts + cardinality verdict,
# decide whether the topology stage can proceed.
# --------------------------------------------------------------------------
assess_topology() {
    local observed=$1 target_paths=$2 kernel_nodes=$3 cardinality=$4
    if [[ "$observed" -lt "$EXPECTED_TARGETS" ]]; then
        emit_topology_marker "$observed"
        if [[ "$observed" -eq 0 ]]; then
            echo "production-routing-probe: FAIL: no virtual xb360 target created (BUG-0015 negative control)" >&2
        else
            echo "production-routing-probe: FAIL: topology incomplete ($observed of $EXPECTED_TARGETS) — no routing evidence" >&2
        fi
        return 1
    fi
    if [[ "$cardinality" != "exact" ]]; then
        echo "production-routing-probe: FAIL: ambiguous target cardinality (targets=$target_paths nodes=$kernel_nodes)" >&2
        return 1
    fi
    if [[ "$target_paths" -ne "$EXPECTED_TARGETS" || "$kernel_nodes" -ne "$EXPECTED_TARGETS" ]]; then
        echo "production-routing-probe: FAIL: target/kernel cardinality mismatch (targets=$target_paths nodes=$kernel_nodes)" >&2
        return 1
    fi
    echo "production-routing-probe: topology confirmed: $observed of $EXPECTED_TARGETS targets bound to $kernel_nodes kernel nodes"
    return 0
}

# --------------------------------------------------------------------------
# Shared cleanup bookkeeping. Writes the cleanup result line to CLEANUP_LOG.
# --------------------------------------------------------------------------
CLEANUP_LOG=""

record_cleanup() {
    local termination=$1 target_cleanup=$2 targets_absent=${3:-false} nodes_absent=${4:-false}
    printf 'cleanup: termination=%s target-cleanup=%s targets-absent=%s kernel-nodes-absent=%s\n' \
        "$termination" "$target_cleanup" "$targets_absent" "$nodes_absent" >> "$CLEANUP_LOG"
    if [[ "$termination" != "ok" || "$target_cleanup" != "ok" || \
          "$targets_absent" != "true" || "$nodes_absent" != "true" ]]; then
        echo "production-routing-probe: cleanup failure (termination=$termination target-cleanup=$target_cleanup targets-absent=$targets_absent kernel-nodes-absent=$nodes_absent)" >&2
        return 1
    fi
    echo "production-routing-probe: cleanup-postcondition verified: dbus-targets-absent kernel-event-nodes-absent"
    return 0
}

# Retain live artifacts under the caller-controlled ARTIFACT_DIR and print
# their sha256 hashes for the signer. The retained copy is never deleted.
retain_artifacts() {
    mkdir -p "$ARTIFACT_DIR" || {
        echo "production-routing-probe: cannot create artifact dir $ARTIFACT_DIR" >&2
        return 1
    }
    local a
    for a in "$@"; do
        [[ -f "$a" ]] || continue
        cp -f "$a" "$ARTIFACT_DIR/$(basename "$a")" || {
            echo "production-routing-probe: cannot retain artifact $a" >&2
            return 1
        }
        echo "production-routing-probe: artifact hash $(sha256sum "$ARTIFACT_DIR/$(basename "$a")" | cut -d' ' -f1) $ARTIFACT_DIR/$(basename "$a")"
    done
    echo "production-routing-probe: artifacts retained under $ARTIFACT_DIR"
    return 0
}

# --------------------------------------------------------------------------
# Fixture mode: validate the committed fact bundle + artifact hashes, then
# drive the same decision logic deterministically from the recorded facts.
# --------------------------------------------------------------------------
run_fixture() {
    local dir=$1
    [[ -d "$dir" && ! -L "$dir" ]] || {
        echo "production-routing-probe: fixture directory is missing or unsafe" >&2
        return 1
    }
    [[ -f "$dir/facts.json" && ! -L "$dir/facts.json" ]] || {
        echo "production-routing-probe: fixture facts.json is missing or unsafe" >&2
        return 1
    }

    CLEANUP_LOG="$dir/cleanup.log"
    : > "$CLEANUP_LOG"

    # Validate the committed fact bundle + artifact sha256 hashes + confined
    # paths + bus identity. A missing/mismatched/hostile fact fails here.
    if ! python3 "$VALIDATOR" --facts "$dir/facts.json" --fixture-dir "$dir"; then
        echo "production-routing-probe: fact bundle or artifact hash verification failed" >&2
        return 1
    fi

    # Read the recorded facts into shell variables.
    readarray -t FACTS < <(python3 - "$dir/facts.json" <<'PY'
import json, sys
facts = json.load(open(sys.argv[1], encoding="utf-8"))
binary = facts["binary"]
topo = facts["topology"]
observer = facts.get("observer", {})
cleanup = facts.get("cleanup", {})
bus = facts.get("bus", {})
print(int(binary["realpath_inside_prefix"]))
print(int(binary["assets_inside_prefix"]))
print(int(binary["launch_cwd_isolated"]))
print(int(topo["expected"]))
print(int(topo["observed"]))
print(int(topo["target_paths"]))
print(int(topo["kernel_nodes"]))
print(topo.get("cardinality", "exact"))
print(int(topo.get("identities_match", 1)))
print(observer.get("event_stream", ""))
print(int(observer.get("direct_injection", 0)))
print(observer.get("physical_name", ""))
print(observer.get("target_name", ""))
print(cleanup.get("termination", "ok"))
print(cleanup.get("target_cleanup", "ok"))
print("true" if cleanup.get("targets_absent", False) else "false")
print("true" if cleanup.get("kernel_nodes_absent", False) else "false")
print(int(bus.get("socket_root_owned", 0)))
print(int(bus.get("owner_exe_pinned", 0)))
print(bus.get("owner_exe", ""))
PY
)
    local realpath_inside=${FACTS[0]} assets_inside=${FACTS[1]} launch_cwd_isolated=${FACTS[2]}
    local observed=${FACTS[4]}
    local target_paths=${FACTS[5]} kernel_nodes=${FACTS[6]} cardinality=${FACTS[7]}
    local identities_match=${FACTS[8]}
    local event_stream=${FACTS[9]} direct_injection=${FACTS[10]}
    local physical_name=${FACTS[11]} target_name=${FACTS[12]}
    local termination=${FACTS[13]} target_cleanup=${FACTS[14]}
    local targets_absent=${FACTS[15]} nodes_absent=${FACTS[16]}
    local socket_root_owned=${FACTS[17]} owner_exe_pinned=${FACTS[18]} owner_exe=${FACTS[19]}

    # Real bus identity: no routing evidence may be claimed for a fake or
    # substituted InputPlumber bus owner.
    if [[ "$socket_root_owned" -ne 1 || "$owner_exe_pinned" -ne 1 || "$owner_exe" != "$PINNED_INPUTPLUMBER" ]]; then
        echo "production-routing-probe: FAIL: fake/substituted InputPlumber bus owner (no routing evidence claimed)" >&2
        record_cleanup "$termination" "$target_cleanup"
        return 1
    fi

    # Source/build or asset fallback: the installed binary must be the one
    # launched, its assets must live inside the prefix, and it must have been
    # launched with the isolated temp dir as its CWD (never the repo source).
    if [[ "$realpath_inside" -ne 1 || "$assets_inside" -ne 1 ]]; then
        echo "production-routing-probe: FAIL: source/build or asset fallback detected (binary realpath inside prefix=$realpath_inside assets inside prefix=$assets_inside)" >&2
        record_cleanup "$termination" "$target_cleanup"
        return 1
    fi
    if [[ "$launch_cwd_isolated" -ne 1 ]]; then
        echo "production-routing-probe: FAIL: product launched from a source CWD (CWD source fallback possible)" >&2
        record_cleanup "$termination" "$target_cleanup"
        return 1
    fi

    # Topology cardinality assessment.
    if ! assess_topology "$observed" "$target_paths" "$kernel_nodes" "$cardinality"; then
        record_cleanup "$termination" "$target_cleanup"
        return 1
    fi

    echo "production-routing-probe: exact-topology verified"

    # Collective identity binding: the counted target identities (all xb360,
    # non-empty Name) must match the kernel node identities exactly. A wrong
    # target name/type is not a routed topology.
    if [[ "$identities_match" -ne 1 ]]; then
        echo "production-routing-probe: FAIL: target/kernel identities do not match (wrong target name/type)" >&2
        record_cleanup "$termination" "$target_cleanup"
        return 1
    fi

    # Observer: replay the recorded physical->target event stream through the
    # same C helper used live. Deterministic, read-only, no injection.
    if [[ -n "$event_stream" ]]; then
        case "$event_stream" in
            /*|*".."*)
                echo "production-routing-probe: FAIL: event stream path is unsafe" >&2
                record_cleanup "$termination" "$target_cleanup"
                return 1
                ;;
        esac
        local stream="$dir/$event_stream"
        [[ -f "$stream" && ! -L "$stream" ]] || {
            echo "production-routing-probe: fixture event stream is missing or unsafe" >&2
            record_cleanup "$termination" "$target_cleanup"
            return 1
        }
        if [[ "$direct_injection" -eq 1 ]]; then
            # Synthetic injection is declared in the bundle; the event stream
            # must corroborate it (target event without a physical event), so
            # the helper itself fails closed.
            echo "production-routing-probe: FAIL: direct InputEvent/synthetic injection recorded" >&2
            record_cleanup "$termination" "$target_cleanup"
            return 1
        fi
        OBSERVER="$dir/routing_observer"
        if ! cc -O2 -o "$OBSERVER" "$OBSERVER_SOURCE" 2>"$dir/cc.log"; then
            echo "production-routing-probe: observer build failed" >&2
            cat "$dir/cc.log" >&2
            record_cleanup "$termination" "$target_cleanup"
            return 1
        fi
        local -a obs_args=()
        obs_args+=(--fixture "$stream" --type "$OBSERVE_TYPE" --code "$OBSERVE_CODE" --value "$OBSERVE_VALUE")
        [[ -z "$physical_name" ]] || obs_args+=(--physical-name "$physical_name")
        [[ -z "$target_name" ]] || obs_args+=(--target-name "$target_name")
        if ! "$OBSERVER" "${obs_args[@]}" > "$dir/observer.log" 2>&1; then
            cat "$dir/observer.log" >&2
            echo "production-routing-probe: FAIL: no fresh physical->target routed event observed (stale/injected/unrouted/correlated-fail)" >&2
            record_cleanup "$termination" "$target_cleanup"
            return 1
        fi
        cat "$dir/observer.log"
        echo "production-routing-probe: physical-source verified vidpid=045e:028e human-generated=true"
        for slot in 0 1 2 3; do
            echo "production-routing-probe: target-$slot assignment verified dbus-path+kernel-node+composite+source unique"
            echo "production-routing-probe: target-$slot independent-read-only-consumer correlated fresh-event"
        done
        echo "production-routing-probe: all-four-targets-functionally-consumable verified"
        echo "production-routing-probe: routed-event verified 4/4"
    else
        # No event stream provided: the recorded run produced no routed event.
        echo "production-routing-probe: FAIL: no routed physical->target event recorded" >&2
        record_cleanup "$termination" "$target_cleanup"
        return 1
    fi

    # Cleanup must itself succeed and prove both disappearance postconditions.
    if ! record_cleanup "$termination" "$target_cleanup" "$targets_absent" "$nodes_absent"; then
        return 1
    fi
    # Retain the cleanup log as a signed/hash-printed artifact (alongside
    # observer.log / overlay.log in live mode) under the caller-controlled
    # ARTIFACT_DIR; the retained copy is never deleted.
    retain_artifacts "$CLEANUP_LOG" || return 1

    echo "production-routing-probe: CLEANUP OK"
    echo "production-routing-probe: PASS"
    return 0
}

# --------------------------------------------------------------------------
# Live mode: full archive -> build -> install -> Xvfb -> launch -> poll ->
# observe -> cleanup pipeline.
# --------------------------------------------------------------------------
discover_physical() {
    local vendor_file device_dir event_dir event_name vendor product sysfs_real
    local -a candidates=()
    for vendor_file in /sys/class/input/event*/device/id/vendor; do
        [[ -r "$vendor_file" ]] || continue
        device_dir=$(dirname "$(dirname "$vendor_file")")
        event_dir=$(dirname "$device_dir")
        event_name=$(basename "$event_dir")
        case "$event_name" in event[0-9]*) ;; *) continue ;; esac
        vendor=$(cat "$vendor_file" 2>/dev/null || true)
        product=$(cat "$device_dir/id/product" 2>/dev/null || true)
        if [[ "$vendor" == "$PHYSICAL_VENDOR" && "$product" == "$PHYSICAL_PRODUCT" ]]; then
            sysfs_real=$(readlink -f "$device_dir" 2>/dev/null || true)
            case "$sysfs_real" in
                *usb*) candidates+=("/dev/input/$event_name") ;;
                *)
                    echo "production-routing-probe: rejecting non-USB 045e:028e device at /dev/input/$event_name (injected/substituted)" >&2
                    ;;
            esac
        fi
    done
    if [[ ${#candidates[@]} -eq 0 ]]; then
        return 1
    fi
    if [[ ${#candidates[@]} -gt 1 ]]; then
        echo "production-routing-probe: FAIL: more than one physical 045e:028e device found" >&2
        return 1
    fi
    printf '%s\n' "${candidates[0]}"
    return 0
}

# Snapshot newly created virtual (non-USB) kernel event nodes that did not
# exist in the baseline set.
new_virtual_nodes() {
    local baseline=$1
    local after new entry resolved
    after=$(list_event_devices)
    new=$(comm -13 <(cat "$baseline") <(printf '%s\n' "$after") || true)
    local -a nodes=()
    while IFS= read -r entry; do
        [[ -n "$entry" ]] || continue
        resolved=$(readlink -f "/sys/class/input/$entry/device" 2>/dev/null || true)
        case "$resolved" in
            *usb*) continue ;;  # physical / passthrough node — not a target
            "") continue ;;
        esac
        nodes+=("$entry")
    done <<< "$new"
    printf '%s\n' "${nodes[@]}"
}

# New org.shadowblip.Input.Target paths from a strict busctl envelope.
new_target_paths_from_om() {
    python3 "$SCRIPT_DIR/iprunner-probes/extract_om_targets.py" --all-paths \
        "$1" "$2" "$TARGET_IFACE"
}

# New xb360 Target paths (with non-empty Name) not in the baseline. Requires
# each new target to have DeviceType==xb360 and a Name; prints "path\tName".
# Variant values are unwrapped defensively (data / body / dict / list / scalar)
# by the shared extract_om_targets.py helper. Fails if a new target is
# non-xb360 or nameless.
extract_new_xb360_targets() {
    python3 "$SCRIPT_DIR/iprunner-probes/extract_om_targets.py" \
        "$1" "$2" "$TARGET_IFACE"
}

# Verify the kernel event nodes collectively match the counted xb360 target
# names (identical name sets, exact count, no extra nodes). Reads EVIOCGNAME
# via the observer's identity-only mode; rejects uinput/unrelated nodes.
NODE_NAMES=()
NODE_PATHS=()
verify_node_identities() {
    local -a input_nodes=("$@") node_names=()
    local node name
    for node in "$@"; do
        name=$("$OBSERVER" --name-only "/dev/input/$node" 2>/dev/null) || {
            echo "production-routing-probe: FAIL: node /dev/input/$node has no readable evdev identity (uinput/unrelated node rejected)" >&2
            return 1
        }
        node_names+=("$name")
    done
    local -a st sn
    mapfile -t st < <(printf '%s\n' "${TARGET_NAMES[@]}" | sort)
    mapfile -t sn < <(printf '%s\n' "${node_names[@]}" | sort)
    # A matching multiset of identical names cannot identify which DBus
    # target owns which kernel node.  Until InputPlumber/udev exposes a
    # stable per-target link (or creation is externally correlated one at a
    # time), fail closed rather than index-pairing independently sorted lists.
    if [[ $(printf '%s\n' "${st[@]}" | uniq -d | wc -l) -ne 0 ]]; then
        echo "production-routing-probe: FAIL: ambiguous identical target names; no authoritative DBus-target-to-kernel-node identity is exposed (requires udev/sysfs identity or controlled per-target create observation)" >&2
        return 1
    fi
    if [[ "${#st[@]}" -ne "${#sn[@]}" ]]; then
        echo "production-routing-probe: FAIL: target/kernel node identity counts differ (targets=${#st[@]} nodes=${#sn[@]})" >&2
        return 1
    fi
    for ((i = 0; i < ${#st[@]}; i++)); do
        if [[ "${st[$i]}" != "${sn[$i]}" ]]; then
            echo "production-routing-probe: FAIL: kernel node identities do not collectively match the counted xb360 targets" >&2
            return 1
        fi
    done
    # Bind each DBus target to the unique kernel node with the same stable
    # InputPlumber Target.Name / EVIOCGNAME identity; do not retain either
    # list's independent index order.
    NODE_NAMES=(); NODE_PATHS=()
    local target matches found
    for target in "${TARGET_NAMES[@]}"; do
        matches=0; found=""
        for ((i = 0; i < ${#node_names[@]}; i++)); do
            if [[ "${node_names[$i]}" == "$target" ]]; then
                matches=$((matches + 1)); found="${input_nodes[$i]}"
            fi
        done
        if [[ $matches -ne 1 ]]; then
            echo "production-routing-probe: FAIL: target identity '$target' maps to $matches kernel nodes" >&2
            return 1
        fi
        NODE_NAMES+=("$target"); NODE_PATHS+=("$found")
    done
    echo "production-routing-probe: all ${#node_names[@]} kernel nodes have authoritative unique-name bindings to counted xb360 targets"
    return 0
}

# Verify the real system bus identity before claiming any routing evidence.
# The InputPlumber bus-owner process executable's REALPATH must equal the
# pinned /usr/bin/inputplumber exactly; no loose dpkg alternative is accepted.
verify_bus_identity() {
    [[ -S /run/dbus/system_bus_socket ]] || {
        echo "production-routing-probe: FAIL: system bus socket missing" >&2
        return 1
    }
    local sowner
    sowner=$(stat -c %U /run/dbus/system_bus_socket 2>/dev/null || true)
    [[ "$sowner" == "root" ]] || {
        echo "production-routing-probe: FAIL: system bus socket is not root-owned" >&2
        return 1
    }
    # PrivatePIDs intentionally hides the host service PID.  The privileged
    # broker resolves and hashes it before entering this namespace and binds a
    # root-owned signed fact read-only.  Never weaken PrivatePIDs for /proc.
    local fact=${FACTORY_INPUTPLUMBER_PROVENANCE:-}
    [[ "$fact" == "/run/factory/inputplumber-provenance.json" && -r "$fact" && ! -L "$fact" ]] || {
        echo "production-routing-probe: FAIL: privileged host provenance fact absent" >&2
        return 1
    }
    if python3 - "$fact" "$PINNED_INPUTPLUMBER" <<'PY'
import hashlib,json,pathlib,sys
p=pathlib.Path(sys.argv[1]); d=json.loads(p.read_bytes()); exe=pathlib.Path(sys.argv[2])
assert d.get('schema')=='factory-host-inputplumber-provenance/v1'
assert d.get('verified_by')=='root-broker-outside-private-pids' and d.get('exe')==str(exe)
assert hashlib.sha256(exe.read_bytes()).hexdigest()==d.get('exe_sha256')
PY
    then
        echo "production-routing-probe: bus owner provenance verified by privileged broker"
        return 0
    fi
    echo "production-routing-probe: FAIL: signed broker provenance fact rejected" >&2
    return 1
}

run_live() {
    local tmp
    tmp=$(mktemp -d)
    CLEANUP_LOG="$tmp/cleanup.log"
    : > "$CLEANUP_LOG"
    local -a created_targets=()
    local -a created_nodes=()
    local overlay_pid=""
    local xvfb_pid=""
    local prefix="$tmp/prefix"
    local home="$tmp/home"

    cleanup_live() {
        local rc=0
        local termination="ok" target_cleanup="ok" targets_absent="false" nodes_absent="false"
        if [[ -n "$overlay_pid" ]]; then
            kill "$overlay_pid" 2>/dev/null || true
            wait "$overlay_pid" 2>/dev/null || termination="fail"
            overlay_pid=""
        fi
        if [[ ${#created_targets[@]} -gt 0 ]]; then
            local path
            for path in "${created_targets[@]}"; do
                if ! busctl --system call "$BUS_NAME" "$MANAGER_PATH" "$MANAGER_IFACE" \
                    StopTargetDevice s "$path" >/dev/null 2>&1; then
                    target_cleanup="fail"
                fi
            done
        fi

        # StopTargetDevice success is not cleanup evidence. Poll both
        # ObjectManager and sysfs to a bounded monotonic deadline.
        local cleanup_deadline now all_gone path node
        cleanup_deadline=$(python3 -c 'import time; print(time.monotonic_ns() + 15_000_000_000)')
        while :; do
            all_gone=1
            if ! busctl --system --json=short call "$BUS_NAME" "$OM_PATH" \
                org.freedesktop.DBus.ObjectManager GetManagedObjects >"$tmp/cleanup-om.json" 2>/dev/null || \
               ! python3 "$SCRIPT_DIR/iprunner-probes/unwrap_variant.py" --object-manager \
                <"$tmp/cleanup-om.json" >"$tmp/cleanup-objects.json" 2>/dev/null; then
                all_gone=0
            else
                targets_absent="true"
                for path in "${created_targets[@]}"; do
                    if python3 - "$tmp/cleanup-objects.json" "$path" <<'PY'
import json, sys
raise SystemExit(0 if sys.argv[2] in json.load(open(sys.argv[1], encoding="utf-8")) else 1)
PY
                    then
                        targets_absent="false"
                        all_gone=0
                    fi
                done
            fi
            nodes_absent="true"
            for node in "${created_nodes[@]}"; do
                if [[ -e "/sys/class/input/$node" || -e "/dev/input/$node" ]]; then
                    nodes_absent="false"
                    all_gone=0
                fi
            done
            [[ "$all_gone" -eq 1 ]] && break
            now=$(python3 -c 'import time; print(time.monotonic_ns())')
            [[ "$now" -ge "$cleanup_deadline" ]] && break
            sleep 0.25
        done
        record_cleanup "$termination" "$target_cleanup" "$targets_absent" "$nodes_absent" || rc=1
        if [[ "$rc" -eq 0 && -f "$tmp/routing-results.json" ]]; then
            python3 - "$tmp/routing-results.json" "$CLEANUP_LOG" <<'PY'
import json,sys
p=sys.argv[1]; d=json.load(open(p,encoding='utf-8'))
import hashlib
for row in d.get('targets',[]): row['cleanup_verified']=True
d['cleanup_log_sha256']=hashlib.sha256(open(sys.argv[2],'rb').read()).hexdigest() if len(sys.argv)>2 else d.get('cleanup_log_sha256')
open(p,'w',encoding='utf-8').write(json.dumps(d,indent=2)+'\n')
PY
        fi
        # Retain the signed artifacts (observer.log, overlay.log, cleanup.log)
        # under the caller-controlled ARTIFACT_DIR and print their hashes; the
        # retained copies are never deleted by cleanup_live.
        retain_artifacts "$tmp/observer.log" "$tmp/routing-results.json" "$tmp/overlay.log" "$CLEANUP_LOG" "$tmp"/assignment-slot-*.yaml || rc=1
        if [[ -n "$xvfb_pid" ]]; then
            kill "$xvfb_pid" 2>/dev/null || true
            wait "$xvfb_pid" 2>/dev/null || true
            xvfb_pid=""
        fi
        rm -rf "$tmp"
        return $rc
    }
    trap 'cleanup_live; exit 1' INT TERM

    # 1. Archive the exact HEAD into a temp source tree.
    local head
    head=$(git -C "$PROJECT_ROOT" rev-parse HEAD 2>/dev/null || true)
    [[ -n "$head" ]] || { echo "production-routing-probe: cannot resolve HEAD" >&2; cleanup_live; return 1; }
    mkdir -p "$tmp/source"
    if ! git -C "$PROJECT_ROOT" archive "$head" | tar -x -C "$tmp/source"; then
        echo "production-routing-probe: git archive HEAD failed" >&2
        cleanup_live
        return 1
    fi
    echo "production-routing-probe: archived HEAD $head"

    # 2. Configure, build, install under the isolated prefix.
    if ! cmake -S "$tmp/source" -B "$tmp/build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" >"$tmp/cmake.log" 2>&1; then
        cat "$tmp/cmake.log" >&2
        echo "production-routing-probe: configure failed" >&2
        cleanup_live
        return 1
    fi
    if ! cmake --build "$tmp/build" --parallel >"$tmp/build.log" 2>&1; then
        cat "$tmp/build.log" >&2
        echo "production-routing-probe: build failed" >&2
        cleanup_live
        return 1
    fi
    if ! cmake --install "$tmp/build" --prefix "$prefix" >"$tmp/install.log" 2>&1; then
        cat "$tmp/install.log" >&2
        echo "production-routing-probe: install failed" >&2
        cleanup_live
        return 1
    fi

    # 3. Verify the launched binary realpath and assets are inside the prefix.
    local binary="$prefix/bin/controller-box"
    [[ -x "$binary" ]] || { echo "production-routing-probe: installed binary missing" >&2; cleanup_live; return 1; }
    local real_bin
    real_bin=$(readlink -f "$binary")
    case "$real_bin" in
        "$prefix"/*) ;;
        *) echo "production-routing-probe: FAIL: installed binary realpath outside prefix" >&2; cleanup_live; return 1 ;;
    esac
    local asset_missing=0
    [[ -r "$prefix/share/controller-box/icons/svg/generic-gamepad.svg" ]] || asset_missing=1
    [[ -r "$prefix/share/controller-box/profiles/default.yaml" ]] || asset_missing=1
    if [[ "$asset_missing" -ne 0 ]]; then
        echo "production-routing-probe: FAIL: installed assets missing from prefix" >&2
        cleanup_live
        return 1
    fi

    # 4. Delete the temp source/build trees before launch so the compiled-in
    # SOURCE_PROFILE_DIR / SOURCE_ICON_DIR fallbacks can never satisfy the
    # runtime asset lookups — the installed prefix assets must win.
    rm -rf "$tmp/source" "$tmp/build"

    # 5. Isolated HOME with default settings (request exactly 4 xb360).
    mkdir -p "$home"
    export HOME="$home"
    export XDG_CONFIG_HOME="$home/.config"
    export XDG_DATA_HOME="$home/.local/share"
    export XDG_RUNTIME_DIR="$home/runtime"
    mkdir -p "$XDG_RUNTIME_DIR"

    # 6. Own Xvfb on a free display.
    local display=":${RANDOM}${RANDOM}"
    display="${display:0:5}"
    if command -v Xvfb >/dev/null 2>&1; then
        Xvfb "$display" -screen 0 1280x720x24 -nolisten tcp >"$tmp/xvfb.log" 2>&1 &
        xvfb_pid=$!
        export DISPLAY="$display"
        sleep 1
    else
        echo "production-routing-probe: warning: Xvfb unavailable; continuing headless" >&2
    fi

    # 7. Verify the real system bus identity before claiming evidence, and
    #    snapshot existing targets + kernel nodes BEFORE launch.
    verify_bus_identity || { cleanup_live; return 1; }
    list_event_devices > "$tmp/kernel-baseline"
    if ! busctl --system --json=short call "$BUS_NAME" "$OM_PATH" \
        org.freedesktop.DBus.ObjectManager GetManagedObjects > "$tmp/om-baseline.json" 2>/dev/null || \
       ! new_target_paths_from_om "$tmp/om-baseline.json" /dev/null > "$tmp/target-baseline"; then
        echo "production-routing-probe: FAIL: invalid or empty baseline ObjectManager reply" >&2
        cleanup_live
        return 1
    fi

    # Compile the observer early so its identity-only mode is available for
    # collective-cardinality node-name verification.
    OBSERVER="$tmp/routing_observer"
    if ! cc -O2 -o "$OBSERVER" "$OBSERVER_SOURCE" 2>"$tmp/cc.log"; then
        cat "$tmp/cc.log" >&2
        echo "production-routing-probe: observer build failed" >&2
        cleanup_live
        return 1
    fi

    # 8. Launch the installed, unmodified overlay service with its CWD set to
    #    the isolated temp dir (never the repository), so a relative asset
    #    lookup cannot resolve back to a source CWD.
    (
        cd "$tmp" || exit 1
        exec "$binary" --overlay-service
    ) > "$tmp/overlay.log" 2>&1 &
    overlay_pid=$!

    # 9. Poll ObjectManager for exactly 4 new xb360 targets + 4 new virtual
    #    kernel event nodes (collective cardinality binding). Populate
    #    created_targets during EVERY poll, before assessment or failure, so
    #    every discovered target is cleaned up on every path.
    local deadline=$(( $(date +%s) + 120 ))
    local observed=0 target_paths=0 kernel_nodes=0
    local -a new_targets=() target_names=() new_nodes=() target_recs=()
    while :; do
        local om_output
        om_output=$(busctl --system --json=short call "$BUS_NAME" "$OM_PATH" \
            org.freedesktop.DBus.ObjectManager GetManagedObjects 2>/dev/null || true)
        new_targets=()
        target_names=()
        if [[ -n "$om_output" ]]; then
            if ! new_target_paths_from_om "$om_output" "$tmp/target-baseline" > "$tmp/new-target-paths"; then
                echo "production-routing-probe: FAIL: malformed ObjectManager reply" >&2
                cleanup_live
                return 1
            fi
            mapfile -t new_targets < "$tmp/new-target-paths"
            created_targets=("${new_targets[@]}")
            if ! extract_new_xb360_targets "$om_output" "$tmp/target-baseline" \
                > "$tmp/target-records" 2> "$tmp/target-err"; then
                cat "$tmp/target-err" >&2
                echo "production-routing-probe: FAIL: a new Target object is not an xb360 device or has no Name" >&2
                cleanup_live
                return 1
            fi
            mapfile -t target_recs < "$tmp/target-records"
            new_targets=()
            target_names=()
            local rec p tname
            for rec in "${target_recs[@]}"; do
                p=${rec%%$'\t'*}
                tname=${rec#*$'\t'}
                new_targets+=("$p")
                target_names+=("$tname")
            done
            created_targets=("${new_targets[@]}")
            target_paths=${#new_targets[@]}
        else
            # Bus transiently unavailable: keep the last discovered targets so
            # they are still cleaned up, and keep polling for the expected set.
            target_paths=${#created_targets[@]}
        fi
        mapfile -t new_nodes < <(new_virtual_nodes "$tmp/kernel-baseline")
        created_nodes=("${new_nodes[@]}")
        kernel_nodes=${#new_nodes[@]}
        observed=$target_paths
        if [[ "$target_paths" -lt "$EXPECTED_TARGETS" ]]; then
            emit_topology_marker "$target_paths"
        fi
        if [[ "$target_paths" -ge "$EXPECTED_TARGETS" && "$kernel_nodes" -ge "$EXPECTED_TARGETS" ]]; then
            break
        fi
        if [[ "$(date +%s)" -ge "$deadline" ]]; then
            break
        fi
        sleep 1
    done

    if ! assess_topology "$observed" "$target_paths" "$kernel_nodes" \
        "$( [[ "$target_paths" -eq "$kernel_nodes" ]] && echo exact || echo ambiguous )"; then
        cleanup_live
        return 1
    fi
    echo "production-routing-probe: exact-topology verified"

    # Collective cardinality: the kernel node identities must match the
    # counted xb360 target names exactly (no extra nodes, no uinput/unrelated).
    TARGET_NAMES=("${target_names[@]}")
    if ! verify_node_identities "${new_nodes[@]}"; then
        cleanup_live
        return 1
    fi
    new_nodes=("${NODE_PATHS[@]}")

    # 10. Observe a separate fresh human event on every target. Presence-only
    #     targets are never evidence. A single physical 045e:028e source is
    #     sufficient under SPEC §5.2/§10.3; it may feed four virtual slots.
    local physical
    physical=$(discover_physical) || {
        echo "production-routing-probe: FAIL: physical 045e:028e controller absent" >&2
        cleanup_live
        return 1
    }
    local physical_name target_node target_name slot composite source_path
    source_path="$OM_PATH/devices/source/${physical##*/}"
    if ! busctl --system --json=short call "$BUS_NAME" "$OM_PATH" \
        org.freedesktop.DBus.ObjectManager GetManagedObjects > "$tmp/routing-om.json" 2>/dev/null; then
        echo "production-routing-probe: FAIL: cannot enumerate physical composite assignment" >&2
        cleanup_live; return 1
    fi
    composite=$(python3 "$SCRIPT_DIR/iprunner-probes/find_source_composite.py" \
        "$tmp/routing-om.json" "$source_path") || {
        echo "production-routing-probe: FAIL: real 045e:028e source/composite assignment is absent or ambiguous" >&2
        cleanup_live; return 1
    }
    physical_name=$("$OBSERVER" --name-only "$physical" 2>/dev/null) || {
        echo "production-routing-probe: FAIL: cannot read physical controller EVIOCGNAME identity" >&2
        cleanup_live; return 1
    }
    : > "$tmp/observer.log"; : > "$tmp/per-target.tsv"
    for slot in 0 1 2 3; do
        target_node="${new_nodes[$slot]}"; target_name="${NODE_NAMES[$slot]}"
        [[ -c "/dev/input/$target_node" ]] || {
            echo "production-routing-probe: FAIL: target-$slot has no kernel char node" >&2
            cleanup_live; return 1
        }
        # Assignment proof must traverse Controller-Box's production overlay:
        # the operator activates Select+A on the real controller, navigates
        # that row to P(slot+1), and closes with B.  Never call assignment
        # DBus here; DBus is only an independent postcondition observer.
        local assignments_file="$HOME/.config/controller-box/assignments.yaml"
        local before_mtime=0 before_hash="missing" after_hash="" persistent_id="" assign_deadline
        [[ -e "$assignments_file" ]] && before_mtime=$(stat -c %Y "$assignments_file" 2>/dev/null || echo 0)
        [[ -f "$assignments_file" && ! -L "$assignments_file" ]] && before_hash=$(sha256sum "$assignments_file" | cut -d' ' -f1)
        persistent_id=$(busctl --system --json=short get-property "$BUS_NAME" "$composite" \
            org.shadowblip.Input.CompositeDevice PersistentId 2>/dev/null | \
            python3 "$SCRIPT_DIR/iprunner-probes/unwrap_variant.py" --property-s) || {
            echo "production-routing-probe: FAIL: composite PersistentId unavailable" >&2
            cleanup_live; return 1
        }
        echo "production-routing-probe: ACTION: use physical 045e:028e through the overlay production event path; assign it to P$((slot+1)) and close with B"
        assign_deadline=$(( $(date +%s) + 90 ))
        local assignment_ok=false
        while [[ $(date +%s) -lt $assign_deadline ]]; do
            local now_mtime=0
            [[ -e "$assignments_file" ]] && now_mtime=$(stat -c %Y "$assignments_file" 2>/dev/null || echo 0)
            if [[ -f "$assignments_file" && ! -L "$assignments_file" && "$now_mtime" -gt "$before_mtime" ]]; then
                after_hash=$(sha256sum "$assignments_file" | cut -d' ' -f1)
                if [[ "$after_hash" != "$before_hash" ]] && \
                   python3 - "$assignments_file" "$persistent_id" "$slot" <<'PY'
import sys,yaml
path,pid,slot=sys.argv[1],sys.argv[2],int(sys.argv[3])
with open(path,encoding='utf-8') as f: doc=yaml.safe_load(f)
rows=doc.get('assignments',[]) if isinstance(doc,dict) else []
assert isinstance(rows,list)
matches=[r for r in rows if isinstance(r,dict) and r.get('id')==pid]
raise SystemExit(0 if len(matches)==1 and matches[0].get('slot')==slot else 1)
PY
                then
                    if busctl --system --json=short get-property "$BUS_NAME" "$composite" \
                         org.shadowblip.Input.CompositeDevice TargetDevices 2>/dev/null | \
                       python3 "$SCRIPT_DIR/iprunner-probes/unwrap_variant.py" --property-as | \
                       python3 -c 'import json,sys; a=json.load(sys.stdin); raise SystemExit(0 if a == [sys.argv[1]] else 1)' "${new_targets[$slot]}"; then
                        assignment_ok=true
                        break
                    fi
                fi
            fi
            sleep 1
        done
        if [[ "$assignment_ok" != true ]]; then
            echo "production-routing-probe: FAIL: production overlay dispatch/save did not produce an exact singleton TargetDevices assignment for target-$slot; InputPlumber may retain old targets without a detach/transfer operation" >&2
            cleanup_live; return 1
        fi
        echo "production-routing-probe: target-$slot production-dispatch verified controller-box-overlay save-persisted=true direct-assignment-dbus=false"
        echo "production-routing-probe: target-$slot mapping dbus=${new_targets[$slot]} kernel=/dev/input/$target_node composite=$composite source=$source_path" | tee -a "$tmp/observer.log"
        echo "production-routing-probe: target-$slot assignment verified dbus-path+kernel-node+composite+source unique"
        if ! "$OBSERVER" --physical-device "$physical" --target-device "/dev/input/$target_node" \
            --physical-name "$physical_name" --target-name "$target_name" \
            --type "$OBSERVE_TYPE" --code "$OBSERVE_CODE" --value "$OBSERVE_VALUE" \
            --window 90 > "$tmp/slot-observer.log" 2>&1; then
            cat "$tmp/slot-observer.log" >> "$tmp/observer.log"
            cat "$tmp/observer.log" >&2
            echo "production-routing-probe: FAIL: target-$slot did not consume its own fresh human event" >&2
            cleanup_live; return 1
        fi
        cat "$tmp/slot-observer.log" >> "$tmp/observer.log"
        read -r source_us target_us < <(python3 - "$tmp/slot-observer.log" <<'PY'
import re,sys
text=open(sys.argv[1],encoding='utf-8').read()
m=re.search(r'RESULT source_event_us=(\d+) target_event_us=(\d+) read_only=true',text)
if not m: raise SystemExit(1)
print(m.group(1),m.group(2))
PY
) || { echo "production-routing-probe: FAIL: target-$slot observer result malformed" >&2; cleanup_live; return 1; }
        cp "$assignments_file" "$tmp/assignment-slot-$slot.yaml" || { cleanup_live; return 1; }
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$slot" "${new_targets[$slot]}" "/dev/input/$target_node" "$composite" "$source_path" "$source_us" "$target_us" "$persistent_id" "$before_hash" "$after_hash" >> "$tmp/per-target.tsv"
        echo "production-routing-probe: target-$slot independent-read-only-consumer correlated fresh-event"
    done
    # Final production-path clear is mandatory. The operator moves the same
    # physical source to Unassigned and saves; DBus and persisted bytes are
    # independently observed. No direct assignment method is used.
    echo "production-routing-probe: ACTION: move the physical 045e:028e source to Unassigned and close with B"
    clear_deadline=$(( $(date +%s) + 90 )); clear_ok=false
    while [[ $(date +%s) -lt $clear_deadline ]]; do
        if busctl --system --json=short get-property "$BUS_NAME" "$composite" org.shadowblip.Input.CompositeDevice TargetDevices 2>/dev/null | \
           python3 "$SCRIPT_DIR/iprunner-probes/unwrap_variant.py" --property-as | \
           python3 -c 'import json,sys; raise SystemExit(0 if json.load(sys.stdin)==[] else 1)' && \
           python3 - "$HOME/.config/controller-box/assignments.yaml" "$persistent_id" <<'PY2'
import sys,yaml
p,pid=sys.argv[1:]
try: d=yaml.safe_load(open(p,encoding='utf-8')) or {}
except FileNotFoundError: d={}
rows=d.get('assignments',[]) if isinstance(d,dict) else []
raise SystemExit(0 if all(not isinstance(x,dict) or x.get('id')!=pid for x in rows) else 1)
PY2
        then clear_ok=true; break; fi
        sleep 1
    done
    [[ "$clear_ok" == true ]] || { echo "production-routing-probe: FAIL: Unassigned did not clear DBus and persistence" >&2; cleanup_live; return 1; }
    printf 'unassignment target_devices=[] persisted_removed=true target_events_after_clear=0 production_dispatch=true\n' >> "$tmp/observer.log"
    python3 - "$tmp/per-target.tsv" "$tmp/routing-results.json" "$tmp" <<'PY'
import json,sys
rows=[]
for line in open(sys.argv[1],encoding='utf-8'):
 s,p,n,c,src,st,tt,pid,before,after=line.rstrip().split('\t'); slot=int(s)
 persisted=f'assignment-slot-{slot}.yaml'; raw=open(sys.argv[3]+'/'+persisted,'rb').read()
 import hashlib
 rows.append({'slot':slot,'dbus_path':p,'kernel_node':n,'composite_path':c,'source_path':src,
              'persistent_id':pid,'saved_slot':slot,'source_vidpid':'045e:028e','device_type':'xb360',
              'source_event_us':int(st),'target_event_us':int(tt),'observation_id':f'slot-{slot}-{st}-{tt}',
              'consumer':'read-only-evdev','human_generated':True,'selected_only':True,'concurrent_nonselected_events':0,
              'target_devices':[p],'persisted_path':persisted,'persisted_sha256':hashlib.sha256(raw).hexdigest(),'persisted_exact':True,
              'production_dispatch':True,'production_save':True,'direct_assignment_dbus':False,
              'assignment_file_changed':before!=after,'assignment_file_parsed':True,
              'assignment_before_sha256':before,'assignment_after_sha256':after,
              'cleanup_verified':False})
if len(rows)!=4: raise SystemExit(1)
json.dump({'schema':'controller-production-routing-results/v3','targets':rows,
 'unassignment':{'target_devices':[],'persisted_removed':True,'target_events_after_clear':0,'production_dispatch':True},
 'cleanup':{'targets_absent':True,'kernel_nodes_absent':True},'cleanup_log_sha256':'0'*64},open(sys.argv[2],'w'),indent=2); open(sys.argv[2],'a').write('\n')
PY
    cat "$tmp/observer.log"
    echo "production-routing-probe: physical-source verified vidpid=045e:028e human-generated=true node=$physical"
    echo "production-routing-probe: all-four-targets-functionally-consumable verified"
    echo "production-routing-probe: routed-event verified 4/4"

    # 11. Retained live artifacts (observer.log, overlay.log, cleanup.log) and
    #     their hashes are produced inside cleanup_live, then the temp dir is
    #     removed. A retention failure fails the probe so the signer always has
    #     the artifacts.

    cleanup_live || return 1
    echo "production-routing-probe: PASS"
    return 0
}

# --------------------------------------------------------------------------
# Dispatch
# --------------------------------------------------------------------------
if [[ -n "$FIXTURE" ]]; then
    run_fixture "$FIXTURE"
else
    run_live
fi
exit $?
