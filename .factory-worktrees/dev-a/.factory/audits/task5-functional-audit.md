# Functional Audit — Task 5: Property signals update the correct displayed device

## Verification performed

- **Clean rebuild** (`rm -rf build-aud-t5`, Debug): **success**, no errors.
- **Full CTest**: **91/91 passed** (2 skips: `test_kernel_controller`, `test_backend_smoke` — both
  legitimate exit-77 capability skips, unrelated to Task 5).
- **Task verification regex** (`test_properties_changed|test_overlay_native|test_manager_native|test_native_dbus`): **5/5 passed**.
- Code review of the full Task 5 path: `src/dbus/ip_properties.c/.h` (validation/dispatch),
  `src/dbus/ip_device_model.c/.h` (per-device application), `src/dbus/dbus_client.c`
  (production sd-bus parse + idempotent re-subscribe), `src/manager/manager.c` +
  `src/manager/controllers_tab.c` (Manager consumer), `src/app/overlay_service.c` +
  `src/overlay/grid_render.c` (overlay consumer), `src/ui/widget_list.c` (list widget),
  `tests/test_properties_changed.c`, `tests/test_overlay_native.c`, `tests/test_manager_native.c`,
  `tests/native_ip_server.c`.

The core acceptance goal works: per-device dispatch is correctly isolated by exact object path
(`cbx_device_model_apply_property` rejects unknown paths; a Manager property cannot land on a
composite and vice versa), sender/interface/path-class validation is layered and correct,
invalidation is handled by exactly one bounded authoritative read, re-subscription is idempotent
with callback refresh (no stale callbacks, no slot leaks), and the native-bus two-composite tests
genuinely exercise the production sd-bus parse path. Findings below are defects in the
surrounding wiring, not in the per-device routing itself.

---

## Findings

### 1. BLOCKER — Every property change resets the Manager Controllers-tab selection and scroll, enabling wrong-device actions

**Files:**
- `src/manager/manager.c` — `cbx_manager_on_prop_change()` (lines ~141–158)
- `src/manager/controllers_tab.c` — `cbx_controllers_tab_refresh_labels()` (line ~534)
- `src/ui/widget_list.c` — `cbx_list_clear()` (sets `selected = -1`, `scroll_offset = 0`),
  `cbx_list_add_item()` (re-selects index 0 when `selected < 0`)

**Description:** `cbx_manager_on_prop_change` unconditionally calls
`cbx_controllers_tab_refresh_labels()` on *every* validated change. That function does
`cbx_list_clear()` (selection → -1, scroll → 0) and rebuilds items; `cbx_list_add_item()` then
re-selects **row 0**. Meanwhile `tab->selected_device` keeps its old value until the next
`cbx_controllers_tab_sync_selection()`, which the Remove and Change-Type buttons call before
acting.

Consequences:
- The user's highlighted device jumps to the first row and the scroll resets to the top on any
  PropertiesChanged — including changes the Manager itself causes: `cbx_controllers_tab_remove()`
  triggers backend detach/SetGamepadOrder mutations, InputPlumber emits TargetDevices/
  GamepadOrder PropertiesChanged back, and the selection resets mid-flow. Overlay saves and any
  other client's routing changes do the same while the Manager is open.
- **Wrong-device action:** user selects device 3 → a property signal lands → highlight silently
  moves to device 0 → user presses Remove → `sync_selection()` reads the widget (0) → **device 0
  is removed instead of device 3**.
- A subsequent UP keypress on the desynced list (`selected` already at boundary) escapes the
  focus chain instead of navigating.

The full re-enumeration path in the same file (line ~660–686) carefully preserves selection
(`selected_path` → restore → clamp → `cbx_list_set_selected`); the reactive path added by this
task skips all of that. No test covers selection preservation across a property change.

**Recommendation:** In `refresh_labels` (or a Task-5-specific wrapper), preserve selection the way
`cbx_controllers_tab_refresh` does: remember `tab->selected_device` / the selected target's path
before the rebuild, then restore + clamp and `cbx_list_set_selected` after. Additionally,
`cbx_manager_on_prop_change` should skip the label rebuild entirely for changes that cannot
affect labels (`GamepadOrder`, `SourceDevicePaths`), and only dirty what actually changed. Add a
regression test: select row N, inject a PropertiesChanged, assert the selection is still N and
that Remove still targets row N.

---

### 2. WARN — Overlay renders nothing from TargetDevices / SourceDevicePaths / GamepadOrder: those changes are model-only ("cache + dirty flag")

**Files:**
- `src/app/overlay_service.c` — `cbx_overlay_on_prop_change()` (only calls
  `overlay_apply_grid_profile()` for ProfileName/ProfilePath, then `mark_dirty_all`)
- `src/overlay/grid_render.c` — grid `cur_col` is built exclusively from the GUI's own
  saved `cbx_assignments` (line ~92–106); `src/app/overlay_service.c` never reads
  `svc->model.*.target_devices`, `source_device_paths`, or `model.gamepad_order` in any render path

**Description:** In the overlay, a routing change (TargetDevices), a source change, or a Manager
GamepadOrder change updates the per-device model and marks the surface dirty — but the re-render
derives the grid from `svc->assignments`, so **the presented frame does not change**. E.g. an
external client reroutes composite 0 to a different target slot: the overlay model knows, but the
displayed player column stays stale until the GUI's own save/restore cycle. The task acceptance
requires routing/source/order changes to affect "the actual per-device model **and rendered UI**
in Manager and overlay, not merely a global cache or dirty flag". (In the Manager, TargetDevices
does reach the rendered label — see finding 1 — but nothing anywhere renders `gamepad_order` or
`source_device_paths`.)

