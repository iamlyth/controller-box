#!/usr/bin/env bash
# create-xb360-targets.sh — candidate provisioning helper that creates exactly
# 4 xb360 virtual targets directly on the real InputPlumber system bus, then
# validates that the DBus Target.Name set collectively matches the evdev
# EVIOCGNAME set of the newly created kernel event nodes (exact count, no
# extra nodes), then stops all 4 targets.
#
# This helper is used during live runner provisioning to EMPIRICALLY validate
# the DBus Target.Name <-> EVIOCGNAME identity/cardinality assumption while
# BUG-0015 remains 0/4 (no production-routing evidence claim is ever made
# here). It NEVER injects events and never touches the product binary.
#
# Fixture mode (--fixture DIR) is deterministic/adversarial-test only: it
# validates the same set/cardinality logic against a committed fixture
# (target-names + node-names files) without touching the bus. The committed
# contract probe_argv never passes --fixture.
#
# Usage:
#   create-xb360-targets.sh [--fixture DIR]
set -u

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# iprunner-probes live beside this script in the deployed runner-authority
# layout and under .factory/runner in the repository layout. Resolve both so
# the same helper works from the repo and from the installed authority tree.
# A symlinked probes dir is rejected as unsafe ambiguity (existing policy).
if [[ -d "$SCRIPT_DIR/iprunner-probes" && ! -L "$SCRIPT_DIR/iprunner-probes" ]]; then
    IPROBES="$SCRIPT_DIR/iprunner-probes"
else
    IPROBES="$(cd -- "$SCRIPT_DIR/.." && pwd)/.factory/runner/iprunner-probes"
    [[ -d "$IPROBES" && ! -L "$IPROBES" ]] || {
        echo "create-xb360-targets: iprunner-probes dir missing or unsafe" >&2
        exit 1
    }
fi
VALIDATE="$IPROBES/validate_name_sets.py"
EXTRACT="$IPROBES/extract_om_targets.py"
OBSERVER_SOURCE="$IPROBES/routing_observer.c"
EXPECTED_TARGETS=4
TARGET_KIND="xb360"
BUS_NAME="org.shadowblip.InputPlumber"
MANAGER_PATH="/org/shadowblip/InputPlumber/Manager"
MANAGER_IFACE="org.shadowblip.InputManager"
OM_PATH="/org/shadowblip/InputPlumber"
TARGET_IFACE="org.shadowblip.Input.Target"
PINNED_INPUTPLUMBER="/usr/bin/inputplumber"

[[ -f "$VALIDATE" && -f "$EXTRACT" && -f "$OBSERVER_SOURCE" ]] || {
    echo "create-xb360-targets: helper sources missing" >&2
    exit 1
}

