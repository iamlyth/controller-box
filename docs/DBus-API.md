# DBus API Integration

Controller-Box communicates with InputPlumber over the **system DBus** using
**sd-bus** (part of systemd — zero additional dependencies).  All DBus
operations go through a function-pointer vtable (`ip_dbus_backend`) so that
the client layer is fully unit-testable with a mock backend.

## Connection Model (Task 9)

### Bus and name

| Property | Value |
|---|---|
| Bus | System bus (`sd_bus_open_system()`) |
| Well-known name | `org.shadowblip.InputPlumber` |
| Root path | `/org/shadowblip/InputPlumber` |
| Manager interface | `org.shadowblip.InputManager` |
| Manager path | `/org/shadowblip/InputPlumber/Manager` |

### Native property signatures

The production backend decodes InputPlumber values by their native variant
signature rather than coercing mocks to strings: ordinary names and paths use
`s`; list properties such as `GamepadOrder`, `TargetDevices`, capabilities,
and device-path lists use `as`; `InterceptMode` uses `u`; and flags such as
`Enabled` and `ManageAllDevices` use `b`. The vtable retains comma-separated
text only as its internal compatibility representation after native decoding.
`test_dbus_signatures` protects this classification; the installed functional
fixture remains the release authority for wire compatibility.

### Connection lifecycle

1. **Connect** — `sd_bus_open_system()` establishes the system bus connection.
2. **Subscribe** — A match rule for `NameOwnerChanged` with `arg0='org.shadowblip.InputPlumber'` is registered before any property reads, so the GUI detects daemon start/stop even if InputPlumber is not running at launch.
3. **Version check** — Reads the `Version` property from the Manager interface. This doubles as a liveness probe:
   - **Success** → `CONNECTED` state.  The unique bus name is resolved via `GetNameOwner` for signal sender verification.
   - **ServiceUnknown** → `DEGRADED` state.  The bus connection stays open; `NameOwnerChanged` will notify when InputPlumber starts.
   - **AccessDenied** → Logs guidance to add the user to the `inputplumber` group; the connection is closed.
   - **NoReply / InvalidArgs** → Connection is closed; the error code is returned to the caller.

### NameOwnerChanged tracking

The `NameOwnerChanged` signal (from `org.freedesktop.DBus`) is filtered to
`arg0='org.shadowblip.InputPlumber'`.  On receipt:

| Event | old_owner | new_owner | Action |
|---|---|---|---|
| Name acquired | `""` | `":1.N"` | Update unique name, re-read Version, set `CONNECTED`, fire re-enumeration callback |
| Name lost | `":1.N"` | `""` | Clear unique name and version, set `DEGRADED`, fire degraded callback |
| Transfer (ignored) | `":1.A"` | `":1.B"` | No state change |

### Categorized error codes

DBus error names are translated to negative errno values:

| DBus error | errno | Macro |
|---|---|---|
| `ServiceUnknown` / `NameHasNoOwner` | `-EUNATCH` | `IP_ERR_SERVICE_UNKNOWN` |
| `AccessDenied` | `-EACCES` | `IP_ERR_ACCESS_DENIED` |
| `NoReply` | `-ETIMEDOUT` | `IP_ERR_NO_REPLY` |
| `InvalidArgs` | `-EINVAL` | `IP_ERR_INVALID_ARGS` |

### Vtable abstraction

```c
typedef struct ip_dbus_backend {
    int  (*connect)(ip_bus_handle *bus);
    void (*disconnect)(ip_bus_handle bus);
    int  (*get_unique_name)(ip_bus_handle bus, const char *well_known, char **out);
    int  (*call_method)(...);
    int  (*get_property)(...);
    int  (*set_property)(...);
    int  (*get_managed_objects)(...);
    int  (*subscribe_signal)(ip_bus_handle bus, const char *iface,
                             const char *member, ip_signal_cb cb, void *ud);
    int  (*inject_signal)(...);  /* test-only */
} ip_dbus_backend;
```

- **Production backend** (`src/dbus/dbus_client.c`): implements the vtable
  using real sd-bus calls.  `ip_bus_handle` wraps a `sd_bus_wrapper` struct
  that holds the `sd_bus*` and registered match-signal slots.
