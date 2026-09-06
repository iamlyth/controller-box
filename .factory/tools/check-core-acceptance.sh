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
#   2. Licensed diagram selection (BUG-0018) requires signed exact-HEAD
#      installed-licensed-diagram AND gpu-compositor evidence. Local pixels,
#      broad silhouettes, source declarations and prose are never evidence.
#      Human golden approval remains a separate policy input if a protected
#      baseline changes; this checker never manufactures that approval.
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
capability_checker="$REPO_ROOT/.factory/tools/check-capability-evidence.py"
if [ ! -f "$capability_checker" ] || [ -L "$capability_checker" ]; then
    echo "core-acceptance: canonical capability evidence checker is unavailable" >&2
    exit 126
fi
if ! python3 "$capability_checker" \
    --root "$REPO_ROOT" \
    --capabilities controller-production-routing,gpu-compositor,installed-licensed-diagram >/dev/null
then
    findings+=(
        "core-acceptance: all-four routing and/or installed licensed GPU diagram lacks accepted signed exact-HEAD non-skipped evidence (BUG-0015/BUG-0018)"
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

echo "core-acceptance: signed exact-HEAD all-four routing and installed licensed accelerated diagram evidence accepted"
exit 0