#!/usr/bin/env bash
# probe-physical-controller.sh — non-skipping capability probe for
# `physical-controller` (Controller-Box infra; runs only under the dedicated
# unprivileged iprunner class on the live runner with the physical pad
# attached).
#
# Discovers the physical USB Xbox 360 pad (VID 045e, PID 028e) from sysfs,
# verifies it is a real USB input device (not a uinput/injected device),
# compiles the SDL probe helper inside the project nix-shell, and requires a
# fresh human-generated event (EV_KEY button or EV_ABS axis) from that exact
# device within the window. Presence alone always fails the probe; there is
# no skip path.
#
# Usage:
#   probe-physical-controller.sh [--fixture FILE] [--window SECONDS] [--vendor V] [--product P] [--event-path PATH]
#
#   --fixture FILE  adversarial-test only: replay a recorded event stream in
#                   the helper's fixture mode. The committed contract
#                   probe_argv never passes this flag.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
HELPER_SOURCE="$SCRIPT_DIR/iprunner-probes/physical_controller_probe.c"

FIXTURE=""
WINDOW=90
VENDOR="0x045e"
PRODUCT="0x028e"
EVENT_PATH=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fixture) FIXTURE=$2; shift 2 ;;
        --window) WINDOW=$2; shift 2 ;;
        --vendor) VENDOR=$2; shift 2 ;;
        --product) PRODUCT=$2; shift 2 ;;
        --event-path) EVENT_PATH=$2; shift 2 ;;
        *) echo "physical-controller-probe: unknown argument: $1" >&2; exit 2 ;;
    esac
done

[[ -f "$HELPER_SOURCE" ]] || { echo "physical-controller-probe: helper source missing" >&2; exit 1; }
[[ "$WINDOW" =~ ^[0-9]+$ && "$WINDOW" -ge 1 ]] || { echo "physical-controller-probe: invalid window" >&2; exit 2; }

if [[ $(id -u) -eq 0 ]]; then
    echo "physical-controller-probe: must not run as root" >&2
    exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if [[ -n "$FIXTURE" ]]; then
    [[ -f "$FIXTURE" && ! -L "$FIXTURE" ]] || {
        echo "physical-controller-probe: fixture file is missing or unsafe" >&2
        exit 1
    }
    # Fixture mode: no compile needed; the test provides its own binary
    # build through the normal nix-shell compile path below.
fi

# Compile the SDL helper inside the project nix-shell (SDL2, cc, pkg-config).
HELPER="$tmp/physical_controller_probe"
# pkg-config emits one whitespace-separated line; read -a splits it into
# individual flags so word splitting is intentional and shellcheck-clean.
read -r -a SDL_FLAGS < <(pkg-config --cflags --libs sdl2)
if ! cc -O2 -o "$HELPER" "$HELPER_SOURCE" "${SDL_FLAGS[@]}" 2>"$tmp/cc.log"; then
    echo "physical-controller-probe: SDL helper build failed" >&2
    cat "$tmp/cc.log" >&2
    exit 1
fi

if [[ -n "$FIXTURE" ]]; then
    exec "$HELPER" --fixture "$FIXTURE"
fi

# Discover the physical USB pad from sysfs. The device must be a USB device
# (its sysfs path contains "usb"); a uinput-created device has a virtual
# sysfs path and is rejected here before SDL is even consulted.
declare -a CANDIDATES=()
for vendor_file in /sys/class/input/event*/device/id/vendor; do
    [[ -r "$vendor_file" ]] || continue
    device_dir=$(dirname "$(dirname "$vendor_file")")
    event_dir=$(dirname "$device_dir")
    event_name=$(basename "$event_dir")
    case "$event_name" in event[0-9]*) ;; *) continue ;; esac
    vendor=$(cat "$vendor_file" 2>/dev/null || true)
    product=$(cat "$device_dir/id/product" 2>/dev/null || true)
    if [[ "$vendor" == "$VENDOR" && "$product" == "$PRODUCT" ]]; then
        sysfs_real=$(readlink -f "$device_dir" 2>/dev/null || true)
        case "$sysfs_real" in
            *usb*) CANDIDATES+=("/dev/input/$event_name") ;;
            *)
                echo "physical-controller-probe: rejecting non-USB device at /dev/input/$event_name (injected/substituted device)" >&2
                ;;
        esac
    fi
done

if [[ ${#CANDIDATES[@]} -eq 0 ]]; then
    echo "physical-controller-probe: FAIL: physical USB controller $VENDOR:$PRODUCT absent" >&2
    exit 1
fi
if [[ ${#CANDIDATES[@]} -gt 1 ]]; then
    echo "physical-controller-probe: FAIL: more than one physical $VENDOR:$PRODUCT device found" >&2
    exit 1
fi
EVENT_PATH=${EVENT_PATH:-${CANDIDATES[0]}}
[[ -c "$EVENT_PATH" && -r "$EVENT_PATH" ]] || {
    echo "physical-controller-probe: FAIL: physical event device $EVENT_PATH is unreadable by this account" >&2
    exit 1
}

echo "physical-controller-probe: physical device discovered at $EVENT_PATH"
exec "$HELPER" --vendor "$VENDOR" --product "$PRODUCT" --event-path "$EVENT_PATH" --window "$WINDOW"
