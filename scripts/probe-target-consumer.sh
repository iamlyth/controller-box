#!/usr/bin/env bash
# probe-target-consumer.sh — CANDIDATE capability probe for `target-consumer`
# (Controller-Box infra; runs only under the dedicated unprivileged iprunner
# class on the live runner).
#
# Uses the real InputPlumber system bus to create/attach an xb360 target,
# independently discovers the new kernel event device (sysfs diff, not an
# InputPlumber-claimed path), sends one real InputEvent through the Target
# interface, and requires the exact resulting kernel event to be observed on
# a separate consumer file descriptor. Cleanup (StopTargetDevice + restoring
# GamepadOrder) runs on every path and must itself succeed. If the current
# API or product cannot produce an observable event, the probe fails honestly
# and the capability stays undeclared. It never uses a private bus and never
# injects uinput events.
#
# Usage:
#   probe-target-consumer.sh [--fixture DIR] [--expectations FILE]
#
#   --fixture DIR   adversarial-test only: replay a scripted scenario. The
#                   committed candidate contract probe_argv never passes it.
set -u

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
EXPECTATIONS="$SCRIPT_DIR/iprunner-probes/target-consumer-expectations.json"
OBSERVER_SOURCE="$SCRIPT_DIR/iprunner-probes/target_consumer_observer.c"
DECODER="$SCRIPT_DIR/iprunner-probes/unwrap_variant.py"

# List kernel event devices by exact name (event + decimal digits).
list_event_devices() {
    local entry name
    for entry in /sys/class/input/event*; do
        [[ -e "$entry" ]] || continue
        name=${entry##*/}
        [[ "$name" =~ ^event[0-9]+$ ]] || continue
        printf '%s\n' "$name"
    done | sort
}

FIXTURE=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --fixture) FIXTURE=$2; shift 2 ;;
        --expectations) EXPECTATIONS=$2; shift 2 ;;
        *) echo "target-consumer-probe: unknown argument: $1" >&2; exit 2 ;;
    esac
done

[[ -f "$EXPECTATIONS" && ! -L "$EXPECTATIONS" ]] || {
    echo "target-consumer-probe: expectations file is missing or unsafe" >&2
    exit 1
}
[[ -f "$OBSERVER_SOURCE" && -x "$DECODER" ]] || {
    echo "target-consumer-probe: observer source or decoder missing" >&2
    exit 1
}

if [[ $(id -u) -eq 0 ]]; then
    echo "target-consumer-probe: must not run as root" >&2
    exit 1
fi

# Read pinned expectations into shell variables.
readarray -t PIN_VALUES < <(python3 - "$EXPECTATIONS" <<'PY'
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
print(data["input_event"]["action"])
print(data["input_event"]["signature_hint"])
print(data["target_kind"])
print(data["observe"]["type"])
print(data["observe"]["code"])
print(data["observe"]["value"])
print(data["timeouts"]["device_appearance_seconds"])
print(data["timeouts"]["consumer_window_seconds"])
print(" ".join(str(b) for b in data["input_event"]["data"]))
print(data["manager"]["bus"])
print(data["manager"]["path"])
print(data["manager"]["interface"])
print(data["target_interface"])
PY
)
EXPECT_ACTION=${PIN_VALUES[0]}
SIGNATURE_HINT=${PIN_VALUES[1]}
TARGET_KIND=${PIN_VALUES[2]}
OBSERVE_TYPE=${PIN_VALUES[3]}
OBSERVE_CODE=${PIN_VALUES[4]}
OBSERVE_VALUE=${PIN_VALUES[5]}
DEVICE_WAIT=${PIN_VALUES[6]}
CONSUMER_WINDOW=${PIN_VALUES[7]}
INPUT_DATA=${PIN_VALUES[8]}
BUS_NAME=${PIN_VALUES[9]}
MANAGER_PATH=${PIN_VALUES[10]}
MANAGER_IFACE=${PIN_VALUES[11]}
TARGET_IFACE=${PIN_VALUES[12]}

tmp=$(mktemp -d)
RESULT=0
TARGET_PATH=""
CONSUMER_PID=""
CLEANUP_LOG="$tmp/cleanup.log"