- **Mock backend** (`tests/dbus_mock.c`): a canned-response store that
  replays expectations registered by tests.  Signal subscriptions are stored
  and dispatched via `inject_signal`, enabling test-driven NameOwnerChanged
  simulation without a running bus.

### API

```c
ip_connection conn;
ip_connection_init(&conn, ip_dbus_sd_backend());   /* production */
// ip_connection_init(&conn, mock_backend);        /* tests */

ip_connection_set_reenumerate_cb(&conn, on_reenumerate, NULL);
ip_connection_set_degraded_cb(&conn, on_degraded, NULL);

int rc = ip_connection_connect(&conn);
// rc == 0                  → connected, version available
// rc == IP_ERR_SERVICE_UNKNOWN → degraded mode, waiting for daemon
// rc == IP_ERR_ACCESS_DENIED   → permission error (see guidance)
// other                     → fatal error

// ... later, on NameOwnerChanged:
// ip_connection_handle_name_changed() is called internally
// by the signal callback, updating state and firing user callbacks.
```

## Object Enumeration (Task 10)

### GetManagedObjects

`org.freedesktop.DBus.ObjectManager.GetManagedObjects()` at the root path
(`/org/shadowblip/InputPlumber`) returns all managed objects in one call.
The reply has DBus signature `a{oa{sa{sv}}}` — a dict mapping object paths
to dicts of interface names to property dicts.

### Text format

Both the production sd-bus backend and the mock backend serialise the
reply into a text representation consumed by a single parser:

```
# one line per managed object
<object_path>\t<iface1>,<iface2>,...
# comment lines start with '#'
# (empty lines are ignored)
```

The production backend (`sd_get_managed_objects` in `dbus_client.c`)
iterates the `sd_bus_message` and builds this text via `open_memstream`.
The mock backend returns a canned text fixture registered by tests via
`ip_dbus_mock_expect_ok(mock, IP_IFACE_OBJECT_MANAGER, "GetManagedObjects", fixture)`.

### Device model

The parser populates a `cbx_device_model`:

| Category | Classification | Limit |
|---|---|---|
| Manager | Interface list contains `org.shadowblip.InputManager` | 1 |
| Composite | Interface list contains `org.shadowblip.Input.CompositeDevice` | 16 |
| Source | Path contains `/devices/source/` | 64 |
| Target | Path contains `/devices/target/` | 64 |

**Security:** All object paths are validated to start with
`/org/shadowblip/InputPlumber/`.  Invalid paths are silently skipped.

### API

```c
/* Full enumeration via backend vtable. */
cbx_device_model model;
int rc = cbx_objectmanager_enumerate(conn.backend, conn.bus, &model);
// rc == 0 → success (model may be empty if InputPlumber is starting up)
// rc < 0  → backend error (e.g. IP_ERR_NO_REPLY)

/* Direct parser (for testing with fixtures). */
int rc = cbx_objectmanager_parse_reply(fixture_text, &model);

/* Lookups. */
const cbx_composite_entry *c = cbx_device_model_find_composite(&model, path);
const cbx_device_entry    *s = cbx_device_model_find_source(&model, path);
const cbx_device_entry    *t = cbx_device_model_find_target(&model, path);
```

### Re-enumeration hook

`ip_connection` fires its `reenumerate_cb` when InputPlumber's bus name is
(re-)acquired (see Connection Model above).  The caller should invoke
`cbx_objectmanager_enumerate()` from this callback to refresh the device
model after daemon restart or initial startup.

## Signal Handling (Task 11)

### Hotplug — InterfacesAdded / InterfacesRemoved

Controller-Box subscribes to ObjectManager `InterfacesAdded` and
`InterfacesRemoved` signals at the root path to keep the device model in
sync with InputPlumber's object tree without full re-enumeration on every
device change.

| Signal | Signature | Action |
|---|---|---|
| `InterfacesAdded` | `oa{sa{sv}}` | Classify path (Manager/Composite/Source/Target), add to model |
| `InterfacesRemoved` | `oas` | Remove matching entry from model by path + interface |

