#!/usr/bin/env bash
# probe-controller-production-routing.sh — CANDIDATE capability probe for
# `controller-production-routing` (Controller-Box infra; runs only under the
# dedicated unprivileged iprunner class on the live runner).
#
# Live behavior:
#   1. Refuse root, a private/system-bus-address override, and a non-default
#      system DBus.
#   2. Archive the exact HEAD into a temp source tree, then configure, build,
#      and install under an isolated prefix.
#   3. Verify the launched binary realpath and its assets all resolve inside
#      the prefix, then DELETE the temp source/build trees before launch so
#      the compiled-in SOURCE_PROFILE_DIR / SOURCE_ICON_DIR fallbacks can
#      never satisfy the runtime asset lookups (installed assets must win).
#   4. Run under an isolated HOME (defaults request exactly 4 xb360 virtual
#      controllers) on its own Xvfb.
#   5. Snapshot the real InputPlumber Target object paths and kernel event
#      nodes BEFORE launch, then launch the installed, unmodified
#      `controller-box --overlay-service`.
#   6. Poll the ObjectManager for exactly 4 newly created
#      org.shadowblip.Input.Target paths and exactly 4 new matching virtual
#      xb360 kernel event nodes (collective cardinality binding; extra or
#      ambiguous nodes fail). While the count is below 4 it emits the
#      distinct immediate marker `production-topology-incomplete: N of 4`;
#      when N is 0 it additionally emits `BUG-0015-negative-control`.
#   7. Then, on separate read-only file descriptors, require a fresh physical
#      045e:028e controller event and a matching event on a new virtual xb360
#      target node (the helper polls both; it never writes, never uses uinput,
#      never calls InputEvent directly, and never uses a private DBus).
#   8. Preserve fact/log hashes and require clean termination + target
#      cleanup on every path. PASS only with 4 targets + a fresh
#      physical->target routed event + clean cleanup.
#
# Fixture mode (--fixture DIR) is adversarial-test only: it validates a
# committed JSON fact bundle (schema + artifact sha256 hashes) and drives the
# exact same decision logic from the recorded facts. The committed contract
# probe_argv never passes --fixture, so production runs always follow the
# live path above.
#
# Usage:
#   probe-controller-production-routing.sh [--fixture DIR]
set -u

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
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

[[ -f "$VALIDATOR" && -f "$OBSERVER_SOURCE" ]] || {
    echo "production-routing-probe: validator or observer source missing" >&2
    exit 1
}