cleanup() {
    local stop_result="not-attempted" restore_result="not-attempted"
    if [[ -n "$TARGET_PATH" ]]; then
        if [[ -n "$FIXTURE" ]]; then
            if [[ -f "$FIXTURE/stop-result" ]]; then
                read -r stop_result < "$FIXTURE/stop-result"
            else
                stop_result="ok"
            fi
        else
            if busctl --system call "$BUS_NAME" "$MANAGER_PATH" "$MANAGER_IFACE" \
                StopTargetDevice s "$TARGET_PATH" >/dev/null 2>&1; then
                stop_result="ok"
            else
                stop_result="fail"
                echo "target-consumer-probe: StopTargetDevice failed for $TARGET_PATH" >&2
            fi
        fi
        if [[ -n "$FIXTURE" ]]; then
            if [[ -f "$FIXTURE/restore-result" ]]; then
                read -r restore_result < "$FIXTURE/restore-result"
            else
                restore_result="ok"
            fi
        else
            if [[ "${SAVED_ORDER_COUNT:-0}" -eq 0 ]]; then
                if busctl --system set-property "$BUS_NAME" "$MANAGER_PATH" "$MANAGER_IFACE" \
                    GamepadOrder as 0 >/dev/null 2>&1; then
                    restore_result="ok"
                else
                    restore_result="fail"
                    echo "target-consumer-probe: GamepadOrder restore failed" >&2
                fi
            else
                set -- "${SAVED_ORDER[@]}"
                if busctl --system set-property "$BUS_NAME" "$MANAGER_PATH" "$MANAGER_IFACE" \
                    GamepadOrder as "$SAVED_ORDER_COUNT" "$@" >/dev/null 2>&1; then
                    restore_result="ok"
                else
                    restore_result="fail"
                    echo "target-consumer-probe: GamepadOrder restore failed" >&2
                fi
            fi
        fi
    fi
    printf 'cleanup: stop-target=%s restore-gamepad-order=%s\n' "$stop_result" "$restore_result" >> "$CLEANUP_LOG"
    if [[ "$stop_result" != "ok" || "$restore_result" != "ok" ]]; then
        RESULT=1
    fi
    if [[ -n "$CONSUMER_PID" ]]; then
        kill "$CONSUMER_PID" 2>/dev/null || true
        wait "$CONSUMER_PID" 2>/dev/null || true
    fi
}
trap 'cleanup; exit 1' INT TERM
trap 'rm -rf "$tmp"' EXIT