**Security:**
- Sender verification: the signal's sender unique name must match
  InputPlumber's tracked unique bus name.  Mismatched senders are silently
  dropped.
- Path validation: object paths must start with
  `/org/shadowblip/InputPlumber/` to be accepted.

**Device model mutation:**

```c
ip_hotplug hp;
ip_hotplug_init(&hp, conn.backend, conn.bus,
                 conn.unique_name, &model);
ip_hotplug_subscribe(&hp);
// Signals are dispatched via the vtable's subscribe_signal callback.
// On InterfacesAdded: ip_hotplug_handle_added() classifies and adds.
// On InterfacesRemoved: ip_hotplug_handle_removed() removes by path.
```

### PropertiesChanged

Controller-Box subscribes to `org.freedesktop.DBus.Properties.PropertiesChanged`
to receive updates for tracked properties:

| Property | Interface | Type | Max element length |
|---|---|---|---|
| `GamepadOrder` | Manager | `as` (string array) | 256 (name) |
| `ProfileName` | CompositeDevice | `s` (string) | 256 (name) |
| `ProfilePath` | CompositeDevice | `s` (string) | 4096 (path) |
| `TargetDevices` | CompositeDevice | `as` (string array) | 256 (name) |
| `SourceDevicePaths` | CompositeDevice | `as` (string array) | 4096 (path) |

**Validation:**
- **Type validation:** the variant type must match the expected type for
  the property (e.g., `ProfileName` expects a string; `GamepadOrder` expects
  an array).  Type mismatches are silently rejected.
- **String length limits:** names max 256 bytes, paths max 4096 bytes.
- **Array size limits:** max 256 elements per array property.
- **Invalidated properties:** `PropertiesChanged` can include an
  invalidated-properties list.  Invalidated tracked properties are dispatched
  with `IP_PROP_TYPE_INVALIDATED` and a NULL value.

