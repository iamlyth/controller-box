# Task 11 Complete — Overlay production-dispatch interaction tests

## What was done

### Production changes

1. **Exposed three static callbacks** in `overlay_service.h` by removing
   `static` and renaming with `cbx_overlay_` prefix:
   - `on_overlay_save` → `cbx_overlay_on_save` — lifecycle on_save:
     conflict detection → resolution → assignment sync → save
   - `on_slot_change` → `cbx_overlay_on_slot_change` — player/host mode
     slot change: marks surface dirty
   - `on_profile_change` → `cbx_overlay_on_profile_change` — player mode
     profile change: calls `ip_composite_load_profile_path` via DBus,
     marks surface dirty

2. **Updated all references** in `overlay_service.c`:
   - `on_host_slot_change` now calls `cbx_overlay_on_slot_change`
   - `run_overlay_service()` wires `cbx_overlay_on_slot_change`,
     `cbx_overlay_on_profile_change`, `cbx_overlay_on_save`

### Test file: `tests/test_overlay_interaction.c` — 19 sub-tests

Fixture (`interaction_fixture`): 2-composite grid with mock DBus,
`ip_input_events` subscribed with device path mappings, `ip_intercept_poll`
initialized, production callbacks wired, instant lifecycle transitions
(fade=0). Lifecycle starts in IDLE; `make_visible()` helper for most tests.

Tests by overlay action:
- **O01** (2 tests): Activate via InterceptMode poll (push poll event →
  mock returns "2" → lifecycle IDLE→VISIBLE); deactivate via poll (mock
  returns "1" → lifecycle VISIBLE→IDLE)
- **O02** (1 test): Move left — right first then left, verify col 1→0
- **O03** (1 test): Move right — verify col 0→1
- **O04** (1 test): Cycle profile up — verify profile changed from "Default"
- **O05** (1 test): Cycle profile down — verify profile changed
- **O06** (2 tests): Enter host mode via R3; verify host_row=0; verify
  non-host controller frozen (DBus input → no movement, CBX_ROW_FROZEN)
- **O07** (1 test): Host navigate rows — DOWN→row 1, UP→row 0
- **O08** (1 test): Host move slot — RIGHT→col 1, LEFT→col 0
- **O09** (1 test): Exit host mode via R3 — verify inactive
- **O10** (3 tests): Close via keyboard B — on_save fires, assignments
  synced (slot 0, id "TEST:0"); close via DBus B — same verification;
  conflict resolution — row 0→P1, row 1→P1 creates conflict, on_save
  auto-resolves to row 0→P1, row 1→P2
- **O11** (4 tests): Multi-controller independence via DBus InputEvent
  (row 0 and row 1 move independently); host mode via DBus (R3 enter,
  frozen controller, navigate, move slot, R3 exit); unknown device
  path dropped; wrong sender rejected
- **O12** (1 test): Host profile cycle deferred per §13 — verified
  current behavior: UP/DOWN navigates rows, not cycles profiles

### Key implementation insights

- `cbx_overlay_on_profile_change` uses `svc->conn.backend` and
  `svc->conn.bus` for the `ip_composite_load_profile_path` DBus call.
  The fixture sets up `svc->conn` via `ip_connection_init` +
  `ip_connection_set_bus` with the mock backend/bus.
- The mock's `get_property` and `set_property` share the same
  expectation lookup by `(iface, prop)`. Value is used by get, ignored
  by set. One expectation `("InterceptMode", "2")` serves both O01
  (get returns "2" = ALL) and O10 (set returns rc=0 = success).
- `ip_input_events_process()` is a no-op on the mock backend.
  `inject_signal()` directly calls the subscription callback
  (`cbx_overlay_input_cb`) synchronously, which is the production
  dispatch path for InputEvent signals.
- `cbx_resolve_user_profiles_dir` just constructs the path (doesn't
  check existence); works in test env with HOME set.
- Lifecycle `fade_in_ms=0` and `fade_out_ms=0` give instant transitions
  for deterministic testing.
- REQ-010/O12: host-mode profile cycling is NOT a blocking gap. It's
  deferred per §13 (interface details). The final audit (Task 14) will
  make the final determination.

### Test results
78/78 pass (1 skip: backend_smoke). No regressions. 19 new sub-tests.

### Commits
- `0a2f6e9` on `develop` (production + test changes)
- `d7f8e7b` on `develop` (plan update)

## Next task
Task 12: Enhance installed smoke test with coordinate-based mouse clicks.
Dependencies: Task 1, Task 2, Task 3 (all complete). Ready to start.