**Recommendation:** Either derive the grid row's `cur_col` (and the Manager's ordering display,
if any) from the reactive model on the dirty re-render, or record explicitly (plan/SPEC) that
displayed routing is GUI-assignment-owned and reconcile reactive TargetDevices into
`assignments`/grid state. If the latter is the intended design, narrow the acceptance wording and
add a test documenting the reconciliation boundary.

---

### 3. WARN — Validation/storage off-by-one: boundary-length strings are accepted then silently truncated

**Files:**
- `src/dbus/ip_properties.c` — `string_within_limit()` uses `strlen(value) <= max_len`
  (`IP_PROP_MAX_NAME_LEN` 256, `IP_PROP_MAX_PATH_LEN` 4096)
- `src/dbus/ip_device_model.c` — `set_prop_string()` stores via
  `snprintf(dst, dst_len, ...)` into `profile_name[256]` / `profile_path[4096]`
  (`CBX_MODEL_PROP_NAME_LEN` / `CBX_MODEL_PROP_PATH_LEN` equal the *validation* limits, not
  limits + 1)

**Description:** A ProfileName of exactly 256 bytes or a ProfilePath of exactly 4096 bytes passes
validation, but storage needs `len + 1` bytes for the NUL, so `snprintf` silently truncates the
last character. The value is then dispatched as "valid" yet displayed/stored wrong. The header
comment in `ip_device_model.h` claims "Bounds mirror the ip_properties validation limits" — they
are off by one. Existing tests only check over-limit rejection (512 / 5000 bytes), never the
exact boundary.

**Recommendation:** Either change validation to `strlen(value) < max_len`, or size the model
fields `IP_PROP_MAX_NAME_LEN + 1` / `IP_PROP_MAX_PATH_LEN + 1`. Add boundary tests (exactly 256 /
exactly 4096) asserting the stored value round-trips byte-for-byte.

---

### 4. WARN — Array validation allows payloads far larger than the model can store: silent mid-element truncation

**Files:**
- `src/dbus/ip_properties.c` — `validate_array()` caps only per-element length and element
  count (256 elements × up to 4096-byte elements for SourceDevicePaths ⇒ ~1 MB CSV accepted)
- `src/dbus/ip_device_model.h` — `gamepad_order` / `target_devices` / `source_device_paths`
  are `char[4096]` (`CBX_MODEL_PROP_ARRAY_LEN`)

**Description:** A validated array whose comma-joined form exceeds 4096 bytes is silently
truncated by `set_prop_string`, cutting the **last element mid-name**. This is realistic: 64
target/source devices with ~70-char DBus paths already exceed 4096. Downstream exact-element
matching (`composite_profile_for_target()` in `src/manager/controllers_tab.c` walks the CSV and
compares full elements) then fails to match the corrupted tail element — a routed profile
annotation silently disappears. The element count from the callback is also discarded
(`(void)count` in both consumers), so the model cannot detect the loss.
`test_handle_array_max_elems` only uses short names (~1.5 KB total), so the gap is untested.

**Recommendation:** Add a total-CSV-length bound to `dispatch_value`/`validate_array` (reject
when the joined value would not fit `CBX_MODEL_PROP_ARRAY_LEN`), or enlarge the array fields, and
store `count` alongside the CSV. Add a test with an over-4096-byte total that asserts the change
is cleanly rejected rather than half-stored.

---

### 5. INFO — CSV array representation is lossy for elements containing commas

**Files:** `src/dbus/dbus_client.c` (array join in `sd_properties_changed_callback`),
`src/manager/controllers_tab.c` (`composite_profile_for_target` split-and-match)

**Description:** Array properties are flattened to a comma-joined string; an element containing a
`,` would be misparsed by every consumer that splits on commas (and exact-match against a longer
name containing the split part). Real InputPlumber values (target names, `/dev/input/eventN`
paths) contain no commas, so this is a documented-representation risk rather than a live bug.

**Recommendation:** Note the constraint next to `ip_properties_changed_payload.value`, or add an
element-level `,` rejection in `validate_array` so malformed data is dropped loudly instead of
misinterpreted.

---

### 6. INFO — Manager refreshes the whole device list for changes that cannot affect it

**Files:** `src/manager/manager.c` — `cbx_manager_on_prop_change`

**Description:** `GamepadOrder` and `SourceDevicePaths` changes trigger a full
`cbx_list_clear` + rebuild even though neither is rendered by the Controllers tab. Beyond wasted
work, this multiplies the window for finding 1's selection reset.

**Recommendation:** Gate the label rebuild on properties the tab actually displays
(`ProfileName`, `ProfilePath`, `TargetDevices`).

---

## Summary

| # | Severity | Area | Issue |
|---|----------|------|-------|
| 1 | **BLOCKER** | Manager UI | Property change resets list selection/scroll → Remove/Change-Type can act on the wrong device |
| 2 | WARN | Overlay UI | TargetDevices/SourceDevicePaths/GamepadOrder changes are model-only in the overlay; rendered frame unchanged |
| 3 | WARN | Validation/storage | Off-by-one: exactly-256/4096-byte strings accepted then truncated by one char |
| 4 | WARN | Validation/storage | Arrays validated up to ~1 MB but stored in 4096-byte fields → silent mid-element truncation |
| 5 | INFO | Representation | CSV join/split is lossy if elements contain commas |
| 6 | INFO | Manager UI | Unnecessary full list rebuild for non-displayed properties |

Finding 1 must be fixed before Task 5 can be considered functionally complete; the remainder are
improvements that can be scheduled. Build and all 91 tests pass on a clean rebuild; the per-device
isolation, validation, invalidation, and subscription-replacement behavior at the heart of the
task is correct and well covered by native-bus tests.