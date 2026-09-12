## Subsystem Study Report: identify (`src/identify/`)

### Overview

The identify subsystem answers "which physical controller is this device, and
where is it mapped?" It is the identity + assignment layer (SPEC §6.2–6.3,
§10.3 gap #2). It converts a source device's hardware properties into a
stable, prefixed identity string, maps that identity to a slot/profile
assignment persisted in `assignments.yaml`, detects identity downgrades on
reconnect, and restores the InputPlumber GamepadOrder across daemon restarts.

Pure logic (identity, assign, downgrade) lives here; persistence and DBus calls
are delegated to `src/config/` and `src/dbus/`. Module is standalone with no
internal file I/O except through the config/dbus dependencies.

**Files (8):** `identity.{c,h}`, `assign.{c,h}`, `assign_persist.{c,h}`,
`identity_downgrade.{c,h}`, `gamepad_order_restore.{c,h}`

### File-by-file breakdown

#### 1. `identity.c` / `identity.h` — identity extraction (Task 25)

- **Purpose:** Extract the strongest available identity from a source device's
  properties, formatted with a type prefix.
- **Identity layers (numeric = weakness order):** `BT:xx:..` (1, MAC),
  `USB:SNxxxx` (2, serial), `USB:phys:x` (3, port path), `ORDER:n` (4, connection
  order). `CBX_IDENTITY_LAYER_NONE` = no identity.
- Key types in `identity.h`:
  - `cbx_identity_layer` enum, `cbx_source_iface` (EVDEV vs HIDRAW),
    `cbx_source_props` (iface, unique_id, phys_path, serial_number, id_bustype),
    `cbx_identity` (`id[CBX_IDENTITY_MAX_LEN=128]` + `layer`).
- Key functions:
  - `cbx_identity_extract(props, connection_order, out)` — the entry point.
    Resolution: BT bustype + MAC → layer 1; else serial (unique_id for evdev /
    serial_number for HIDRaw, with cross-fallback in `get_serial`) not a MAC →
    layer 2; else phys_path → layer 3; else connection_order ≥ 0 → layer 4;
    else `-ENOENT` / NONE.
  - `cbx_identity_parse_layer(id)` — re-derive layer from a prefixed string by
    parsing prefix + format (used by downgrade logic and persistence).
  - `cbx_identity_is_downgrade(old, new)` — true when new layer number > old.
  - `cbx_identity_is_mac_address(s)` — validates `xx:xx:xx:xx:xx:xx`.
  - `cbx_identity_parse_bustype(str)` — strtol decimal, range 0..0xFFFF.
  - `cbx_identity_init(ident)`.
- Internal helpers: `format_bt_mac`, `format_usb_serial`, `format_usb_phys`,
  `format_order`, `get_serial`, char validators.
- **Interfaces:** Purely a library consumed by `assign_persist_auto_assign`
  flow, downgrade module, and overlay/manager code (via parse_layer etc.). Bus
  constants `BUS_USB=0x03`, `BUS_BLUETOOTH=0x05` local.

#### 2. `assign.c` / `assign.h` — assignment lookup & default (Task 26)

- **Purpose:** Pure in-memory operations on a loaded `cbx_assignments` table.
- Key funcs: `cbx_assign_find_index`, `cbx_assign_lookup`, `cbx_assign_slot_occupied`,
  `cbx_assign_lowest_free_slot` (scans 0..max_slots-1 for first free),
  `cbx_assign_make_default` (uses `CBX_DEFAULT_PROFILE "default"`),
  `cbx_assign_resolve` (lookup → else lowest-free default; returns 0 existing,
  1 default, `-ENOENT` all full, `-EINVAL` null args).
- **Interfaces:** Pure; depends only on `config/config_assignments.h`
  (`cbx_assignments`, `cbx_validate_id`, strncpy bound macros). No I/O.

#### 3. `assign_persist.c` / `assign_persist.h` — atomic persistence (Task 26)

- **Purpose:** Load-modify-save operations on `assignments.yaml`, atomic from the
  caller's perspective (load → change → validate → save via
  `cbx_assignments_save`, which does temp-file+rename atomically).
- Key funcs: `cbx_assign_persist_set`, `cbx_assign_persist_set_slot`,
  `cbx_assign_persist_set_profile`, `cbx_assign_persist_remove` (idempotent),
  `cbx_assign_persist_auto_assign` (the atomic lowest-free-slot path: load → look
  up → else compute slot → insert default → save; returns 0 existing / 1 new /
  `-ENOENT` full / `-EINVAL` / errno on I/O). Internal helper
  `assign_set_in_memory` returns 0 update / 1 insert / `-ENOSPC` full.
- **Interfaces:** Depends on `assign.h` + `config/config_assignments.h`
  (`cbx_assignments_init/load/save`, `cbx_validate_id/profile`,
  `CBX_MAX_ASSIGNMENTS`).

