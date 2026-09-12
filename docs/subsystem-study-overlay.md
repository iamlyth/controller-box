## Subsystem Study Report: Overlay (`src/overlay/`)

Location: `/workspace/project/src/overlay/` (20 files: 10 `.c` + 10 `.h`).
Single production integrator: `src/app/overlay_service.c` + `src/app/overlay_service.h`.

The overlay subsystem is the character-select overlay for the controller-box
daemon: an SDL surface (pre-built render-to-texture) that shows a grid of
physical controllers (rows) × virtual player slots (columns), lets controllers
navigate in Player Mode or Host Mode, and on close applies the resulting slot +
profile routing to InputPlumber and persists assignments. It is driven by a
lifecycle state machine keyed off InputPlumber's `InterceptMode`.

The subsystem cleanly splits into **pure-data logic** (grid, conflict, trigger
parse, dynamic columns, profile cycle, player/host mode, lifecycle state machine)
— all unit-testable with the mock DBus backend and no SDL/file I/O — and
**rendering/SDL** (surface_build, grid_render render path).

---

### Files and responsibilities

#### `trigger.h` / `trigger.c` (Task 32, §2.5, §4.2)
- **Purpose:** Parse a trigger combo string (`Select+A`) and register it on each
  composite device via `SetInterceptActivation`, then set `InterceptMode=1 (PASS)`.
- Key functions:
  - `cbx_trigger_parse()` — splits `"+`-delimited" combo into events CSV + target; trims tokens; validates single-event requirement.
  - `cbx_trigger_register()` — parse + `ip_composite_set_intercept_activation` + `ip_composite_set_intercept_mode`.
  - `cbx_trigger_register_all()` — loops devices, continues on individual failure, returns `-failures`.
- Error handling: `-EINVAL` null/empty, `-ENAMETOOLONG` buffer overflow, counts failures across devices.

#### `close.h` / `close.c` (Task 32, §2.5, §4.5, §11)
- **Purpose:** Close-time coordinate sequence: detect+resolve grid conflicts, sync grid→assignments, save to disk. Lifecycle then handles PASS + hide + IDLE.
- Key: `cbx_close_sync_assignments()` (grid rows → assignment slot/profile, or removal), `cbx_close_on_save()` (the wired `lifecycle.on_save`), `cbx_overlay_request_close()` (wires callback with a **stack-local** `cbx_close_ctx`, calls `cbx_overlay_lifecycle_close`).
- Note: this module provides its own on-save close path, but `overlay_service.c` implements its **own** `cbx_overlay_on_save()` and never wires `cbx_overlay_request_close()`; the two paths are parallel implementations of §4.5.

#### `conflict.h` / `conflict.c` (Task 30, §4.5)
- **Purpose:** Detect when 2+ rows share a P-slot column and resolve by moving the second arrival to the lowest free P-slot.
- Key: `cbx_conflict_detect` (first-by-index is owner, later rows are conflicted), `cbx_conflict_resolve` (row-order, deterministic, moves via `move_left/right`; leaves in place if all slots full), `cbx_conflict_find_lowest_free_slot`, `cbx_conflict_count_occupied`, `cbx_conflict_is_row_conflicted` (used by renderer).

#### `dynamic_columns.h` / `dynamic_columns.c` (Task 31, §4.7, §5.2)
- **Purpose:** Make grid column count track the number of virtual (target) controllers InputPlumber has instantiated.
- Key: `cbx_dynamic_columns_needs_rebuild` (col_count vs target_count+1), `_rebuild` (builds `cbx_virtual_controllers`, preserves profile list across rebuilds), `_clamp_positions` (rows past new max moved to Unassigned), `_extract_types`, `_build_vcs`.

