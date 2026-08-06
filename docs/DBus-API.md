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
The GUI polls it separately (~500 ms interval).

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