**Note:** `InterceptMode` does NOT emit `PropertiesChanged` (gap #1, §10.3).
The GUI polls it separately at 50 ms intervals (DEC-002; see the
InterceptMode polling section below for details).

```c
ip_properties props;
ip_properties_init(&props, conn.backend, conn.bus,
                    conn.unique_name, on_prop_changed, userdata);
ip_properties_subscribe(&props);

void on_prop_changed(const char *prop_name, ip_prop_type type,
                      const char *value, int count, void *ud) {
    // prop_name: "GamepadOrder", "ProfileName", etc.
    // type: IP_PROP_TYPE_STRING, IP_PROP_TYPE_ARRAY, or IP_PROP_TYPE_INVALIDATED
    // value: string value or comma-separated array elements (NULL if invalidated)
    // count: array element count (-1 if invalidated)
}
```

## Manager Interface Wrappers (Task 12)

Thin wrappers for the InputPlumber Manager interface
(`org.shadowblip.InputManager`) at `/org/shadowblip/InputPlumber/Manager`.
All wrappers go through the vtable and return categorized error codes.

### Method calls

| Wrapper | DBus method | Signature | Returns |
|---|---|---|---|
| `ip_manager_create_target_device` | `CreateTargetDevice` | `s` | path string (heap-allocated) |
| `ip_manager_stop_target_device` | `StopTargetDevice` | `s` | void |
| `ip_manager_attach_target_device` | `AttachTargetDevice` | `ss` | void |
| `ip_manager_set_target_devices` | `SetTargetDevices` (CompositeDevice iface) | `as` | void |

### Property access

| Wrapper | DBus property | Type | Access |
|---|---|---|---|
| `ip_manager_get_gamepad_order` | `GamepadOrder` | `as` | read (comma-separated paths) |
| `ip_manager_set_gamepad_order` | `GamepadOrder` | `as` | write (with device model validation) |
| `ip_manager_get_supported_target_device_ids` | `SupportedTargetDeviceIds` | `as` | read (comma-separated IDs) |
| `ip_manager_get_supported_target_devices` | `SupportedTargetDevices` | `as` | read (comma-separated names) |

### GamepadOrder validation

The `ip_manager_set_gamepad_order` wrapper validates every path in the
comma-separated value against the device model before calling InputPlumber.
If any path does not correspond to a known composite device
(`cbx_device_model_find_composite`), the wrapper returns `-EINVAL`
without making the DBus call.  This prevents the GUI from sending
invalid paths that could cause InputPlumber to suspend all devices
indefinitely.

### Calling convention for method calls

The vtable's `call_method` uses a variadic calling convention:

- `sig` encodes input argument types (`s` = string, `as` = string array
  passed as a comma-separated string)
- The last variadic argument is always a `char **out_value`: `NULL` for
  void methods, a valid pointer for methods that return a string
- The mock backend counts input args from `sig`, skips them, and fills
  `*out_value` from the canned expectation's value string
- The production backend builds a `sd_bus_message` from the input args,
  calls the method, and reads the reply string into `*out_value`

```c
/* Create a virtual controller. */
char *path = NULL;
int rc = ip_manager_create_target_device(conn.backend, conn.bus,
                                           "xb360", &path);
// rc == 0, path = "/org/shadowblip/InputPlumber/devices/target/gamepad0"
free(path);

/* Get GamepadOrder. */
char *order = NULL;
rc = ip_manager_get_gamepad_order(conn.backend, conn.bus, &order);
// rc == 0, order = "/org/.../CompositeDevice0,/org/.../CompositeDevice1"
free(order);

/* Set GamepadOrder (validated against device model). */
rc = ip_manager_set_gamepad_order(conn.backend, conn.bus, order, &model);
// rc == 0, or -EINVAL if any path is not in the model
```
## CompositeDevice Interface Wrappers (Task 13)

### Overview

Thin wrappers around the `org.shadowblip.Input.CompositeDevice` interface at
per-device paths `/org/shadowblip/InputPlumber/CompositeDevice{N}`. All
wrappers go through the `ip_dbus_backend` vtable for testability.

### Method calls

| Wrapper | DBus method | Signature | Returns |
|---|---|---|---|
| `ip_composite_set_intercept_activation` | `SetInterceptActivation` | `ass` | void |
| `ip_composite_load_profile_path` | `LoadProfilePath` | `s` | void |
| `ip_composite_load_profile_from_yaml` | `LoadProfileFromYaml` | `s` | void |
| `ip_composite_get_profile_yaml` | `GetProfileYaml` | `` | string |
| `ip_composite_set_target_devices` | `SetTargetDevices` | `as` | void |
| `ip_composite_stop` | `Stop` | `` | void |

### Property access

| Wrapper | Property | Type | Access |
|---|---|---|---|
| `ip_composite_get_intercept_mode` | `InterceptMode` | `u` | read |
| `ip_composite_set_intercept_mode` | `InterceptMode` | `u` | write |
| `ip_composite_get_target_devices` | `TargetDevices` | `as` | read |
| `ip_composite_get_source_device_paths` | `SourceDevicePaths` | `as` | read |
| `ip_composite_get_persistent_id` | `PersistentId` | `s` | read |
| `ip_composite_get_name` | `Name` | `s` | read |
| `ip_composite_get_profile_name` | `ProfileName` | `s` | read |
| `ip_composite_get_profile_path` | `ProfilePath` | `s` | read |
| `ip_composite_get_capabilities` | `Capabilities` | `as` | read |
| `ip_composite_get_output_capabilities` | `OutputCapabilities` | `as` | read |
| `ip_composite_get_target_capabilities` | `TargetCapabilities` | `as` | read |
| `ip_composite_get_dbus_devices` | `DbusDevices` | `as` | read |

### Production backend: uint32 property support

`InterceptMode` is a `u` (uint32) property. The production `sd_set_property`
implementation detects uint properties via `sd_is_uint_property()` and builds
a variant `"u"` by parsing the string value to `unsigned long` and appending
via `sd_bus_message_append_basic(m, 'u', &uval)`.

### InterceptMode polling (gap #1 workaround)

`InterceptMode` does NOT emit `PropertiesChanged` (gap #1). The GUI must poll
the property at 50ms intervals (DEC-002) to detect mode transitions.

**State machine:**

```
IDLE → start() → PASS_WAIT (polling at 50ms, PASS expected)
                 → detect ALL/GAMEPAD_ONLY → fire activating_cb → ACTIVE
ACTIVE → detect PASS/NONE → fire deactivating_cb → IDLE
        → timeout (mode stuck at ALL) → fire error_cb → IDLE
```

**SDL integration:** `ip_intercept_poll_start()` creates an `SDL_AddTimer`
(50ms interval). The timer callback pushes a custom `SDL_UserEvent` onto the
event queue. The main event loop calls `ip_intercept_poll_tick()` when it
sees the custom event.

**Timeout handling:**
- In `PASS_WAIT`: if InterceptMode is `NONE` (unexpected reset) for
  `max_timeout_ticks` consecutive ticks, fire error callback and reset to IDLE.
- In `ACTIVE`: if InterceptMode stays at `ALL`/`GAMEPAD_ONLY` for
  `max_timeout_ticks` consecutive ticks (GUI set PASS but InputPlumber didn't
  switch), fire error callback with `-ETIMEDOUT` and reset to IDLE.
- Consecutive property read errors: after `max_errors` (default 5), fire
  error callback and reset to IDLE.
- Parse failures (invalid mode string): treated as transient errors, count
  toward `max_errors`.

**API example:**

```c
ip_intercept_poll poll;
ip_intercept_poll_init(&poll, conn.backend, conn.bus,
    "/org/shadowblip/InputPlumber/CompositeDevice0",
    on_activating, &overlay_data,
    on_deactivating, &overlay_data,
    on_error, &error_data);

uint32_t event_type = SDL_RegisterEvents(1);
ip_intercept_poll_start(&poll, IP_INTERCEPT_POLL_INTERVAL_MS, event_type);

/* In main event loop, when event_type is received: */
ip_intercept_poll_tick(&poll);

/* To stop polling: */
ip_intercept_poll_stop(&poll);
```

### DbusDevices correlation

The `DbusDevices` property returns the object paths of `DBusDevice` objects
associated with a composite device. When `InterceptMode` is `ALL` or
`GAMEPAD_ONLY`, input is routed to these `DBusDevice` objects and emitted as
`InputEvent` signals. Task 14 uses this property to discover `DBusDevice`
object paths per composite and subscribe to `InputEvent` signals.

## Source/Target Device Properties and InputEvent (Task 14)

### Source device interfaces

Source devices expose identification properties on per-type interfaces:

| Interface | Properties |
|-----------|-----------|
| `org.shadowblip.Input.Source.EventDevice` | `Name`, `PhysPath`, `IdVendor`, `IdProduct`, `UniqueId`, `IdBustype` |
| `org.shadowblip.Input.Source.UdevDevice` | `Name`, `PhysPath`, `IdVendor`, `IdProduct`, `UniqueId`, `IdBustype` |
| `org.shadowblip.Input.Source.HIDRawDevice` | `Name`, `SerialNumber`, `IdVendor`, `IdProduct`, `Manufacturer`, `Product` |

**Note:** serial is `UniqueId` on evdev/udev but `SerialNumber` on HIDRaw.
The caller must pass the correct interface for the device type.

Wrappers (`ip_source.h`):

```c
int ip_source_get_name(backend, bus, source_path, iface, &out);
int ip_source_get_unique_id(backend, bus, source_path, iface, &out);
int ip_source_get_phys_path(backend, bus, source_path, iface, &out);
int ip_source_get_id_vendor(backend, bus, source_path, iface, &out);
int ip_source_get_id_product(backend, bus, source_path, iface, &out);
int ip_source_get_id_bustype(backend, bus, source_path, iface, &out);
int ip_source_get_serial_number(backend, bus, source_path, iface, &out);
```

All wrappers take `iface` as a parameter since the source interface varies
by device type (EventDevice, UdevDevice, HIDRawDevice).

### Target device interface

Target devices expose display properties on `org.shadowblip.Input.Target`:

| Property | Type | Purpose |
|----------|------|---------|
| `Name` | `s` | Display name |
| `DeviceType` | `s` | Icon-mapping key (e.g. `"xb360"`, `"ds5"`, `"deck"`) |

Wrappers (`ip_target.h`):

```c
int ip_target_get_name(backend, bus, target_path, &out);
int ip_target_get_device_type(backend, bus, target_path, &out);
```

### InputEvent signal handling

The `org.shadowblip.Input.DBusDevice` interface emits `InputEvent(event: s,
value: d)` during intercept mode. The handler (`ip_input_signal.h`):

1. **Sender verification** — verifies the signal sender matches
   InputPlumber's unique bus name.
2. **Event string parsing** — parses the event string into a normalized
   `ip_input_id` enum. Unknown events are silently dropped.
3. **Value validation** — buttons must be `0.0` or `1.0`; axes must be in
   `[-1.0, 1.0]`. NaN/infinity are rejected.
4. **Rate limiting** — max 200 events/second per device path (sliding
   1-second window). Events over the limit are silently dropped.
5. **Dispatch** — fires the user callback with parsed input, category,
   validated value, raw event string, and device path.

#### Normalized input enum

| InputPlumber event string | `ip_input_id` | Category |
|--------------------------|---------------|----------|
| `Up`, `Down`, `Left`, `Right` | `IP_INPUT_UP/DOWN/LEFT/RIGHT` | Button |
| `A`, `B`, `X`, `Y` | `IP_INPUT_A/B/X/Y` | Button |
| `Start`, `Select`, `Back`, `Guide`, `Home` | `IP_INPUT_START/SELECT/GUIDE` | Button |
| `L1`, `R1`, `LeftBumper`, `RightBumper` | `IP_INPUT_L1/R1` | Button |
| `L2`, `R2`, `LeftTrigger`, `RightTrigger` | `IP_INPUT_L2/R2` | Button |
| `L3`, `R3`, `LeftStick`, `RightStick` | `IP_INPUT_L3/R3` | Button |
| `LeftStickX/Y`, `RightStickX/Y` | `IP_INPUT_LEFT/RIGHT_STICK_X/Y` | Axis |

Unknown event strings map to `IP_INPUT_UNKNOWN` and are dropped.

#### Production backend

The production sd-bus backend (`dbus_client.c`) parses `InputEvent` signals
with signature `(sd)` using `sd_bus_message_read(msg, "sd", &event, &value)`.
Sender and path are extracted from the message via `sd_bus_message_get_sender`
and `sd_bus_message_get_path`.

## CreateCompositeDevice Temp File Workaround (Task 15, gap #3)

InputPlumber's `Manager.CreateCompositeDevice(config_path: s) → s` requires a
YAML file path — there is no string-based variant on DBus (SPEC §10.3 gap #3).
The workaround writes a temp file and passes its path:

### API

```c
int ip_create_composite_device(const ip_dbus_backend *backend,
                                 ip_bus_handle bus,
                                 const char *yaml_content,
                                 char **out_path);
```

### Behavior

1. Resolves temp directory: prefers `XDG_RUNTIME_DIR`, falls back to `/tmp`.
2. Creates temp file via `mkstemps()` (glibc) or `mkstemp()` (fallback) with
   mode 0600 via `fchmod()`.
3. Writes the YAML content to the temp file, `fsync()`, and closes it.
4. Calls `Manager.CreateCompositeDevice` with the temp file path.
5. **Always unlinks the temp file** — regardless of success or failure.

### Security

- The temp file name is generated by `mkstemps`/`mkstemp` (unique,
  unpredictable).
- File mode is 0600 (owner read/write only).
- No user-controlled paths are passed to `CreateCompositeDevice` — only
  the temp file path that this function created.
- User-controlled YAML content is written to the file but the user never
  controls the file path.

## GamepadOrder Persistence Layer (Task 15, gap #2)

InputPlumber's `Manager.GamepadOrder` property is in-memory only and resets to
empty on daemon restart (SPEC §10.3 gap #2). The persistence layer saves the
order to `assignments.yaml` and provides a load function for restoration:

### API

```c
int ip_gamepad_order_save(const ip_dbus_backend *backend,
                           ip_bus_handle bus,
                           const cbx_device_model *model,
                           const char *paths_csv);

int ip_gamepad_order_load(char **out_csv);
```

### Save flow

1. Loads existing `assignments.yaml` (preserves assignment entries).
2. Clears the existing `gamepad_order` array (replaces, not appends).
3. Iterates comma-separated composite device paths:
   - Verifies each path exists in the device model (**stale paths skipped**).
   - Queries `PersistentId` via `ip_composite_get_persistent_id()`.
   - If the query fails, the entry is **skipped** (not an error).
   - Validates the ID via `cbx_validate_id()` (skips invalid IDs).
   - Deduplicates (same ID not added twice).
4. Saves `assignments.yaml` atomically (mkstemp + rename, mode 0600).

### Load flow

1. Loads `assignments.yaml`.
2. Builds a CSV string from the `gamepad_order` entries.
3. Skips entries that fail `cbx_validate_id()` validation.

### Orchestration

The save/load functions are the **persistence layer only**. The orchestration
of when to save (on every GamepadOrder change) and when to restore (after
daemon restart, mapping IDs back to composite paths via the device model) is
handled by the identity downgrade detection layer (Task 27). The ID-to-path
mapping on restore requires querying `PersistentId` for each composite in
the device model and matching against saved IDs.

## Five DBus Gaps Summary

All five InputPlumber DBus API gaps confirmed during implementation. Each has
a workaround sufficient for v1; no upstream changes are required.

| # | Gap | Workaround | Where Documented |
|---|-----|------------|------------------|
| 1 | No `PropertiesChanged` signal for `InterceptMode` | Poll at 50 ms interval (DEC-002); state machine with timeout handling | CompositeDevice section above |
| 2 | `GamepadOrder` not persisted (in-memory only, resets on restart) | Save to `assignments.yaml` keyed by `PersistentId`; re-apply after restart | GamepadOrder Persistence section above |
| 3 | `CreateCompositeDevice` requires YAML file path (no string variant) | Write temp YAML via `mkstemp` (mode 0600), pass path, unlink after call | CreateCompositeDevice section above |
| 4 | No DBus method to enumerate profiles/configs/capability maps on disk | Read filesystem directly: `~/.local/share/inputplumber/profiles/`, `/usr/share/inputplumber/profiles/`, `/usr/share/inputplumber/devices/`, `/usr/share/inputplumber/capability_maps/` | PACKAGING.md (Flatpak filesystem permissions) |
| 5 | No DBus method to add/remove source devices on running composites | Not needed for v1; InputPlumber auto-manages composites from device configs | SPEC §12 (out of scope) |

## Object Tree

```
/org/shadowblip/InputPlumber
├── Manager                        (org.shadowblip.InputManager)
│   ├── CreateTargetDevice
│   ├── StopTargetDevice
│   ├── AttachTargetDevice
│   ├── CreateCompositeDevice
│   ├── GamepadOrder (rw)
│   ├── SupportedTargetDeviceIds (r)
│   ├── SupportedTargetDevices (r)
│   ├── Version (r)
│   └── ManageAllDevices (rw)
├── CompositeDevice{N}             (org.shadowblip.Input.CompositeDevice)
│   ├── SetInterceptActivation
│   ├── InterceptMode (rw, no change signal — gap #1)
│   ├── LoadProfilePath / LoadProfileFromYaml
│   ├── GetProfileYaml
│   ├── ProfileName / ProfilePath (r)
│   ├── SetTargetDevices / TargetDevices (rw)
│   ├── SourceDevicePaths (r)
│   ├── PersistentId (r)
│   ├── Name / Capabilities / OutputCapabilities / TargetCapabilities (r)
│   ├── Stop()
│   ├── SendEvent / SendButtonChord
│   ├── DbusDevices (r)
│   ├── FilteredEvents / FilterableEvents (r)
│   └── DBusDevice objects       (org.shadowblip.Input.DBusDevice)
│       └── InputEvent(event: s, value: d)
├── devices/source/{...}           (org.shadowblip.Input.Source.EventDevice |
│                                   org.shadowblip.Input.Source.UdevDevice |
│                                   org.shadowblip.Input.Source.HIDRawDevice)
└── devices/target/{...}           (org.shadowblip.Input.Target)
    ├── Name (r)
    ├── DeviceType (r)
    └── .Gamepad / .Keyboard / .Mouse / .Touchscreen interfaces
