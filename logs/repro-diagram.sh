#!/usr/bin/env bash
# Reproduce BUG-0014: blank controller diagram in production manager launch.
set -euo pipefail
cd /workspace/project

TMPH=$(mktemp -d /tmp/cbx_repro_XXXX)
XVFB_DISPLAY=":97"
cleanup() {
  [ -n "${MGRPID:-}" ] && kill -9 "$MGRPID" 2>/dev/null || true
  [ -n "${XVPID:-}" ] && kill -9 "$XVPID" 2>/dev/null || true
  rm -rf "$TMPH"
}
trap cleanup EXIT

# A test profile to edit
mkdir -p "$TMPH/.local/share/inputplumber/profiles"
cat > "$TMPH/.local/share/inputplumber/profiles/testprof.yaml" <<'EOF'
version: 1
kind: DeviceProfile
name: TestProfile
description: NES test profile
mapping:
  - name: btn_A
    source_event:
      gamepad:
        button: A
    target_events:
      - keyboard: KeyA
  - name: btn_B
    source_event:
      gamepad:
        button: B
    target_events:
      - keyboard: KeyB
EOF

Xvfb "$XVFB_DISPLAY" -screen 0 1280x720x24 >/dev/null 2>&1 &
XVPID=$!
sleep 1

HOME="$TMPH" XDG_CONFIG_HOME="$TMPH/.config" XDG_DATA_HOME="$TMPH/.local/share" \
  DISPLAY="$XVFB_DISPLAY" ./build-check/controller-box --manager >"$TMPH/mgr.log" 2>&1 &
MGRPID=$!
sleep 2

# The manager window. Find its id.
WINID=$(DISPLAY="$XVFB_DISPLAY" xdotool search --sync --name "Controller-Box Manager" | head -1)
echo "WINID=$WINID"
DISPLAY="$XVFB_DISPLAY" xdotool windowactivate "$WINID" 2>/dev/null || true
DISPLAY="$XVFB_DISPLAY" xdotool windowfocus "$WINID" 2>/dev/null || true

# Navigate: RIGHT to Profiles tab
DISPLAY="$XVFB_DISPLAY" xdotool key --window "$WINID" Right
sleep 0.4
DISPLAY="$XVFB_DISPLAY" xdotool key --window "$WINID" Down
sleep 0.3
# Click Edit button (create=16..196, edit=212..392, y 500..544)
DISPLAY="$XVFB_DISPLAY" xdotool mousemove --window "$WINID" 300 522
DISPLAY="$XVFB_DISPLAY" xdotool click 1
sleep 0.6

DISPLAY="$XVFB_DISPLAY" import -window "$WINID" "$TMPH/editor.png" 2>/dev/null || true
DISPLAY="$XVFB_DISPLAY" xdotool getwindowgeometry --shell "$WINID" 2>/dev/null || true

echo "=== screenshot: $TMPH/editor.png ==="
echo "=== manager log ==="
cat "$TMPH/mgr.log"
