# Subsystem Study Report: dbus

**Location:** `/workspace/project/src/dbus/` (14 `.c` + 14 `.h` files)
**Purpose:** All communication with the InputPlumber system DBus service (`org.shadowblip.InputPlumber`) — connection lifecycle, object discovery, signal handling, and method/property wrappers — abstracted behind a testable function-pointer vtable.

## 0. Core architecture (read this first — it frames everything)

The subsystem is split into two layers, both defined in `dbus_interface.h`:

1. **The vtable abstraction `ip_dbus_backend`** — a struct of function pointers
   (`connect`, `disconnect`, `get_unique_name`, `get_connection_creds`,
   `call_method`, `get_property`, `set_property`, `get_managed_objects`,
   `subscribe_signal`, `inject_signal`, `process`). The `ip_bus_handle` is an
   opaque `void *` that wraps an `sd_bus *` in production or mock state in tests.
   Shared payload structs (`ip_owner_changed_payload`,
   `ip_interfaces_changed_payload`, `ip_properties_changed_payload`,
   `ip_input_event_payload`) are defined here so prod + tests speak the same ABI.
2. **Consumers** (`ip_connection.c`, `ip_objectmanager.c`, `ip_hotplug.c`,
   `ip_properties.c`, `ip_composite.c`, `ip_manager.c`, `ip_input_signal.c`,
   `ip_intercept_poll.c`, `ip_source.c`, `ip_target.c`, `ip_create_composite.c`,
   `ip_gamepad_order.c`) which hold `const ip_dbus_backend *backend` + `ip_bus_handle` and call through the vtable. **They never touch `sd_bus_*` directly.**

**Two backends implement the vtable:**
- Production: `dbus_client.c` → `sd_bus_*` (compiled into `libcontrollerbox`, exposed via `ip_dbus_sd_backend()`).
- Test: `tests/dbus_mock.c` (canned expectations + `inject_signal`) — **never included by `src/`.**

This is why nearly every unit test exercises the same production logic paths with a mock bus, and why `test_native_dbus.c` / `tests/native_ip_server.c` additionally spin up a real InputPlumber-compatible server for integration coverage.

## 1. File-by-file

### `dbus_interface.h` (header only)
- **Purpose:** vtable definition, DBus constants, signal payload structs.
- **Key items:** all `IP_DBUS_NAME/PATH/IFACE_*` constants; `ip_dbus_backend`; typedefs for signal callbacks (`ip_signal_cb`); `ip_owner_changed_payload`, `ip_interfaces_changed_payload`, `ip_properties_changed_payload` (incl. `ip_prop_type` enum), `ip_input_event_payload`; declares `ip_dbus_sd_backend()`.