main() {
    # ---- Save GamepadOrder so it can be restored on every path. ----------
    if [[ -n "$FIXTURE" ]]; then
        SAVED_ORDER_COUNT=0
        SAVED_ORDER=()
    else
        if order_json=$(busctl --system --json=short get-property "$BUS_NAME" "$MANAGER_PATH" \
            "$MANAGER_IFACE" GamepadOrder 2>/dev/null); then
            if ! printf '%s\n' "$order_json" | python3 "$DECODER" --property-as > "$tmp/order.json"; then
                echo "target-consumer-probe: malformed GamepadOrder property reply" >&2
                return 1
            fi
            readarray -t SAVED_ORDER < <(python3 - "$tmp/order.json" <<'PY'
import json, sys
print("\n".join(json.load(open(sys.argv[1], encoding="utf-8"))))
PY
            )
            SAVED_ORDER_COUNT=${#SAVED_ORDER[@]}
        else
            echo "target-consumer-probe: cannot read GamepadOrder; restore would be impossible" >&2
            return 1
        fi
    fi

    # ---- Baseline kernel event devices. ----------------------------------
    if [[ -n "$FIXTURE" ]]; then
        if [[ -f "$FIXTURE/device-baseline" ]]; then
            cp "$FIXTURE/device-baseline" "$tmp/baseline"
        else
            : > "$tmp/baseline"
        fi
    else
        list_event_devices > "$tmp/baseline"
    fi

    # ---- Create the xb360 target. ----------------------------------------
    if [[ -n "$FIXTURE" ]]; then
        if [[ -f "$FIXTURE/create-response" ]]; then
            read -r TARGET_PATH < "$FIXTURE/create-response"
            if [[ "$TARGET_PATH" == "FAIL" ]]; then
                echo "target-consumer-probe: CreateTargetDevice failed (fixture)" >&2
                return 1
            fi
        else
            echo "target-consumer-probe: fixture missing create-response" >&2
            return 1
        fi
    else
        if create_output=$(busctl --system --json=short call "$BUS_NAME" "$MANAGER_PATH" "$MANAGER_IFACE" \
            CreateTargetDevice s "$TARGET_KIND" 2>/dev/null); then
            TARGET_PATH=$(printf '%s\n' "$create_output" | python3 "$DECODER" --object-path 2>/dev/null | \
                python3 -c 'import json,sys; print(json.load(sys.stdin))' 2>/dev/null || true)
            echo "target-consumer-probe: created target $TARGET_PATH"
        else
            echo "target-consumer-probe: CreateTargetDevice failed on the real bus" >&2
            return 1
        fi
    fi
    [[ -n "$TARGET_PATH" ]] || {
        echo "target-consumer-probe: CreateTargetDevice returned no path" >&2
        return 1
    }

    # ---- Bind the created DBus target to exactly one new evdev node. ------
    # InputPlumber v0.78 exposes Name and DeviceType on the exact Target
    # object path. It does not expose a devnode, so independently cross-check
    # that identity against every newly appeared kernel event node. Never
    # accept the first new node: zero and multiple matches both fail closed.
    if [[ -n "$FIXTURE" ]]; then
        if [[ ! -f "$FIXTURE/target-identity" ]]; then
            echo "target-consumer-probe: fixture missing target-identity" >&2
            return 1
        fi
        IFS=$'\t' read -r TARGET_NAME TARGET_DEVICE_TYPE < "$FIXTURE/target-identity"
    else
        read_target_property() {
            busctl --system --json=short get-property "$BUS_NAME" "$TARGET_PATH" \
                "$TARGET_IFACE" "$1" 2>/dev/null | python3 "$DECODER" --property-s | \
                python3 -c 'import json,sys; print(json.load(sys.stdin))'
        }
        TARGET_NAME=$(read_target_property Name) || {
            echo "target-consumer-probe: cannot read Name from created target $TARGET_PATH" >&2
            return 1
        }
        TARGET_DEVICE_TYPE=$(read_target_property DeviceType) || {
            echo "target-consumer-probe: cannot read DeviceType from created target $TARGET_PATH" >&2
            return 1
        }
    fi
    if [[ -z "$TARGET_NAME" || "$TARGET_DEVICE_TYPE" != "$TARGET_KIND" ]]; then
        echo "target-consumer-probe: created target identity mismatch (name=$TARGET_NAME type=$TARGET_DEVICE_TYPE expected-type=$TARGET_KIND)" >&2
        return 1
    fi

    discover_device() {
        local deadline=$(( $(date +%s) + DEVICE_WAIT ))
        while :; do
            local after new entry name resolved
            local -a matches=()
            after=$( { if [[ -n "$FIXTURE" ]]; then cat "$FIXTURE/device-after" 2>/dev/null || cat "$tmp/baseline"; else list_event_devices; fi; } )
            new=$(comm -13 <(cat "$tmp/baseline") <(printf '%s\n' "$after") || true)
            while IFS= read -r entry; do
                [[ -n "$entry" ]] || continue
                if [[ -n "$FIXTURE" ]]; then
                    name=$(awk -F '\t' -v node="$entry" '$1 == node {print $2; found=1; exit} END {if (!found) exit 1}' \
                        "$FIXTURE/device-identities" 2>/dev/null) || continue
                else
                    resolved=$(readlink -f "/sys/class/input/$entry/device" 2>/dev/null || true)
                    [[ -n "$resolved" && "$resolved" != *usb* ]] || continue
                    name=$(cat "/sys/class/input/$entry/device/name" 2>/dev/null || true)
                fi
                [[ "$name" == "$TARGET_NAME" ]] && matches+=("$entry")
            done <<< "$new"
            if [[ ${#matches[@]} -eq 1 ]]; then
                printf '%s\n' "${matches[0]}"
                return 0
            fi
            if [[ ${#matches[@]} -gt 1 ]]; then
                echo "target-consumer-probe: ambiguous target identity: ${#matches[@]} new devices match $TARGET_NAME" >&2
                printf '%s\n' "AMBIGUOUS"
                return 2
            fi
            if [[ "$(date +%s)" -ge "$deadline" ]]; then
                printf '%s\n' "STALE_OR_ABSENT"
                return 1
            fi
            sleep 1
        done
    }

    NEW_DEVICE=""
    NEW_DEVICE=$(discover_device) || true
    if [[ "$NEW_DEVICE" == "AMBIGUOUS" ]]; then
        echo "target-consumer-probe: refusing ambiguous kernel target identity" >&2
        return 1
    fi
    if [[ "$NEW_DEVICE" == "STALE_OR_ABSENT" || -z "$NEW_DEVICE" ]]; then
        echo "target-consumer-probe: no uniquely identity-bound kernel event device appeared; FAIL" >&2
        return 1
    fi
    CONSUMER_DEVICE="/dev/input/$NEW_DEVICE"
    if [[ -n "$FIXTURE" ]] || { [[ -c "$CONSUMER_DEVICE" && -r "$CONSUMER_DEVICE" ]]; }; then
        echo "target-consumer-probe: discovered new kernel event device $CONSUMER_DEVICE"
    else
        echo "target-consumer-probe: new kernel event device $CONSUMER_DEVICE is not a readable char device" >&2
        return 1
    fi

    # ---- Verify the Target interface exposes InputEvent. ------------------
    INPUT_SIGNATURE=""
    if [[ -n "$FIXTURE" ]]; then
        if [[ -f "$FIXTURE/input-event-signature" ]]; then
            read -r INPUT_SIGNATURE < "$FIXTURE/input-event-signature"
        else
            INPUT_SIGNATURE="$SIGNATURE_HINT"
        fi
    else
        if busctl --system introspect "$BUS_NAME" "$TARGET_PATH" > "$tmp/introspect" 2>/dev/null; then
            # busctl introspection layout: ".InputEvent method <in> <out> ..."
            INPUT_SIGNATURE=$(awk '$1 == ".InputEvent" && $2 == "method" { print $3; exit }' "$tmp/introspect")
        fi
    fi
    if [[ -z "$INPUT_SIGNATURE" ]]; then
        echo "target-consumer-probe: the Target interface does not expose InputEvent; the current API cannot do this" >&2
        return 1
    fi
    if [[ "$INPUT_SIGNATURE" != "$SIGNATURE_HINT" ]]; then
        echo "target-consumer-probe: Target.InputEvent signature $INPUT_SIGNATURE differs from pinned $SIGNATURE_HINT; re-pin before declaring" >&2
        return 1
    fi

    # ---- Compile and start the independent consumer observer. -------------
    OBSERVER="$tmp/target_consumer_observer"
    if ! cc -O2 -o "$OBSERVER" "$OBSERVER_SOURCE" 2>"$tmp/cc.log"; then
        echo "target-consumer-probe: observer build failed" >&2
        cat "$tmp/cc.log" >&2
        return 1
    fi
    if [[ -n "$FIXTURE" ]]; then
        if [[ -f "$FIXTURE/event-stream" ]]; then
            "$OBSERVER" --fixture "$FIXTURE/event-stream" --type "$OBSERVE_TYPE" \
                --code "$OBSERVE_CODE" --value "$OBSERVE_VALUE" > "$tmp/observer.out" 2>&1 &
        else
            # No event stream means the consumer sees nothing.
            "$OBSERVER" --fixture /dev/null --type "$OBSERVE_TYPE" \
                --code "$OBSERVE_CODE" --value "$OBSERVE_VALUE" > "$tmp/observer.out" 2>&1 &
        fi
    else
        "$OBSERVER" --device "$CONSUMER_DEVICE" --expect-name "$TARGET_NAME" \
            --type "$OBSERVE_TYPE" --code "$OBSERVE_CODE" --value "$OBSERVE_VALUE" --window "$CONSUMER_WINDOW" \
            > "$tmp/observer.out" 2>&1 &
    fi
    CONSUMER_PID=$!

    # ---- Send one real InputEvent through the Target API. -----------------
    EVENT_CALL_OK=0
    if [[ -n "$FIXTURE" ]]; then
        if [[ -f "$FIXTURE/input-event-result" ]]; then
            read -r event_result < "$FIXTURE/input-event-result"
        else
            event_result="ok"
        fi
        if [[ "$event_result" == "ok" ]]; then
            EVENT_CALL_OK=1
        else
            echo "target-consumer-probe: InputEvent call failed (fixture)" >&2
        fi
    else
        # shellcheck disable=SC2086
        read -r -a DATA_BYTES <<< "$INPUT_DATA"
        if busctl --system call "$BUS_NAME" "$TARGET_PATH" "$TARGET_IFACE" InputEvent \
            "$INPUT_SIGNATURE" "$EXPECT_ACTION" "${#DATA_BYTES[@]}" "${DATA_BYTES[@]}" >/dev/null 2>"$tmp/input-event.err"; then
            EVENT_CALL_OK=1
            echo "target-consumer-probe: InputEvent sent (action=$EXPECT_ACTION, ${#DATA_BYTES[@]} bytes)"
        else
            cat "$tmp/input-event.err" >&2
            echo "target-consumer-probe: InputEvent call failed on the real bus" >&2
        fi
    fi
    if [[ "$EVENT_CALL_OK" -ne 1 ]]; then
        echo "target-consumer-probe: no event was sent; the target API call failed (candidate API mismatch; capability stays undeclared)" >&2
        return 1
    fi

    # ---- Require the exact event on the consumer fd. ----------------------
    if ! wait "$CONSUMER_PID"; then
        cat "$tmp/observer.out" >&2
        echo "target-consumer-probe: the exact event was not observed on the consumer device (candidate API/payload mismatch; capability stays undeclared)" >&2
        return 1
    fi
    cat "$tmp/observer.out"
    return 0
}

main || RESULT=$?
cleanup
trap - INT TERM
if [[ -n "$FIXTURE" ]]; then
    cp "$CLEANUP_LOG" "$FIXTURE/cleanup.log" 2>/dev/null || true
fi
if [[ $RESULT -eq 0 ]]; then
    echo "target-consumer-probe: CLEANUP OK"
    echo "target-consumer-probe: PASS"
    exit 0
fi
exit 1