#### 4. `identity_downgrade.c` / `.h` — downgrade detection (Task 27, Spec §6.3)

- **Purpose:** When a device reconnects with a weaker (higher-layer) identity
  (e.g. `USB:SN` → `USB:phys`), fall back to an `ORDER:n` identity instead of
  creating a permanent assignment keyed on an unstable identifier.
- Key funcs + return contract (0 = no downgrade / use new_ident copy; 1 =
  downgrade → `out_ident` becomes `ORDER:n`; `-ENOENT` if new layer NONE;
  `-EINVAL` null args):
  - `cbx_downgrade_check(old_id, new_ident, conn_order, out)` — direct old-vs-new
    layer comparison via `cbx_identity_parse_layer` + `cbx_identity_is_downgrade`.
  - `cbx_downgrade_resolve(a, new_ident, conn_order, out)` — checks the new ID
    doesn't already exist in assignments, then scans for a stronger-layer
    assignment → downgrade.
  - `cbx_downgrade_find_stronger(a, new_layer, out_id, out_len)` — returns the
    strongest (lowest-layer) stored ID stronger than new_layer.
- Internal helper `build_order_identity` (empty string if conn_order < 0).
- **Interfaces:** Pure; uses `identity.h` + `config_assignments.h` +
  `assign.h` (`cbx_assign_lookup`).

#### 5. `gamepad_order_restore.c` / `.h` — GamepadOrder restore (Task 27, gap #2)

- **Purpose:** After InputPlumber restart (GamepadOrder is in-memory only),
  map persisted order IDs back to composite device paths via PersistentId DBus
  queries and re-apply the order.
- Key funcs:
  - `cbx_gamepad_order_restore(backend, bus, model, &restored, &skipped)` —
    orchestration: `ip_gamepad_order_load` → map → `ip_manager_set_gamepad_order`.
    Returns `-ENOENT` if no saved order; errno from load/set; 0 on success.
  - `cbx_gamepad_order_map_ids(...)` — the pure-ish core, exposed for testing:
    CSV split (`csv_for_each`) → each ID matched against model composites via
    `ip_composite_get_persistent_id` → builds CSV of paths. `-ENOSPC` if buffer
    too small.
  - Internal: `csv_for_each` (whitespace-trimmed comma tokenizer, tokens
    < `CBX_MAX_ID_LEN`), `map_one_id` (per-ID composite scan), `map_ctx`.
- **Interfaces:** DBus backend vtable + bus handle + device model
  (`dbus/dbus_interface.h`, `dbus/ip_device_model.h`); calls `dbus/ip_gamepad_order.h`,
  `dbus/ip_manager.h`, `dbus/ip_composite.h`, `config/config_assignments.h`
  bounds. Allocate/free of `persistent_id` in `map_one_id` is handled carefully.

### Entry points (called from outside the subsystem)

- `cbx_identity_extract` / `cbx_identity_parse_layer` / `cbx_identity_init` /
  downgrade helpers — consumed by manager/overlay device-connect path.
- `cbx_assign_resolve`, `cbx_assign_find_index`, `cbx_assign_lookup` — used by
  GUI/overlay slot display (e.g. `src/overlay/close.c`, `grid_render.c`,
  `profile_cycle.c` include identify headers).
- `cbx_assign_persist_*` (esp. `auto_assign`) — the hot persistence path on each
  controller connect.
- `cbx_downgrade_check` / `cbx_downgrade_resolve` — manager reconnect path.
- `cbx_gamepad_order_restore` / `cbx_gamepad_order_map_ids` — restart reapply.

### Internal state / lifecycle

- **Stateless pure functions** for identity/assign/downgrade — no globals or
  static mutable state. State is passed in (loaded `cbx_assignments`, source
  props, device model).
- Persistence is read-through: every `_persist_*` call loads fresh from disk,
  mutates, saves. This is the intended atomicity model (each call minimizes the
  load→save race window), but there is **no cross-process lock** — two
  concurrent connects rely on temp-file+rename atomicity only.
- `gamepad_order_restore.c` uses only stack/alloc'd locals; `persistent_id`
  frees balanced in `map_one_id`.

### Error handling

- All public functions validate args and return `-EINVAL` for null/bad input
  (never crash).
- `cbx_identity_extract` returns `-ENOENT` when no identity, `0` otherwise.
- Persistence surfaces errno from `cbx_assignments_load`/`save`.
- `cbx_assign_persist_set` returns `-ENOSPC` when the table is full on insert.
- `cbx_downgrade_*` returns encoded flags (0/1/errno) as documented above.
- `gamepad_order_restore`: DBus failures on individual composite queries are
  treated as "skip this composite" (not fatal); `-ENOENT` if no saved order;
  setter failure still reports counts.

### Test coverage

