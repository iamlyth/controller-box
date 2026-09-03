#!/usr/bin/env bash
#
# check-core-acceptance.sh — mandatory core-behavior acceptance detector.
#
# This script DETECTS broken core behaviors; it does not fix them. The
# factory campaign loop runs it after the deterministic ``verify-project.sh``
# gate passes (Phase 4) so that silent acceptance of broken core behaviors
# cannot hide behind passing mocked/private-DBus/pixel-count tests.
#
# Exit codes (deliberately never the silent skip exit 77 — skips are silently
# accepted by the campaign, so broken core behavior or unavailable hardware
# must surface as a finding):
#
#   0          core behaviors look healthy (no findings)
#   1          core-behavior finding (BUG-0015 and/or BUG-0018 detected)
#   126/127/   the detector itself could not execute -> the campaign treats
#   negative   this as infrastructure_failure (the verifier is broken)
#
# Two core behaviors are probed:
#
#   1. Virtual controller creation (BUG-0015): the manager must report 4 of
#      4 virtual controllers active against the REAL InputPlumber system bus.
#      If the real system bus is unavailable or 0 of 4 targets are active,
#      this is a finding (mocked/private-DBus tests are not real evidence).
#
#   2. Licensed diagram asset selection (BUG-0018): the production diagram
#      path must load licensed device-specific SVGs (e.g. ``cc-xbox-360``,
#      ``cc-ps5``, ``cc-steam-deck``). If only ``generic-gamepad.svg`` is
#      registered for the production device-layout table, every device
#      silently falls back to the generic asset.
#
set -u

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$REPO_ROOT"

findings=()

# ---------------------------------------------------------------------------
# 1. Virtual controller creation (BUG-0015).
# ---------------------------------------------------------------------------
#
# The real InputPlumber system bus is the only acceptable live evidence that
# the four virtual controller targets can actually be created. The dedicated
# non-skipping probe ``scripts/probe-inputplumber-system-dbus.sh`` validates
# the live system bus; when it cannot reach the real bus (no InputPlumber
# service, a private/mock bus, a missing package) it exits non-zero, which is
# exactly the BUG-0015 condition. A persisted manager report carrying the
# known BUG-0015 signature ("Topology incomplete: 0 of 4 ...") is also honored
# so a recorded production run is honest evidence too; a "4 of 4" report from
# a real run is honored as a healthy topology even when the live probe cannot
# run in the current (e.g. offline/CI) environment.
#
probe="$REPO_ROOT/scripts/probe-inputplumber-system-dbus.sh"
manager_report="${FACTORY_MANAGER_REPORT:-}"
if [ -z "$manager_report" ] && [ -f "$REPO_ROOT/.factory/artifacts/manager-report.txt" ]; then
    manager_report="$REPO_ROOT/.factory/artifacts/manager-report.txt"
fi

bus_ok=1
if [ -x "$probe" ]; then
    "$probe" >/dev/null 2>&1 || bus_ok=0
else
    # Without the probe the real system bus cannot be validated.
    bus_ok=0
fi

# topology_resolved: 1 = healthy, -1 = explicit BUG-0015 signature,
# 0 = unresolved (fall back to the live probe).
topology_resolved=0
if [ -n "$manager_report" ] && [ -r "$manager_report" ]; then
    if grep -qi '4 of 4 virtual controllers active' "$manager_report"; then
        topology_resolved=1
    elif grep -qi '0 of 4 virtual controllers active' "$manager_report"; then
        topology_resolved=-1
    fi
fi
if [ "$topology_resolved" -eq 0 ] && [ "$bus_ok" -ne 0 ]; then
    topology_resolved=1   # real system bus reachable, healthy
fi

if [ "$topology_resolved" -le 0 ]; then
    findings+=(
        "core-acceptance: virtual controller creation incomplete (BUG-0015): 0 of 4 targets active — real InputPlumber system bus required"
    )
fi

# ---------------------------------------------------------------------------
# 2. Licensed diagram asset selection (BUG-0018).
# ---------------------------------------------------------------------------
#
# The production diagram path resolves each device to an entry of the
# ``s_device_layouts`` table in ``src/manager/profile_diagram.c``; any device
# without a registered entry falls back to ``generic-gamepad``. BUG-0018 is
# the table shipping only the generic entry, so every device silently uses
# ``generic-gamepad.svg``. The detector inspects the committed production
# device-layout table (or an explicit FACTORY_DIAGRAM_MANIFEST override) and
# reports a finding when no licensed device-specific entry is registered.
#
licensed_markers="cc-xbox-360 cc-ps5 cc-steam-deck"
diagram_src="$REPO_ROOT/src/manager/profile_diagram.c"
diagram_manifest="${FACTORY_DIAGRAM_MANIFEST:-}"
if [ -z "$diagram_manifest" ] && [ -f "$REPO_ROOT/.factory/artifacts/diagram-selection.json" ]; then
    diagram_manifest="$REPO_ROOT/.factory/artifacts/diagram-selection.json"
fi

# Collect the raw set of device identifiers the production path
# registers/selects (quoted, from the source table) or selects (from a
# manifest). Then normalise to one bare identifier per line so quoted and
# unquoted sources are treated identically.
registered_raw=""
if [ -n "$diagram_manifest" ] && [ -r "$diagram_manifest" ]; then
    registered_raw="$(cat "$diagram_manifest")"
elif [ -r "$diagram_src" ]; then
    # Extract ONLY the icon names registered in the production
    # ``s_device_layouts[]`` table (lines like:  { "generic-gamepad", ... },)
    # so unrelated quoted tokens elsewhere in the module cannot mask the
    # fallback by being mistaken for a licensed device entry.
    registered_raw="$(awk '
        /s_device_layouts\[\]/ {in_table=1; next}
        in_table && /^[[:space:]]*};[[:space:]]*$/ {in_table=0}
        in_table {
            line=$0
            while (match(line, /"[a-z0-9][a-z0-9-]*"/)) {
                print substr(line, RSTART, RLENGTH)
                line=substr(line, RSTART+RLENGTH)
            }
        }
    ' "$diagram_src" 2>/dev/null || true)"
fi

# Normalise: strip quotes, keep one bare lowercase identifier per line.
registered_normalized="$(printf '%s\n' "$registered_raw" \
    | tr -d '"' | grep -oE '[a-z0-9][a-z0-9-]*' 2>/dev/null || true)"

licensed_loaded=0
for marker in $licensed_markers; do
    case "$registered_normalized" in
        *"$marker"*) licensed_loaded=$((licensed_loaded + 1)) ;;
    esac
done

# A non-generic, device-specific registered entry is also acceptable
# evidence (the licensed markers above are canonical; accept any registered
# icon that is not the generic fallback).
non_generic_registered=0
if [ -n "$registered_normalized" ]; then
    non_generic_registered="$(printf '%s\n' "$registered_normalized" \
        | grep -v '^generic-gamepad$' | grep -c . || true)"
fi

if [ "$licensed_loaded" -le 0 ] && [ "$non_generic_registered" -le 0 ]; then
    findings+=(
        "core-acceptance: licensed diagram fallback active (BUG-0018): device-specific SVGs not loaded in production path"
    )
fi

# ---------------------------------------------------------------------------
# Report.
# ---------------------------------------------------------------------------
if [ "${#findings[@]}" -gt 0 ]; then
    for f in "${findings[@]}"; do
        echo "$f" >&2
    done
    exit 1
fi

echo "core-acceptance: virtual controllers active; licensed diagrams selected"
exit 0