### `dbus_client.[c|h]` — **Production sd-bus backend vtable** (the one real-DBus file)
- **Purpose:** implements every vtable slot with real sd-bus calls, plus sender-verification state.
- **Struct `sd_bus_wrapper`:** holds `sd_bus *bus`, `slots[16]`/`slot_data[16]`, `slot_count`, `expected_sender` (InputPlumber's tracked unique name), `expected_pid`, `expected_pid_set`.
- **Key functions:**
  - `sd_connect` / `sd_disconnect` — open system bus / unref slots + close.
  - `sd_get_unique_name` — `GetNameOwner`; on `NameHasNoOwner` → `IP_ERR_SERVICE_UNKNOWN`; caches `expected_sender` + PID fingerprint (F3).
  - `sd_query_owner_creds` / `sd_get_connection_creds` — `GetConnectionCredentials` for (pid,uid) verification.
  - `sd_sender_ok` — rejects any signal whose sender ≠ `expected_sender`; **when `expected_sender` is NULL (IP down/unverified) all signals are dropped** (anti name-squatting).
  - 5 signal callbacks: `sd_noc_callback` (NameOwnerChanged, also re-verifies + updates `expected_sender`/PID), `sd_interfaces_added_callback` (`oa{sa{sv}}` → CSV of iface names), `sd_interfaces_removed_callback` (`oas`), `sd_properties_changed_callback` (`sa{sv}as`, builds per-property payloads; handles `s`, `as`, invalidated), `sd_input_event_callback` (`sd`). Each rebuilds a comma-separated iface/array string via `open_memstream`.
  - `sd_get_property` — dispatches by `ip_dbus_property_signature()`: `as` for arrays, `u` for InterceptMode, `b` for ManageAllDevices/Enabled, else `s`.
  - `sd_set_property` — builds `Properties.Set` with the correct variant type; validates uint/bool strings.
  - `sd_call_method` — variadic; iterates `sig`; `as` → split CSV into array; last vararg is always `char **out`; reads `s` reply if out non-NULL.
  - `sd_get_managed_objects` — `GetManagedObjects`, serializes `a{oa{sa{sv}}}` into text `path\tiface,iface…` lines (shared parse format with mock).
  - `translate_sd_error` — maps DBus error names → `IP_ERR_*` codes.
  - `sd_process` — wraps `sd_bus_process` (returns 0/>0/negative).
  - `sd_inject_signal` → `-ENOSYS` (prod never injects).
- **Property-type tables:** `sd_is_array_property` (GamepadOrder, TargetDevices, SourceDevicePaths, SupportedTargetDeviceIds, SupportedTargetDevices, Capabilities, OutputCapabilities, TargetCapabilities, DbusDevices), `sd_is_uint_property` (InterceptMode), `sd_is_bool_property` (ManageAllDevices, Enabled).

### `ip_connection.[c|h]` — **Connection lifecycle & name-ownership tracking**
- **Purpose:** connect, read `Version` (compat ≥ 0.78.0), subscribe NameOwnerChanged, manage CONNECTED/DEGRADED/DISCONNECTED states, drive re-enumeration callbacks.
- **Struct `ip_connection`:** backend, bus, `state`, `unique_name`, `version`, `expected_pid/uid`, `sender_verified`, reenumerate/degraded callbacks. **This is the struct `overlay_service.c` embeds as `svc->conn` and `manager.c` as `mgr->connection`.**
- **Key functions:**
  - `ip_connection_init`, `ip_connection_set_bus`, `ip_connection_connect`, `ip_connection_disconnect`.
  - `ip_connection_connect`: (1) connect, (2) subscribe NameOwnerChanged, (3) read `Version`. ServiceUnknown → DEGRADED but **stays connected** to catch later start; version incompatible → DEGRADED + `IP_ERR_INCOMPATIBLE`; other errors (incl. AccessDenied) → DISCONNECTED. On success then `get_unique_name` + `verify_sender`.
  - `verify_sender` (static) — `get_connection_creds` then `ip_connection_uid_is_trusted` (uid==0 or ==geteuid()).
  - `ip_connection_handle_name_changed` — on acquire: re-verify sender (fail → DEGRADED + degraded_cb), re-read Version, fire reenumerate_cb; on loss: clear state, DEGRADED + degraded_cb. Transfers (both non-empty) ignored.
  - `ip_connection_reason_for_error`, `ip_version_is_compatible`, `ip_connection_uid_is_trusted`.
- **Entry points (outside):** `ip_connection_init/connect/disconnect/*_get_*/is_*`, `set_reenumerate_cb`, `set_degraded_cb`, `reason_for_error`.

### `ip_objectmanager.[c|h]` — **Enumeration (GetManagedObjects)**
- `cbx_objectmanager_enumerate` — calls `get_managed_objects`, frees reply, parses.
- `cbx_objectmanager_parse_reply` — parses text fixture (shared with backend): splits lines, path-prefix validates against `IP_DBUS_PATH "/"`, classifies Manager/Composite/Source/Target. **Note:** target add requires `IP_IFACE_TARGET` in iface list (source does not). Sorts composites by index, devices by path (stable order). Uses `strtok_r` on a strdup'd copy.
- **Entry points (outside):** `overlay_service.c:535,995`, `controllers_tab.c:571`.

### `ip_device_model.[c|h]` — **In-memory device model**
- Plain-data structs `cbx_device_model` (manager + composites[16] + sources/targets[64]); `cbx_composite_entry` (path+index), `cbx_device_entry` (path+name).
- Init/find_*/set/remove/`add_*`/`remove_*` (idempotent adds, `parse_composite_index` with `strtol`+bounds). **This is NOT a DBus I/O module** — it's the shared data structure used by objectmanager (populate), hotplug (mutate), manager (GamepadOrder validation).

### `ip_hotplug.[c|h]` — **InterfacesAdded / InterfacesRemoved handling**
- `ip_hotplug` struct holds backend/bus/`expected_sender`/model/`model_changed`.
- `ip_hotplug_subscribe` (both signals), `ip_hotplug_handle_added/removed`: sender_ok + `path_is_valid` + `classify_device_path` (exact prefix at the right position — hardened against substring confusion), then mutate model via device_model add/remove.
- **Entry point (outside):** wired through overlay/manager signal dispatch.

### `ip_properties.[c|h]` — **PropertiesChanged filtering/dispatch**
- Tracked property spec table: GamepadOrder(ARRAY), ProfileName(STRING), ProfilePath(STRING), TargetDevices(ARRAY), SourceDevicePaths(ARRAY). **InterceptMode intentionally NOT here** (gap #1 → polled).
- `ip_properties_handle_changed`: sender_ok, find spec, type-match, length/array-size limits (name 256, path 4096, ≤256 elems), then fire `ip_prop_changed_cb`.
- **Entry point:** user callback invoked by overlay/manager.

### `ip_composite.[c|h]` — **CompositeDevice interface wrappers**
- Methods: `ip_composite_set_intercept_activation` (`ass`), `load_profile_path`, `load_profile_from_yaml`, `get_profile_yaml`, `set_target_devices` (`as`), `stop`, plus `set_target_device_paths`.
- Properties: get/set InterceptMode, get TargetDevices/SourceDevicePaths/PersistentId/ProfileName/ProfilePath/Name/Capabilities/OutputCapabilities/TargetCapabilities/DbusDevices. All thin → `call_method`/`get_property`/`set_property`.
- Constants `IP_INTERCEPT_NONE/PASS/ALL/GAMEPAD_ONLY` (0–3).

### `ip_manager.[c|h]` — **Manager interface wrappers**
- Methods: `create_target_device` (`s`→path), `stop_target_device`, `attach_target_device` (`ss`), `set_target_devices` (→CompositeDevice iface).
- Properties: `get/set_gamepad_order` (setter validates every path exists in `cbx_device_model` before the DBus call — `validate_gamepad_order_paths`), `get_supported_target_device_ids`, `get_supported_target_devices`.
- Declares `IP_DBUS_MANAGER_PATH`.

### `ip_create_composite.[c|h]` — **CreateCompositeDevice temp-file workaround (gap #3)**
- `ip_create_composite_device(backend, bus, yaml, &out_path)`: mkstemp/mkstemps 0600 in XDG_RUNTIME_DIR (else /tmp), fstat captures dev/ino, fchmod 0600, write-loop, fsync (non-fatal), `lstat` re-check for TOCTOU (reject on dev/ino mismatch → `-ESTALE`), `call_method` with the temp path (never user path), then **always `unlink`**.

### `ip_gamepad_order.[c|h]` — **GamepadOrder persistence (gap #2)**
- `ip_gamepad_order_save(backend, bus, model, paths_csv)` — iterates CSV composite paths, verifies each `cbx_device_model_find_composite`, queries PersistentId, appends to `cbx_assignments.gamepad_order` (validate + dedupe), saves.
- `ip_gamepad_order_load(&out_csv)` — loads assignments, builds CSV of valid IDs.
- Depends on `config/config_assignments.[ch]` (`cbx_assignments_*`).

### `ip_intercept_poll.[c|h]` — **InterceptMode polling state machine (gap #1)**
- States IDLE → PASS_WAIT → ACTIVE; `ip_intercept_poll_start` creates `SDL_AddTimer` (50ms) whose cb pushes a custom `SDL_UserEvent` (data1=poll); main loop calls `ip_intercept_poll_tick()`.
- `ip_intercept_poll_tick`: `get_property("InterceptMode")`, on ALL/GAMEPAD_ONLY → fire `activating_cb` → ACTIVE; on PASS/NONE in ACTIVE → fire `deactivating_cb` → IDLE; error counting (`max_errors=5`) + ACTIVE timeout (`200 ticks` ≈10s) → `error_cb` + reset. Pure logic — fully unit-testable without SDL.

### `ip_input_signal.[c|h]` — **InputEvent handling**
- Enum `ip_input_id` (buttons + 4 stick axes), `s_input_table[]` string→id map (incl. aliases Back/Home/LeftBumper/RightTrigger/LeftStick…).
- `ip_input_parse`, `ip_input_category_of`, `ip_input_validate_value` (button ==0.0/1.0; axis finite in [-1,1]).
- Per-device rate limiter: `ip_rate_limiter_entry[64]`, 200 events/s window via `CLOCK_MONOTONIC` ms.
- `ip_input_events_handle`: sender check → path check → parse → category/value validate → rate-limit → fire `ip_input_event_cb`.
- `ip_input_events_process` — drains `process()` loop.

### `ip_source.[c|h]` / `ip_target.[c|h]` — **Source/Target device property wrappers**
- `ip_source_get_name/unique_id/phys_path/id_vendor/id_product/id_bustype/serial_number` — each takes an `iface` arg (caller picks Event/Udev/HIDRaw; note serial=UniqueId vs SerialNumber).
- `ip_target_get_name/device_type` — `IP_IFACE_TARGET`, DeviceType is the icon-mapping key.

## 2. Cross-module interfaces (this subsystem's contract with the rest)

- **Consumers of `libcontrollerbox` that drive DBus:** `src/app/overlay_service.c` (owns an `ip_connection` embedded in its service struct, gets backend via `ip_dbus_sd_backend()`, wires reenumerate/degraded callbacks, enumerates), `src/manager/manager.c` (`mgr->connection`, `mgr->dbus_backend`), `src/manager/controllers_tab.c` (enumerate + error reasons).
- **Signal → handler wiring:** overlay/manager register the backend's `subscribe_signal` with the module-level callbacks and the SDL/event loop calls `process` (or mock `inject_signal`) to dispatch.
- **Shared data types used outside:** `cbx_device_model` (identify/gamepad_order_restore.c references it), `ip_input_id` & `ip_input_event_cb` (`src/ui/input_map.h`, `src/overlay/*`).
- **Persistence dependency:** `ip_gamepad_order` → `config/config_assignments.h`.

## 3. Entry points (called from outside the subsystem)

- `ip_dbus_sd_backend()` — get the production backend.
- `ip_connection_*` — init/connect/disconnect/state/get/set_cb/reason/version/is_compatible/uid_is_trusted.
- `cbx_objectmanager_enumerate/parse_reply`.
- `cbx_device_model_*` (init/find/add/remove/set/remove_manager).
- `ip_hotplug_*`, `ip_properties_*`, `ip_composite_*`, `ip_manager_*`, `ip_source_*`, `ip_target_*` wrapper calls.
- `ip_create_composite_device`, `ip_gamepad_order_save/load`.
- `ip_intercept_poll_*` (init/start/stop/tick/state_name).
- `ip_input_events_*` (init/subscribe/handle/process/reset_rate_limiters), `ip_input_parse/category_of/validate_value`.

## 4. Internal state & lifecycle

- `sd_bus_wrapper` (static per connect): slots (unref'd on disconnect), `expected_sender`/`expected_pid` (updated by NameOwnerChanged + get_unique_name).
- `ip_connection` (caller-allocated, embedded in overlay/manager): state machine + `unique_name`/`version`/`sender_verified`; all strings heap-freed in `disconnect`/on loss.
- `cbx_device_model`/`ip_hotplug.model`/`ip_properties`/`ip_input_events`: caller-owned; `model_changed` flag.
- `ip_intercept_poll.timer_id` (SDL timer, `SDL_RemoveTimer` on stop/disconnect); `state`, `error_count`, `timeout_ticks`.
- Static tables: `s_input_table`, `prop_specs`, property-type helpers — immutable, no mutable globals shared across connections.

## 5. Error handling conventions

- **Return codes:** 0 = success; **negative errno** (`-EINVAL`, `-ENOMEM`, `-ENOSYS`, `-EHWPOISON`…) **plus categorized `IP_ERR_*`** from `ip_connection.h`: `IP_ERR_SERVICE_UNKNOWN`=`-EUNATCH`, `IP_ERR_ACCESS_DENIED`=`-EACCES`, `IP_ERR_NO_REPLY`=`-ETIMEDOUT`, `IP_ERR_INVALID_ARGS`=`-EINVAL`, `IP_ERR_NOT_CONNECTED`=`-ENOTCONN`, `IP_ERR_INTERNAL`=`-EIO`, `IP_ERR_INCOMPATIBLE`=`-ENOSYS`. `translate_sd_error` maps DBus error names to these in the prod backend; the mock returns them directly.
- **Signal handlers:** failures are generally **silently dropped** (return 0, don't kill the bus) — parse errors, sender mismatches, unknown events. Logging is minimal (only `fprintf(stderr,…)` for AccessDenied in `ip_connection_connect`).
- **Memory:** callers own returned `char **out_*` (must free); signal payload strings are ephemeral; temp file always unlinked.

## 6. Test coverage (tests/CMakeLists.txt)

Direct unit tests (mock backend + `inject_signal`):
- **test_connection** — connection lifecycle, version compat, degraded modes, sender verify.
- **test_objectmanager_parse** — enumeration parsing, classification, sorting, path validation.
- **test_hotplug** — InterfacesAdded/Removed, sender/path validation, model mutation.
- **test_properties_changed** — tracked-prop filtering, type/length/array validation.
- **test_manager_calls** — Manager wrappers, GamepadOrder validation.
- **test_composite_calls** — CompositeDevice wrappers.
- **test_intercept_poll** — state machine transitions/timeouts (tick driven, no SDL).
- **test_input_signal** — parse/mapping, value validation, rate limiting.
- **test_source_props**, **test_target_props** — source/target property wrappers.
- **test_create_composite** — temp-file workaround.
- **test_gamepad_order** — persistence round-trip.
- **test_manager_dbus_inject** — Manager DBus integration with injected signals (cmocka).
- **test_dbus_signatures** — production transport type signatures (guards string-only mock regression).

Integration/native:
- **test_native_dbus** (with `native_ip_server.c`) — real sd-bus against a live InputPlumber-compatible server.
- Higher-level consumers exercising the stack: `test_manager_native*`, `test_overylal*`/`test_overlay_service`, `test_order_restore`, `test_manager_integration`.

## 7. Potential issues / observations (candidate implementation tasks)

1. **Rate limiter bypass on table saturation — fail-open (security/DoS).** In `ip_input_signal.c`, `find_rate_limiter` returns NULL when `IP_INPUT_MAX_DEVICES` (64) slots are all busy with other devices; `rate_limit_check` then returns `true` (accept). An attacker controlling many device paths (or InputPlumber emitting events across >64 distinct paths) could exceed the 200/s budget. Consider returning `false` (or reusing the oldest entry) instead of failing open. Task 14-related hardening.
2. **`now_ms()` returns 0 on `clock_gettime` failure**, which makes the per-device window start at 0 and can cause the first `event_count` window to appear expired (harmless) — but combined with (1) it's a weak control. Low severity.
3. **Two independent "expected sender" state copies.** The sd layer tracks `sd_bus_wrapper->expected_sender` (updated by `sd_noc_callback`/`sd_get_unique_name`), while `ip_connection->unique_name` is a separate copy maintained by `ip_connection_handle_name_changed`. Both are handed to module handlers as `expected_sender`. If they ever diverge (e.g. re-verification ordering on name change), sd signal callbacks could reject a signal the module accepts or vice versa. Worth a dedicated consistency test.
4. **`sd_connect` does not clear `*bus` on failure.** If `sd_bus_open_system` fails, `*bus` retains its prior (possibly non-NULL) value while the call returns negative; caller could attempt `disconnect` on a garbage handle. Guard: set `*bus` to NULL on entry/failure in `sd_connect` (**and confirm `overlay_service.c`/`manager.c` zero or pre-check the handle**).
5. **Source vs target classification asymmetry (intentional but worth a test).** `cbx_objectmanager_parse_reply` adds a target only if `IP_IFACE_TARGET` is in the iface list, but adds a source solely on path prefix. A crafted source path under `devices/source/` without a source iface is still added. Verify this matches intended behavior; the hotplug path has the same asymmetry.
6. **`verify_sender` failure path** in `ip_connection_handle_name_changed` acquires → if `verify_sender` fails the code goes degraded but does **not** free an already-`strdup`'d? — it does free. But note: on acquire-verify failure it returns early *before* re-reading Version, leaving `conn->unique_name` NULL while `version` may still hold a stale value from before the stop/start. Consider explicitly clearing `version`/`sender_verified` on acquire-failure.
7. **`fclose` return values ignored** throughout signal-callback `open_memstream` usage (`sd_interfaces_removed_callback`, etc.) — writes are append-only to a memstream so risk is minimal, but the pattern is inconsistent with the checked paths.
8. **`IP_ERR_INCOMPATIBLE` and `IP_ERR_INVALID_ARGS` share `-ENOSYS`/`-EINVAL` with ordinary errno values**, so error-code discrimination relies on callers checking the specific return path (e.g. `ip_connection_connect` returns `IP_ERR_INCOMPATIBLE` directly). Tests assert these explicitly; keep them distinct to avoid ambiguity with `-EINVAL` from `ip_connection_connect` argument errors (currently `connect` returns `IP_ERR_INTERNAL` for NULL backend, not `-EINVAL`).
9. **`ip_intercept_poll_start` allows only 50ms interval fixed** and returns `-EIO` on `SDL_AddTimer` failure — fine, but there is no cleanup/lock around `timer_id` if `stop` is called from another thread (SDL timers run on a separate thread). If the main loop can call `stop` concurrently with a pending timer callback, a use-after-free of `poll` (passed as `data1`) is conceivable. Confirm SDL event-loop threading assumptions.
10. **`ip_manager_set_gamepad_order` empty-`model` acceptance:** `validate_gamepad_order_paths` returns true for an empty `value` even if `model` is NULL-guarded earlier, but a non-empty value with a NULL model returns false — consistent, but note `set_gamepad_order` is the only manager setter that validates before DBus; `set_target_devices` does not (by design, it operates on type strings).

## 8. Suggested next-step focus for the planner

High value, well-isolated areas to build tasks around:
- Hardening pass on the InputEvent rate limiter fail-open path (#1) + a consistency test for dual `expected_sender` copies (#3).
- Cleanup/test-focus on `sd_connect` handle clearing (#4) and acquire-failure state hygiene (#6).
- Explicit tests locking down source/target classification (#5) and the `IP_ERR_INCOMPATIBLE`/errno discrimination (#8) so future changes don't regress the sender-verification model (F3 / SPEC §10.1).
