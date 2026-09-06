#!/usr/bin/env bash
# probe-inputplumber-system-dbus.sh — non-skipping capability probe for
# `inputplumber-system-dbus` (Controller-Box infra; runs only under the
# dedicated unprivileged iprunner class on the live runner).
#
# Collects live facts from the real system (dpkg, systemctl, busctl) and
# validates every fact against the committed pins
# (.factory/runner/iprunner-probes/inputplumber-expectations.json) via
# validate-inputplumber-facts.py. Any mismatch fails the probe with a precise
# diagnostic; there is no skip path.
#
# The probe refuses a non-default system bus (DBUS_SYSTEM_BUS_ADDRESS set, a
# private bus, or a bind-mounted substitute) so a private/mock service can
# never be passed off as the real InputPlumber system-bus service.
#
# Usage:
#   probe-inputplumber-system-dbus.sh [--fixture-facts FILE]
#
#   --fixture-facts FILE  adversarial-test only: validate a collected-facts
#                         fixture instead of collecting live facts. The
#                         committed contract probe_argv never passes this
#                         flag, so production runs always collect live facts.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
VALIDATOR="$SCRIPT_DIR/iprunner-probes/validate-inputplumber-facts.py"
DECODER="$SCRIPT_DIR/iprunner-probes/unwrap_variant.py"
EXPECTATIONS="$SCRIPT_DIR/iprunner-probes/inputplumber-expectations.json"
BUS_NAME="org.shadowblip.InputPlumber"
MANAGER_PATH="/org/shadowblip/InputPlumber/Manager"
MANAGER_IFACE="org.shadowblip.InputManager"
OM_PATH="/org/shadowblip/InputPlumber"
SOCKET="/run/factory/dbus/system_bus_socket"
EXPECTED_BUS="unix:path=$SOCKET"

[[ -x "$VALIDATOR" && -x "$DECODER" && -f "$EXPECTATIONS" ]] || {
    echo "inputplumber-probe: validator, decoder, or expectations missing" >&2
    exit 1
}

