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