Dedicated unit tests (all registered in `tests/CMakeLists.txt`, link
`controllerbox` + cmocka or `cbx_test_support`):

- `tests/test_identity.c` (730 lines) — extraction precedence across all layers,
  MAC/empty-uniq fallthrough, HIDRaw vs evdev serial fallback, invalid/oversize
  serials, phys/order, parse_layer, bustype, is_mac, is_downgrade, null args.
- `tests/test_assign.c` (445 lines) — find_index, lookup, slot_occupied,
  lowest_free_slot (gaps/full/neg), make_default, resolve (existing/new/full/
  no-mutate).
- `tests/test_assign_persist.c` (639 lines) — all `persist_*` ops incl.
  auto_assign; file mode; gamepad_order preservation; table-full; reload-match.
- `tests/test_identity_downgrade.c` (584 lines) — check (all layer pairs,
  order fallback, empty/invalid old id), find_stronger (empty/invalid/same-layer/
  picks-strongest), resolve incl. integration flows.
- `tests/test_order_restore.c` (579 lines) — map_ids (single/multi/stale/DBus
  error/whitespace/order-id/ENOSPC), restore full flow (empty/no-saved/all-stale/
  round-trip).
- `tests/test_gamepad_order.c` (457 lines) — the Task 15 persistence side
  (save/load order CSV) that `gamepad_order_restore` depends on.
- `tests/test_assignments.c` (644 lines) — Task 6 YAML load/save + ID/prof
  validation that the persist layer relies on.

The identify functions are also exercised indirectly by many overlay/manager
integration tests (e.g. `test_manager_interaction_ctrl`, `test_close`,
`test_profile_cycle`, `test_installed_functional`) that route through identity +
assignment on the connect path.

### Potential issues / observations for the planner

1. **No locking around the "atomic" persistence window.** `auto_assign` reduces
   the race but relies solely on temp-file+rename atomicity; simultaneous
   connects across processes could both compute the same lowest-free slot and
   the second save overwrites the first. Worth confirming the intended
   concurrency guarantee (AGENTS.md notes a `controller-production-routing`
   runner for the real path).
2. **`format_usb_phys` permits arbitrary printable non-space chars** in the ID
   that then flow into a YAML key; injection/traversal not checked (relies on
   `cbx_validate_id` at the persistence boundary). Confirm validate is enforced
   on every path — `assign_persist_set` validates, but `auto_assign`'s
   `assign_set_in_memory` validates id via `cbx_validate_id`, so this is covered
   there.
3. **`cbx_downgrade_resolve` treats all stronger-layer assignments as equal**
   signal when the new ID doesn't match; the `find_stronger` helper correctly
   picks the single strongest, so resolve uses only that one. The heuristic
   (a same-layer-ID absent but *any* stronger ID present → downgrade) could
   false-positive when a genuinely-new device happens to have a weaker identity.
4. **`cbx_identity_extract` MAC-on-non-BT-bus branch** (`is_mac` && bus != BT)
   is intentionally a no-op that falls through to layer 3 — but the code comment
   contains unresolved "In practice…" reasoning (dead code path kept for
   documentation). Cleaner as a direct skip rather than a comment-laden block.
5. **`cbx_assign_persist_set_slot`/`set_profile` do not validate the ID** is a
   known existing ID before load (they handle `-ENOENT` correctly), but they
   also don't validate slot bounds beyond `< 0`. Slot ≥ max_slots is not checked
   at persistence time (enforced only by consumers).
6. **`gamepad_order_map_ids` output CSV sizing** uses
   `CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER` stack buffer in `restore`; a
   pathological many-composite model is bounded by the table cap, so overflow is
   prevented, but there is no explicit check tying saved-ID count to that cap at
   the top level.
7. **`csv_for_each` silently drops tokens ≥ `CBX_MAX_ID_LEN`** (skips without
   error) — a very long stale ID is silently omitted rather than surfaced; the
   comment notes this is intentional truncation-by-skip.
8. **Downstream overlay includes identify headers directly** (`src/overlay/close.c`,
   `grid_render.c`, `profile_cycle.c`) — changes to assignment struct layout
   affect these; no issue today but a boundary worth noting.

### Dependencies summary

- `src/config/config_assignments.h`: `cbx_assignments` struct, init/load/save/
  validate, `cbx_validate_id`, `cbx_validate_profile`, bounds macros
  (`CBX_MAX_ASSIGNMENTS=32`, `CBX_MAX_ID_LEN=128`, `CBX_MAX_PROFILE_LEN=64`,
  `CBX_MAX_GAMEPAD_ORDER=16`).
- `src/dbus/ip_composite.h`, `ip_manager.h`, `ip_gamepad_order.h`,
  `dbus_interface.h`, `ip_device_model.h`.
- Consumers in `src/app/`, `src/overlay/`, and manager connect/disconnect flows.