FIXTURE=""
if [[ $# -eq 2 && "$1" == "--fixture" ]]; then
    FIXTURE=$2
elif [[ $# -ne 0 ]]; then
    echo "production-routing-probe: usage: $0 [--fixture DIR]" >&2
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
    local termination=$1 target_cleanup=$2
    printf 'cleanup: termination=%s target-cleanup=%s\n' "$termination" "$target_cleanup" >> "$CLEANUP_LOG"
    if [[ "$termination" != "ok" || "$target_cleanup" != "ok" ]]; then
        echo "production-routing-probe: cleanup failure (termination=$termination target-cleanup=$target_cleanup)" >&2
        return 1
    fi
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

    # Validate the committed fact bundle + artifact sha256 hashes. A missing
    # or mismatched artifact hash fails the probe here (missing screenshot /
    # log / fact hash).
    if ! python3 "$VALIDATOR" --facts "$dir/facts.json" --fixture-dir "$dir"; then
        echo "production-routing-probe: fact bundle or artifact hash verification failed" >&2
        return 1
    fi

    # Read the recorded facts into shell variables.
    readarray -t FACTS < <(python3 - "$dir/facts.json" "$dir" <<'PY'
import json, sys
facts = json.load(open(sys.argv[1], encoding="utf-8"))
base = sys.argv[2]
binary = facts["binary"]
topo = facts["topology"]
observer = facts.get("observer", {})
cleanup = facts.get("cleanup", {})
print(int(binary["realpath_inside_prefix"]))
print(int(binary["assets_inside_prefix"]))
print(int(topo["expected"]))
print(int(topo["observed"]))
print(int(topo["target_paths"]))
print(int(topo["kernel_nodes"]))
print(topo.get("cardinality", "exact"))
print(observer.get("event_stream", ""))
print(int(observer.get("direct_injection", 0)))
print(cleanup.get("termination", "ok"))
print(cleanup.get("target_cleanup", "ok"))
print(observer.get("physical_name", ""))
print(observer.get("target_name", ""))
PY
)
    local realpath_inside=${FACTS[0]} assets_inside=${FACTS[1]}
    local observed=${FACTS[3]}
    local target_paths=${FACTS[4]} kernel_nodes=${FACTS[5]} cardinality=${FACTS[6]}
    local event_stream=${FACTS[7]} direct_injection=${FACTS[8]}
    local termination=${FACTS[9]} target_cleanup=${FACTS[10]}
    local physical_name=${FACTS[11]} target_name=${FACTS[12]}

    # Source/build or asset fallback: the installed binary must be the one
    # launched and its assets must live inside the prefix. Deleting the temp
    # source/build trees before launch is what makes SOURCE_* fallbacks
    # impossible; the recorded facts must confirm the installed path won.
    if [[ "$realpath_inside" -ne 1 || "$assets_inside" -ne 1 ]]; then
        echo "production-routing-probe: FAIL: source/build or asset fallback detected (binary realpath inside prefix=$realpath_inside assets inside prefix=$assets_inside)" >&2
        record_cleanup "$termination" "$target_cleanup"
        return 1
    fi

    # Topology cardinality assessment.
    if ! assess_topology "$observed" "$target_paths" "$kernel_nodes" "$cardinality"; then
        record_cleanup "$termination" "$target_cleanup"
        return 1
    fi

    # Observer: replay the recorded physical->target event stream through the
    # same C helper used live. Deterministic, read-only, no injection.
    if [[ -n "$event_stream" ]]; then
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
            echo "production-routing-probe: FAIL: no fresh physical->target routed event observed (stale/injected/unrouted)" >&2
            record_cleanup "$termination" "$target_cleanup"
            return 1
        fi
        cat "$dir/observer.log"
    else
        # No event stream provided: the recorded run produced no routed event.
        echo "production-routing-probe: FAIL: no routed physical->target event recorded" >&2
        record_cleanup "$termination" "$target_cleanup"
        return 1
    fi

    # Cleanup must itself succeed.
    if ! record_cleanup "$termination" "$target_cleanup"; then
        return 1
    fi

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

# New org.shadowblip.Input.Target object paths not in the baseline set.
new_target_paths() {
    local baseline=$1
    local output after new
    if output=$(busctl --system --json=short call "$BUS_NAME" "$OM_PATH" \
        org.freedesktop.DBus.ObjectManager GetManagedObjects 2>/dev/null); then
        after=$(python3 - "$output" "$TARGET_IFACE" <<'PY'
import json, sys
data = json.loads(sys.argv[1])
paths = []
for path, interfaces in data.items():
    if sys.argv[2] in interfaces:
        paths.append(path)
print("\n".join(sorted(paths)))
PY
        )
    fi
    new=$(comm -13 <(cat "$baseline") <(printf '%s\n' "$after") || true)
    printf '%s\n' "$new"
}

run_live() {
    local tmp
    tmp=$(mktemp -d)
    CLEANUP_LOG="$tmp/cleanup.log"
    : > "$CLEANUP_LOG"
    local -a created_targets=()
    local overlay_pid=""
    local xvfb_pid=""
    local prefix="$tmp/prefix"
    local home="$tmp/home"

    cleanup_live() {
        local rc=0
        local termination="ok" target_cleanup="ok"
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
        record_cleanup "$termination" "$target_cleanup" || rc=1
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
    head=$(git -C "$SCRIPT_DIR/.." rev-parse HEAD 2>/dev/null || true)
    [[ -n "$head" ]] || { echo "production-routing-probe: cannot resolve HEAD" >&2; cleanup_live; return 1; }
    mkdir -p "$tmp/source"
    if ! git -C "$SCRIPT_DIR/.." archive "$head" | tar -x -C "$tmp/source"; then
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

    # 7. Snapshot existing targets + kernel nodes BEFORE launch.
    list_event_devices > "$tmp/kernel-baseline"
    busctl --system --json=short call "$BUS_NAME" "$OM_PATH" \
        org.freedesktop.DBus.ObjectManager GetManagedObjects > "$tmp/om-baseline.json" 2>/dev/null || true
    # Derive the baseline target path set (all currently managed Targets).
    if [[ -f "$tmp/om-baseline.json" ]]; then
        python3 - "$tmp/om-baseline.json" "$TARGET_IFACE" <<'PY' > "$tmp/target-baseline"
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
for path, interfaces in data.items():
    if sys.argv[2] in interfaces:
        print(path)
PY
    else
        : > "$tmp/target-baseline"
    fi

    # 8. Launch the installed, unmodified overlay service.
    "$binary" --overlay-service > "$tmp/overlay.log" 2>&1 &
    overlay_pid=$!

    # 9. Poll ObjectManager for exactly 4 new xb360 targets + 4 new virtual
    #    kernel event nodes (collective cardinality binding).
    local deadline=$(( $(date +%s) + 120 ))
    local observed=0 target_paths=0 kernel_nodes=0
    local -a new_targets=() new_nodes=()
    while :; do
        mapfile -t new_targets < <(new_target_paths "$tmp/target-baseline")
        mapfile -t new_nodes < <(new_virtual_nodes "$tmp/kernel-baseline")
        target_paths=${#new_targets[@]}
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
    created_targets=("${new_targets[@]}")

    # 10. Observe a fresh physical 045e:028e event routed to a new virtual
    #     xb360 target node on separate read-only fds.
    local physical
    physical=$(discover_physical) || {
        echo "production-routing-probe: FAIL: physical 045e:028e controller absent" >&2
        cleanup_live
        return 1
    }
    local target_node="${new_nodes[0]}"
    [[ -c "/dev/input/$target_node" ]] || {
        echo "production-routing-probe: FAIL: target node /dev/input/$target_node is not a char device" >&2
        cleanup_live
        return 1
    }
    local observer="$tmp/routing_observer"
    if ! cc -O2 -o "$observer" "$OBSERVER_SOURCE" 2>"$tmp/cc.log"; then
        cat "$tmp/cc.log" >&2
        echo "production-routing-probe: observer build failed" >&2
        cleanup_live
        return 1
    fi
    if ! "$observer" --physical-device "$physical" --target-device "/dev/input/$target_node" \
        --type "$OBSERVE_TYPE" --code "$OBSERVE_CODE" --value "$OBSERVE_VALUE" \
        --window 90 > "$tmp/observer.log" 2>&1; then
        cat "$tmp/observer.log" >&2
        echo "production-routing-probe: FAIL: no fresh physical->target routed event observed" >&2
        cleanup_live
        return 1
    fi
    cat "$tmp/observer.log"

    # 11. Preserve fact/log hashes.
    local -a facts=("$tmp/observer.log" "$tmp/overlay.log")
    for artifact in "${facts[@]}"; do
        echo "production-routing-probe: artifact hash $(sha256sum "$artifact" | cut -d' ' -f1) $artifact"
    done

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