FIXTURE_FACTS=""
if [[ $# -eq 2 && "$1" == "--fixture-facts" ]]; then
    FIXTURE_FACTS=$2
elif [[ $# -ne 0 ]]; then
    echo "inputplumber-probe: usage: $0 [--fixture-facts FILE]" >&2
    exit 2
fi

if [[ $(id -u) -eq 0 ]]; then
    echo "inputplumber-probe: must not run as root" >&2
    exit 1
fi

echo "--- inputplumber-system-dbus capability contract ---"
if [[ -z "$FIXTURE_FACTS" ]]; then
    [[ "${DBUS_SYSTEM_BUS_ADDRESS:-}" == "$EXPECTED_BUS" ]] || {
        echo "inputplumber-probe: exact broker D-Bus proxy is required" >&2; exit 1;
    }
    [[ "${FACTORY_INPUTPLUMBER_PROVENANCE:-}" == "/run/factory/inputplumber-provenance.json" ]] || {
        echo "inputplumber-probe: broker-held provenance is required" >&2; exit 1;
    }
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

collect_facts() {
    local facts="$1"
    # Effective system bus address: the real system socket must be a socket.
    if [[ -S "$SOCKET" ]]; then
        BUS_ADDRESS="unix:path=$SOCKET"
    else
        BUS_ADDRESS=""
    fi

    # Package/service/executable provenance is gathered and held by the root
    # broker outside PrivatePIDs.  The candidate never invokes host systemctl
    # or accepts package facts from its own namespace.
    readarray -t HELD < <(python3 - "$FACTORY_INPUTPLUMBER_PROVENANCE" <<'PY'
import json,sys
p=json.load(open(sys.argv[1],encoding='utf-8'))
assert p.get('schema')=='factory-host-inputplumber-provenance/v2'
for k in ('package_name','package_version','service_unit','service_type','exe'):
 print(p[k])
for k in ('package_installed','service_active','exe_owned_by_package'):
 print('true' if p[k] is True else 'false')
PY
)
    [[ ${#HELD[@]} -eq 8 ]] || { echo "inputplumber-probe: held provenance malformed" >&2; return 1; }
    PKG_NAME=${HELD[0]}; PKG_VERSION=${HELD[1]}; SVC_UNIT=${HELD[2]}; SVC_TYPE=${HELD[3]}; BIN_PATH=${HELD[4]}
    PKG_INSTALLED=${HELD[5]}; SVC_ACTIVE=${HELD[6]}; BIN_OWNED=${HELD[7]}
    SVC_EXEC_BIN=$BIN_PATH; BIN_EXISTS="false"; BIN_EXECUTABLE="false"
    [[ -f "$BIN_PATH" && ! -L "$BIN_PATH" ]] && BIN_EXISTS="true"
    [[ -x "$BIN_PATH" ]] && BIN_EXECUTABLE="true"

    # Bus facts via busctl (real system bus).
    HAS_OWNER="false"; UNIQUE_OWNER=""
    if output=$(busctl --system --json=short call org.freedesktop.DBus /org/freedesktop/DBus \
        org.freedesktop.DBus GetNameOwner s "$BUS_NAME" 2>/dev/null); then
        UNIQUE_OWNER=$(printf '%s\n' "$output" | python3 "$DECODER" --string-method 2>/dev/null | python3 -c 'import json,sys; v=json.load(sys.stdin); print(v if isinstance(v,str) else "")' 2>/dev/null || true)
        [[ "$UNIQUE_OWNER" =~ ^:[0-9]+\.[0-9]+$ ]] && HAS_OWNER="true"
    fi

    # Manager version.
    MANAGER_VERSION=""
    if output=$(busctl --system --json=short get-property "$BUS_NAME" "$MANAGER_PATH" \
        "$MANAGER_IFACE" Version 2>/dev/null); then
        MANAGER_VERSION=$(printf '%s\n' "$output" | python3 "$DECODER" --property-s 2>/dev/null | python3 -c 'import json,sys; v=json.load(sys.stdin); print(v if isinstance(v,str) else "")' 2>/dev/null || true)
    fi

    # Manager introspection: methods, GamepadOrder flags, interface names.
    METHODS='{}'
    GAMEPAD_WRITABLE="false"; GAMEPAD_SIG=""; IDS='[]'
    INTERFACES='[]'
    if busctl --system introspect "$BUS_NAME" "$MANAGER_PATH" >"$tmp/introspect" 2>/dev/null; then
        METHODS=$(python3 - "$tmp/introspect" <<'PY'
import json, re, sys
methods = {}
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    match = re.match(r"^\.([A-Za-z0-9_]+)\s+method\s+(\S+)\s+(\S+)\s+", line)
    if match:
        methods[match.group(1)] = {"in": match.group(2), "out": match.group(3)}
print(json.dumps(methods, sort_keys=True))
PY
        )
        read -r GAMEPAD_SIG GAMEPAD_WRITABLE < <(python3 - "$tmp/introspect" <<'PY'
import re, sys
sig, writable = "", "false"
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    match = re.match(r"^\.GamepadOrder\s+property\s+(\S+)\s+", line)
    if match:
        sig = match.group(1)
        writable = "true" if "writable" in line else "false"
        break
print(sig, writable)
PY
        )
        INTERFACES=$(python3 - "$tmp/introspect" <<'PY'
import json, re, sys
names = []
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    match = re.match(r"^([A-Za-z0-9_.]+)\s+interface\s+", line)
    if match:
        names.append(match.group(1))
print(json.dumps(names, sort_keys=True))
PY
        )
    fi

    # SupportedTargetDeviceIds.
    if output=$(busctl --system --json=short get-property "$BUS_NAME" "$MANAGER_PATH" \
        "$MANAGER_IFACE" SupportedTargetDeviceIds 2>/dev/null); then
        IDS=$(printf '%s\n' "$output" | python3 "$DECODER" --property-as 2>/dev/null || true)
    fi

    # ObjectManager.
    OM_OBJECTS=0; OM_PATH_FACT=""; OM_IFACE_FACT=""
    if output=$(busctl --system --json=short call "$BUS_NAME" "$OM_PATH" \
        org.freedesktop.DBus.ObjectManager GetManagedObjects 2>/dev/null); then
        if decoded=$(printf '%s\n' "$output" | python3 "$DECODER" --object-manager 2>/dev/null); then
            OM_OBJECTS=$(python3 -c 'import json,sys; print(len(json.loads(sys.argv[1])))' "$decoded")
            OM_PATH_FACT="$OM_PATH"; OM_IFACE_FACT="org.freedesktop.DBus.ObjectManager"
        fi
    fi

    python3 - "$facts" "$BUS_ADDRESS" "$PKG_NAME" "$PKG_VERSION" "$PKG_INSTALLED" \
        "$BIN_PATH" "$BIN_EXISTS" "$BIN_EXECUTABLE" "$BIN_OWNED" \
        "$SVC_UNIT" "$SVC_ACTIVE" "$SVC_TYPE" "$SVC_EXEC_BIN" "$HAS_OWNER" "$UNIQUE_OWNER" "$MANAGER_VERSION" "$METHODS" \
        "$GAMEPAD_SIG" "$GAMEPAD_WRITABLE" "$IDS" "$INTERFACES" \
        "$OM_OBJECTS" "$OM_PATH_FACT" "$OM_IFACE_FACT" <<'PY'
import json, sys
out, args = sys.argv[1], sys.argv[2:]
facts = {
    "schema": "iprunner-inputplumber-facts/v1",
    "dbus_system_bus_address_env": "unix:path=/run/factory/dbus/system_bus_socket",
    "bus_address_effective": args[0],
    "package": {
        "name": args[1],
        "version": args[2],
        "installed": args[3] == "true",
    },
    "binary": {
        "path": args[4],
        "exists": args[5] == "true",
        "executable": args[6] == "true",
        "owned_by_package": args[7] == "true",
    },
    "service": {
        "unit": args[8],
        "active": args[9] == "true",
        "type": args[10],
        "exec_start_binary": args[11],
    },
    "name_owner": {
        "has_owner": args[12] == "true",
        "unique_owner": args[13],
    },
    "manager": {
        "path": "/org/shadowblip/InputPlumber/Manager",
        "interface": "org.shadowblip.InputManager",
        "version": args[14],
        "methods": json.loads(args[15]),
        "gamepad_order_signature": args[16],
        "gamepad_order_writable": args[17] == "true",
        "supported_target_device_ids": json.loads(args[18]),
    },
    "interfaces_present": json.loads(args[19]),
    "object_manager": {
        "objects": int(args[20]),
        "path": args[21],
        "interface": args[22],
    },
}
with open(out, "w", encoding="utf-8") as stream:
    json.dump(facts, stream, indent=2)
    stream.write("\n")
PY
}

if [[ -n "$FIXTURE_FACTS" ]]; then
    [[ -f "$FIXTURE_FACTS" && ! -L "$FIXTURE_FACTS" ]] || {
        echo "inputplumber-probe: fixture facts file is missing or unsafe" >&2
        exit 1
    }
    python3 "$VALIDATOR" --facts "$FIXTURE_FACTS" --expectations "$EXPECTATIONS"
    exit 0
fi

collect_facts "$tmp/facts.json"
python3 "$VALIDATOR" --facts "$tmp/facts.json" --expectations "$EXPECTATIONS"
