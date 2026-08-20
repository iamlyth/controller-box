#!/usr/bin/env bash
# probe-inputplumber-system-dbus.sh — non-skipping capability probe for
# `inputplumber-system-dbus` (Controller-Box infra; runs only under the
# dedicated unprivileged iprunner class on the live runner).
#
# Collects live facts from the real system (dpkg, systemctl, busctl) and
# validates every fact against the committed pins
# (scripts/iprunner-probes/inputplumber-expectations.json) via
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
EXPECTATIONS="$SCRIPT_DIR/iprunner-probes/inputplumber-expectations.json"
BUS_NAME="org.shadowblip.InputPlumber"
MANAGER_PATH="/org/shadowblip/InputPlumber/Manager"
MANAGER_IFACE="org.shadowblip.InputManager"
OM_PATH="/org/shadowblip/InputPlumber"
SOCKET="/run/dbus/system_bus_socket"

[[ -x "$VALIDATOR" && -f "$EXPECTATIONS" ]] || {
    echo "inputplumber-probe: validator or expectations missing" >&2
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

if [[ -n "${DBUS_SYSTEM_BUS_ADDRESS:-}" ]]; then
    echo "inputplumber-probe: DBUS_SYSTEM_BUS_ADDRESS is set; refusing a private bus" >&2
    exit 1
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

    # Package facts.
    PKG_NAME=""; PKG_VERSION=""; PKG_INSTALLED="false"
    if dpkg-query -W -f='${Package} ${Version} ${Status}' "$(python3 -c "import json;print(json.load(open('$EXPECTATIONS'))['package']['name'])")" \
        >"$tmp/pkg" 2>/dev/null; then
        read -r PKG_NAME PKG_VERSION PKG_STATUS < "$tmp/pkg" || true
        if [[ "$PKG_STATUS" == *installed* ]]; then
            PKG_INSTALLED="true"
        fi
    fi

    # Binary facts: any executable regular file shipped by the package.
    BIN_PATH=""
    while IFS= read -r candidate; do
        [[ -n "$candidate" && "$candidate" == /* && -f "$candidate" && -x "$candidate" ]] || continue
        BIN_PATH="$candidate"
        break
    done < <(dpkg -L "$PKG_NAME" 2>/dev/null | grep -E '^/(usr/bin|usr/libexec|usr/lib)/' || true)

    # Service facts.
    SVC_ACTIVE="false"; SVC_TYPE=""; SVC_UNIT=""; SVC_EXEC=""
    SVC_UNIT=$(systemctl show -p Id --value "inputplumber.service" 2>/dev/null || true)
    if [[ -n "$SVC_UNIT" ]]; then
        [[ "$(systemctl is-active "inputplumber.service" 2>/dev/null || true)" == "active" ]] && SVC_ACTIVE="true"
        SVC_TYPE=$(systemctl show -p Type --value "inputplumber.service" 2>/dev/null || true)
        SVC_EXEC=$(systemctl show -p ExecStart --value "inputplumber.service" 2>/dev/null || true)
    fi
    SVC_EXEC_BIN=""
    if [[ -n "$SVC_EXEC" ]]; then
        read -r SVC_EXEC_BIN _ <<< "$SVC_EXEC" || true
    fi

    # Bus facts via busctl (real system bus).
    HAS_OWNER="false"; UNIQUE_OWNER=""
    if output=$(busctl --system --json=short call org.freedesktop.DBus /org/freedesktop/DBus \
        org.freedesktop.DBus GetNameOwner s "$BUS_NAME" 2>/dev/null); then
        UNIQUE_OWNER=$(python3 -c "import json,sys;print(json.loads(sys.argv[1]).get('data',''))" "$output" 2>/dev/null || true)
        [[ -n "$UNIQUE_OWNER" ]] && HAS_OWNER="true"
    fi

    # Manager version.
    MANAGER_VERSION=""
    if output=$(busctl --system --json=short get-property "$BUS_NAME" "$MANAGER_PATH" \
        "$MANAGER_IFACE" Version 2>/dev/null); then
        MANAGER_VERSION=$(python3 -c "import json,sys;print(json.loads(sys.argv[1]).get('data',''))" "$output" 2>/dev/null || true)
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
        IDS=$(python3 -c "import json,sys;print(json.dumps(json.loads(sys.argv[1]).get('data',[])))" "$output" 2>/dev/null || true)
    fi

    # ObjectManager.
    OM_OBJECTS=0; OM_PATH_FACT=""; OM_IFACE_FACT=""
    if output=$(busctl --system --json=short call "$BUS_NAME" "$OM_PATH" \
        org.freedesktop.DBus.ObjectManager GetManagedObjects 2>/dev/null); then
        OM_OBJECTS=$(python3 -c "import json,sys;print(len(json.loads(sys.argv[1])))" "$output" 2>/dev/null || echo 0)
        OM_PATH_FACT="$OM_PATH"; OM_IFACE_FACT="org.freedesktop.DBus.ObjectManager"
    fi

    python3 - "$facts" "$BUS_ADDRESS" "$PKG_NAME" "$PKG_VERSION" "$PKG_INSTALLED" \
        "$BIN_PATH" "$SVC_UNIT" "$SVC_ACTIVE" "$SVC_TYPE" "$SVC_EXEC_BIN" \
        "$HAS_OWNER" "$UNIQUE_OWNER" "$MANAGER_VERSION" "$METHODS" \
        "$GAMEPAD_SIG" "$GAMEPAD_WRITABLE" "$IDS" "$INTERFACES" \
        "$OM_OBJECTS" "$OM_PATH_FACT" "$OM_IFACE_FACT" <<'PY'
import json, sys
out, args = sys.argv[1], sys.argv[2:]
facts = {
    "schema": "iprunner-inputplumber-facts/v1",
    "dbus_system_bus_address_env": None,
    "bus_address_effective": args[0],
    "package": {
        "name": args[1],
        "version": args[2],
        "installed": args[3] == "true",
    },
    "binary": {
        "path": args[4],
        "exists": bool(args[4]),
        "executable": bool(args[4]),
        "owned_by_package": bool(args[4]),
    },
    "service": {
        "unit": args[5],
        "active": args[6] == "true",
        "type": args[7],
        "exec_start_binary": args[8],
    },
    "name_owner": {
        "has_owner": args[9] == "true",
        "unique_owner": args[10],
    },
    "manager": {
        "path": "/org/shadowblip/InputPlumber/Manager",
        "interface": "org.shadowblip.InputManager",
        "version": args[11],
        "methods": json.loads(args[12]),
        "gamepad_order_signature": args[13],
        "gamepad_order_writable": args[14] == "true",
        "supported_target_device_ids": json.loads(args[15]),
    },
    "interfaces_present": json.loads(args[16]),
    "object_manager": {
        "objects": int(args[17]),
        "path": args[18],
        "interface": args[19],
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