#### `grid_render.h` / `grid_render.c` (Task 29, §4.1, §8.1)
- **Purpose:** Core data model (`cbx_select_grid`) + SDL rendering.
- Key structs: `cbx_select_grid`, `cbx_grid_row`, `cbx_grid_col`, `cbx_grid_composite_info`, `cbx_grid_render_ctx`.
- Key funcs: `cbx_select_grid_build` (rows from composites, assigns `cur_col`/`profile` from assignments), navigation `move_left/right`, `cycle_profile_up/down`, accessors, slot↔col conversion (`col_to_slot`/`slot_to_col`), and `cbx_select_grid_render` (+ `_render_cb`) drawing headers, rows, icons (with settings override), position indicators, conflict-red, host/selected/frozen states.
- NULL-safe render.

#### `host_mode.h` / `host_mode.c` (Task 30, §4.4)
- **Purpose:** Exclusive-host state machine. R3 makes a controller host; it can navigate any row (Up/Down) and edit its slot (Left/Right); others freeze.
- Key: `enter/exit/toggle`, `handle` (returns `CBX_HM_RESULT_*`), `row_state` (SELECTED > HOST > FROZEN priority), `is_frozen`, `on_slot_change` callback.

#### `lifecycle.h` / `lifecycle.c` (Task 28, §2.5, §11)
- **Purpose:** Overlay state machine `IDLE → ACTIVATING → VISIBLE → CLOSING → IDLE`, timeout watchdog, fade animation, InterceptMode=PASS on close.
- Key: `init`, `activate` (IDLE→ACTIVATING), `close` (fires `on_save`, sets PASS, fade-out; allows close from VISIBLE or ACTIVATING), `tick` (polls animation; in VISIBLE increments timeout and force-closes), `force_close` (no on_save), `get_state`, `is_active`, `state_name`.
- Internal state: `cbx_overlay_lifecycle` struct fields (`state`, `visible_ticks`, `error_count`, fade anim). No static/global state.

#### `player_mode.h` / `player_mode.c` (Task 29, §4.3)
- **Purpose:** Default mode; each controller edits its own row independently.
- Key: `cbx_player_mode_handle` (LEFT/RIGHT move slot, UP/DOWN cycle profile, B→CLOSE, R3→HOST; fires `on_slot_change`/`on_profile_change`), `get_slot`, `get_profile`.

#### `profile_cycle.h` / `profile_cycle.c` (Task 31, §4.6, §10.2, §7.2)
- **Purpose:** Profile enumeration → grid population → profile change → `LoadProfilePath` + assignment update workflow.
- Key: `init`, `load_profiles` (clears grid, adds filenames), `find_path`, `update_assignment`, `apply` (**loads profile via `ip_composite_load_profile_path`, then verifies via read-back of `ProfilePath` before persisting** — §10.2 verification), `profile_follows` (confirms per-row profile model).
- **`profile_cycle.c` uses `PATH_MAX` but does NOT `#include <limits.h>`** — relies on transitive include via `<stdio.h>`/`<string.h>` (works on glibc, fragile elsewhere). See Potential issues.

#### `surface_build.h` / `surface_build.c` (Task 24, §4.9, §11)
- **Purpose:** Pre-built `SDL_TEXTUREACCESS_TARGET` overlay texture at screen resolution; show/hide is a single `SDL_RenderCopy`+`Present` (no allocation in hot path, <10 ms). Dirty-rect incremental re-render.
- Key: `init` (creates texture, applies opacity, blend mode), `destroy`, `set/get_opacity`, `show`, `hide` (does not destroy texture — §11), `mark_dirty`, `mark_dirty_all`, `is_dirty`, `render` (switches render target to texture, merges+clips dirty rects via `cbx_dirty_rect_render`, calls `fn`, restores target, clears dirty on success), accessors.

---

