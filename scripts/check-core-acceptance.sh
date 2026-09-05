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
# Two independent core behaviors are checked:
#
#   1. Virtual controller creation/routing (BUG-0015) requires the canonical
#      strong capability checker to accept signed, exact-HEAD, non-skipped
#      ``controller-production-routing`` evidence.  A reachable system bus or
#      manager prose is never routing evidence.
#
#   2. Licensed diagram asset selection (BUG-0018): the production diagram
#      path must load licensed device-specific SVGs (e.g. ``cc-xbox-360``,
#      ``cc-ps5``, ``cc-steam-deck``). If only ``generic-gamepad.svg`` is
#      registered for the production device-layout table, every device
#      silently falls back to the generic asset.
#
set -u

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$REPO_ROOT" || exit 1

findings=()

# ---------------------------------------------------------------------------
# 1. Virtual controller production routing (BUG-0015).
# ---------------------------------------------------------------------------
#
# Delegate the entire trust decision to the canonical strong checker.  It
# validates declaration/contract, the signed aggregate and detached
# signatures, exact HEAD/tree/environment/log bindings, must-execute marker,
# and skip/simulation denial for this specific capability.
capability_checker="$REPO_ROOT/scripts/check-capability-evidence.py"
if [ ! -f "$capability_checker" ] || [ -L "$capability_checker" ]; then
    echo "core-acceptance: canonical capability evidence checker is unavailable" >&2
    exit 126
fi
if ! python3 "$capability_checker" \
    --root "$REPO_ROOT" \
    --capabilities controller-production-routing >/dev/null
then
    findings+=(
        "core-acceptance: controller-production-routing lacks accepted signed exact-HEAD non-skipped capability evidence (BUG-0015)"
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

echo "core-acceptance: production routing evidenced; licensed diagrams selected"
exit 0