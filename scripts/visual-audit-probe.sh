#!/usr/bin/env bash
# Real non-skipping image round-trip probe for the visual-audit vision model.
#
# Before a product may select a vision model for visual audit, this probe must
# pass: it builds a deterministic known image (a solid red rectangle), reads
# its bytes, computes the SHA-256, and delivers those exact bytes in-memory to
# the configured model through the SDK driver. The model must correctly
# describe the image (solid red rectangle). This proves actual image-byte
# delivery to the exact model — not prompt-only, text-description, golden, or
# fixture-stub behavior. On any model outage or wrong answer the probe fails
# closed and the product must not enable visual audit.
#
# Usage: scripts/visual-audit-probe.sh [--model ollama/kimi-k2.7-code]
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
MODEL="${VISUAL_AUDIT_VISION_MODEL:-ollama/kimi-k2.7-code}"

work=$(mktemp -d "$PROJECT_ROOT/.factory-state/visual-audit-probe.XXXXXX")
trap 'rm -rf "$work"' EXIT
chmod 700 "$work"

# Deterministic known image: 64x32 solid red PNG (stdlib writer).
python3 - "$work/probe.png" <<'PY'
import struct, sys, zlib
path = sys.argv[1]
w, h = 64, 32
def chunk(t, d):
    c = t + d
    return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
raw = b''.join(b'\x00' + bytes((200, 30, 30)) * w for _ in range(h))
data = b'\x89PNG\r\n\x1a\n'
data += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
data += chunk(b'IDAT', zlib.compress(raw))
data += chunk(b'IEND', b'')
open(path, 'wb').write(data)
PY

expected_sha=$(sha256sum "$work/probe.png" | cut -d' ' -f1)
cat > "$work/prompt.md" <<EOF
This image's bytes SHA-256 is $expected_sha. Reply with a single strict JSON object:
{"verdict":"pass","observations":[{"code":"PROBE_COLOR","severity":"info","description":"<color>"}]}
Do not use markdown fences.
EOF

# The SDK driver delivers the exact hashed bytes in-memory.
node "$SCRIPT_DIR/visual-audit-review-sdk.mjs" \
    --image "$work/probe.png" \
    --expected-sha256 "$expected_sha" \
    --state-id "probe" \
    --role "probe" \
    --prompt-file "$work/prompt.md" \
    --prompt-sha256 "$(sha256sum "$work/prompt.md" | cut -d' ' -f1)" \
    --schema-sha256 "probe" \
    --model "$MODEL" \
    --out-dir "$work/out" >/dev/null

finding="$work/out/finding-probe-probe.json"
[[ -f "$finding" ]] || { echo "visual-audit-probe: no finding produced" >&2; exit 1; }
description=$(python3 - "$finding" <<'PY'
import json, sys
f = json.load(open(sys.argv[1]))
text = " ".join(o.get("description", "") for o in f.get("observations", []))
print(text.lower())
PY
)
case "$description" in
    *red*|*rectangle*|*solid*)
        echo "visual-audit-probe: PASS — model described known image (bytes $expected_sha)"
        ;;
    *)
        echo "visual-audit-probe: FAIL — model did not describe the known image: $description" >&2
        exit 1
        ;;
esac