### Entry points (called from outside the subsystem)
Primary consumer is `src/app/overlay_service.c` (`run_overlay_service` + `cbx_overlay_service_step`). Used directly:
- `cbx_overlay_surface_init/destroy/show/hide/render/mark_dirty_all/is_dirty` — surface lifecycle & render.
- `cbx_select_grid_build/init`, `cbx_grid_render_ctx`, `cbx_select_grid_render_cb` — grid build + render.
- `cbx_conflict_list_init`, `cbx_conflict_detect` — conflict highlights.
- `cbx_trigger_register_all`, `cbx_trigger_register` — activation combo on each composite.
- `cbx_overlay_lifecycle_init/activate/close/tick/force_close/is_active` — state machine.
- `cbx_player_mode_init/handle` (via `cbx_pm_*` callbacks `cbx_overlay_on_slot_change`, `cbx_overlay_on_profile_change`).
- `cbx_host_mode_init/handle/toggle/is_active/get_host_row/get_selected_row/exit` (callback `on_host_slot_change`).
- `cbx_profile_cycle_init/apply/load_profiles`.
- `cbx_dynamic_columns_needs_rebuild/rebuild`.
- Overlay-service's own `cbx_overlay_on_save()` is the real `lifecycle.on_save` (parallel to `close.c`'s).

Test hooks (exported only under `CBX_TESTING`): `on_intercept_activating/deactivating/error`, and various `cbx_overlay_*` step/input helpers in the service header.

---

### Internal state / lifecycle
- All state is held in caller-provided structs (`cbx_select_grid`, `cbx_overlay_lifecycle`, `cbx_player_mode`, `cbx_host_mode`, `cbx_overlay_surface`, `cbx_profile_cycle`). **No static/global mutable state** inside `src/overlay`.
- The only globals live in `overlay_service.c` (`static volatile sig_atomic_t g_running`); not part of the pure subsystem.
- Lifecycle: surface texture created once at startup and reused; lifecycle re-arms idle polls so a later activation works; hotplug triggers a full grid/trigger/poll rebuild via `cbx_overlay_reconcile_hotplug`.

---

### Error handling
- Pure-data functions return negated errno (`-EINVAL`, `-ERANGE`, `-ENOENT`, `-ENOSPC`, `-ENAMETOOLONG`, `-EIO`, `-ENODEV`, `-ENOENT`, `-EPERM`).
- Null args guarded throughout; accessors return safe defaults (`-1`/`false`/`NULL`) on bad args rather than crashing.
- DBus failures propagate up from `ip_composite_*`/`ip_target_*`/`ip_manager_*`; lifecycle tracks `error_count` vs `max_errors` and fires optional `on_error` callback.
- `cbx_profile_cycle_apply` does **read-back verification** of `ProfilePath`; mismatch → `-EIO`, guaranteeing failed loads are never saved.
- Render path is NULL-safe no-op.

---

### Test coverage
Unit tests in `tests/` map 1:1 to modules (build dir `build-planner`):
- `test_trigger.c`, `test_close.c`, `test_conflict.c`, `test_dynamic_columns.c`,
  `test_grid_render.c`, `test_host_mode.c`, `test_overlay_lifecycle.c`,
  `test_player_mode.c`, `test_profile_cycle.c`, `test_surface_build.c`.
- Integration/driver tests for the service layer: `test_overlay_integration.c`,
  `test_overlay_interaction.c`, `test_overlay_latency.c`, `test_overlay_native.c`,
  `test_overlay_reconcile.c`, `test_overlay_service.c`,
  `test_overlay_visual.c` (framebuffer/SDL), and `test_widget_grid.c`.

Test harness assets: `tests/dbus_mock.{c,h}` (mock backend), `tests/interaction_inventory.{c,h}`, `tests/fixtures/`, `tests/golden/`.

---