FIXTURE=""
if [[ $# -eq 2 && "$1" == "--fixture" ]]; then
    FIXTURE=$2
elif [[ $# -ne 0 ]]; then
    echo "create-xb360-targets: usage: $0 [--fixture DIR]" >&2
    exit 2
fi

if [[ $(id -u) -eq 0 ]]; then
    echo "create-xb360-targets: must not run as root" >&2
    exit 1
fi
if [[ -n "${DBUS_SYSTEM_BUS_ADDRESS:-}" ]]; then
    echo "create-xb360-targets: DBUS_SYSTEM_BUS_ADDRESS is set; refusing a private bus" >&2
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

# Snapshot newly created virtual (non-USB) kernel event nodes not in baseline.
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
            *usb*) continue ;;
            "") continue ;;
        esac
        nodes+=("$entry")
    done <<< "$new"
    printf '%s\n' "${nodes[@]}"
}

# Validate a name set against the DBus Target.Name set (used by both modes).
# Returns 0 on exact collective match.
validate_sets() {
    python3 "$VALIDATE" --expected "$EXPECTED_TARGETS" \
        --target-names "$1" --node-names "$2"
}

run_fixture() {
    local dir=$1
    [[ -d "$dir" && ! -L "$dir" ]] || {
        echo "create-xb360-targets: fixture directory missing or unsafe" >&2
        return 1
    }
    [[ -f "$dir/target-names" && ! -L "$dir/target-names" ]] || {
        echo "create-xb360-targets: fixture target-names missing or unsafe" >&2
        return 1
    }
    [[ -f "$dir/node-names" && ! -L "$dir/node-names" ]] || {
        echo "create-xb360-targets: fixture node-names missing or unsafe" >&2
        return 1
    }
    if ! validate_sets "$dir/target-names" "$dir/node-names"; then
        echo "create-xb360-targets: FAIL: Target.Name <-> EVIOCGNAME set mismatch" >&2
        return 1
    fi
    echo "create-xb360-targets: PASS (collective name/cardinality validated)"
    return 0
}

run_live() {
    local tmp
    tmp=$(mktemp -d)
    local -a created_paths=()
    cleanup_helper() {
        local stop_fail=0
        if [[ ${#created_paths[@]} -gt 0 ]]; then
            local path
            for path in "${created_paths[@]}"; do
                if ! busctl --system call "$BUS_NAME" "$MANAGER_PATH" "$MANAGER_IFACE" \
                    StopTargetDevice s "$path" >/dev/null 2>&1; then
                    stop_fail=1
                fi
            done
        fi
        rm -rf "$tmp"
        if [[ "$stop_fail" -ne 0 ]]; then
            echo "create-xb360-targets: FAIL: could not stop all created targets" >&2
            return 1
        fi
        return 0
    }
    trap 'cleanup_helper; exit 1' INT TERM

    # Real bus identity: this helper only runs against the pinned InputPlumber.
    [[ -S /run/dbus/system_bus_socket ]] || { echo "create-xb360-targets: system bus socket missing" >&2; cleanup_helper; return 1; }
    [[ "$(stat -c %U /run/dbus/system_bus_socket 2>/dev/null || true)" == "root" ]] || {
        echo "create-xb360-targets: system bus socket is not root-owned" >&2; cleanup_helper; return 1; }
    local pid exe
    pid=$(busctl --system call org.freedesktop.DBus /org/freedesktop/DBus \
        org.freedesktop.DBus GetConnectionUnixProcessID s "$BUS_NAME" 2>/dev/null | awk '{print $2}')
    exe=$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)
    [[ "$exe" == "$PINNED_INPUTPLUMBER" ]] || {
        echo "create-xb360-targets: FAIL: InputPlumber owner realpath '$exe' is not the pinned $PINNED_INPUTPLUMBER" >&2
        cleanup_helper
        return 1
    }

    # Baseline snapshot of kernel nodes + existing target paths.
    list_event_devices > "$tmp/kernel-baseline"
    if ! busctl --system --json=short call "$BUS_NAME" "$OM_PATH" \
        org.freedesktop.DBus.ObjectManager GetManagedObjects > "$tmp/om-baseline.json" 2>/dev/null || \
       ! python3 "$EXTRACT" --all-paths "$tmp/om-baseline.json" /dev/null "$TARGET_IFACE" \
        > "$tmp/target-baseline"; then
        echo "create-xb360-targets: invalid or empty baseline ObjectManager reply" >&2
        cleanup_helper
        return 1
    fi

    # Compile the observer for EVIOCGNAME identity reads.
    local observer="$tmp/routing_observer"
    if ! cc -O2 -o "$observer" "$OBSERVER_SOURCE" 2>"$tmp/cc.log"; then
        cat "$tmp/cc.log" >&2
        echo "create-xb360-targets: observer build failed" >&2
        cleanup_helper
        return 1
    fi

    # Create exactly 4 xb360 targets directly on InputPlumber.
    local n
    for n in 1 2 3 4; do
        local out path
        out=$(busctl --system --json=short call "$BUS_NAME" "$MANAGER_PATH" "$MANAGER_IFACE" \
            CreateTargetDevice s "$TARGET_KIND" 2>/dev/null) || {
            echo "create-xb360-targets: CreateTargetDevice failed (target $n)" >&2
            cleanup_helper
            return 1
        }
        path=$(printf '%s\n' "$out" | python3 "$IPROBES/unwrap_variant.py" --object-path 2>/dev/null | \
            python3 -c 'import json,sys; print(json.load(sys.stdin))' 2>/dev/null || true)
        [[ -n "$path" ]] || {
            echo "create-xb360-targets: CreateTargetDevice returned no path" >&2
            cleanup_helper
            return 1
        }
        created_paths+=("$path")
        echo "create-xb360-targets: created target $path"
    done

    # Collect the 4 new Target.Name values (defensively unwrapped) and require
    # exactly 4 xb360 targets with non-empty names.
    local om_output
    om_output=$(busctl --system --json=short call "$BUS_NAME" "$OM_PATH" \
        org.freedesktop.DBus.ObjectManager GetManagedObjects 2>/dev/null || true)
    if ! python3 "$EXTRACT" "$om_output" "$tmp/target-baseline" "$TARGET_IFACE" \
        > "$tmp/target-records" 2> "$tmp/target-err"; then
        cat "$tmp/target-err" >&2
        echo "create-xb360-targets: FAIL: created targets are not xb360 or lack Name" >&2
        cleanup_helper
        return 1
    fi
    : > "$tmp/target-names"
    while IFS=$'\t' read -r _rec _name; do
        [[ -n "$_name" ]] && printf '%s\n' "$_name" >> "$tmp/target-names"
    done < "$tmp/target-records"

    # Read EVIOCGNAME of each newly created node.
    local -a nodes
    mapfile -t nodes < <(new_virtual_nodes "$tmp/kernel-baseline")
    : > "$tmp/node-names"
    local node evname
    for node in "${nodes[@]}"; do
        evname=$("$observer" --name-only "/dev/input/$node" 2>/dev/null) || {
            echo "create-xb360-targets: FAIL: node /dev/input/$node has no readable evdev identity" >&2
            cleanup_helper
            return 1
        }
        printf '%s\n' "$evname" >> "$tmp/node-names"
    done

    # Validate DBus Target.Name <-> EVIOCGNAME set/cardinality.
    if ! validate_sets "$tmp/target-names" "$tmp/node-names"; then
        echo "create-xb360-targets: FAIL: Target.Name <-> EVIOCGNAME set mismatch" >&2
        cleanup_helper
        return 1
    fi
    echo "create-xb360-targets: PASS (4 xb360 Target.Name collectively match 4 EVIOCGNAME nodes)"

    cleanup_helper || return 1
    echo "create-xb360-targets: all 4 targets stopped"
    return 0
}

if [[ -n "$FIXTURE" ]]; then
    run_fixture "$FIXTURE"
else
    run_live
fi
exit $?