### Potential issues / risks
1. **Stack-local on_save struct in `close.c` (`cbx_overlay_request_close`).** `cbx_close_ctx ctx` is stack-local; the code relies on `cbx_overlay_lifecycle_close` invoking `on_save` synchronously and never reusing `on_save_data` afterward. This is a latent use-after-scope hazard if the lifecycle ever defers/caches/save is ever called twice — worth an explicit contract test. Note this path is **not** the one production uses (overlay_service.c wires its own `cbx_overlay_on_save`), so `close.c`'s close-coordination and the service's on_save are duplicated implementations of §4.5.
2. **Missing `#include <limits.h>` in `profile_cycle.c`** — uses `PATH_MAX` (line 126) without including it; only compiles because a transitively-included header happens to provide it on the toolchain. Add the include for portability.
3. **Dynamic-columns edge (SPEC §5.2):** when a slot is removed mid-session, `cbx_dynamic_columns_clamp_positions` moves the occupant row to Unassigned by direct struct access (`grid->rows[i].cur_col = CBX_GRID_UNASSIGNED_COL`) rather than via `cbx_select_grid_move_left` — no side-effect callback fires, so no assignment/engine update until next close. Confirm the service re-applies state on the reconcile path.
4. **Host/selected visual tie:** in `cbx_host_mode_row_state`, `SELECTED` is checked before `HOST`, so if `selected_row == host_row` the host's own row renders as SELECTED (blue), not HOST (green). Likely intended but undocumented; the green host-indicator path only shows when editing a *different* row.
5. **`cbx_conflict_detect` records max one conflict per column pair** per row (breaks after first match); acceptable but means a row sharing a column used by 2 earlier rows is still counted once. `conflicted row is second-arrival` semantics are index-order based — behavior consistent but worth a comment.
6. **Profile-cycle duplicate-path:** `cbx_profile_cycle_update_assignment` creates a default assignment via `cbx_assign_resolve` when none exists — on a full `assignment_count` this returns `-ENOSPC`, which aborts the whole `cbx_profile_cycle_apply` even though the engine already loaded the profile. Minor inconsistency (engine applied but no assignment persisted) — a partial-failure gap for the "verified before persist" contract.
7. **Error-count semantics in lifecycle:** `set_intercept_pass` increments `error_count` on every DBus failure and resets only on success, but `max_errors` is **defined but never acted upon** (no code force-closes or escalates when `error_count >= max_errors`). Dead field / unused guard.
8. **Trigger parse** does not bound the per-token `tmp[128]` against the CSV buffer interaction precisely (it truncates segment at 127 but then copies `tok_len` into `events_csv` checking only `csv_pos+tok_len >= csv_len`); overall bounds are enforced, low risk, but the two-buffer size coupling (`CBX_TRIGGER_CSV_LEN` = 128) is implicit.

---

### Key cross-module dependencies
- `identify/assign.*` — `cbx_assign_lookup`, `cbx_assign_find_index`, `cbx_assign_resolve`, `CBX_DEFAULT_PROFILE` (grid build, close, profile cycle).
- `dbus/ip_composite.*` — `SetInterceptActivation`, `SetInterceptMode`, `LoadProfilePath`, `GetProfilePath`, `GetTargetDevices`, `GetDbusDevices`, `GetPersistentId`, `GetName`.
- `dbus/ip_manager.*`, `dbus/ip_target.*`, `dbus/ip_objectmanager.*`, `dbus/ip_device_model.*`, `dbus/ip_intercept_poll.*`, `dbus/ip_input_signal.*`, `dbus/ip_hotplug.*`, `dbus/ip_connection.*` — through the service layer.
- `config/config_settings.*`, `config/config_assignments.*`, `config/config_profile_list.*`.
- `ui/renderer.*`, `ui/theme.*`, `ui/text.*`, `ui/animation.*`, `ui/dirty_rect.*`, `icons/icon_cache.*`, `icons/icon_map.*`, `icons/icon_lookup.*`.

Planner note: tasks 24, 28–32 are fully implemented; follow-up work should focus on de-duplicating the two §4.5 on-save paths, the `max_errors` dead field, the `limits.h` include, and the clamp-without-callback behavior in dynamic_columns.
