# Implementation Scratchpad — Controller-Box v1

## Task 1 complete: Boolean DBus property SET native type fidelity

- **Commit:** f21831e on develop
- **What:** Added `sd_is_bool_property()` branch to `sd_set_property()` in `src/dbus/dbus_client.c` — writes native `v<b>` variant for `ManageAllDevices` and `Enabled` instead of falling through to string `v<s>`. Parses `"1"/"true"/"yes"` → 1, `"0"/"false"/"no"` → 0, rejects other values with `-EINVAL`.
- **Test server:** Added `ManageAllDevices` as a writable boolean (`SD_BUS_WRITABLE_PROPERTY`) on the Manager vtable in `tests/native_ip_server.c` with GET/SET handlers and global `g_nip_manage_all_devices`. Reset in `nip_reset_server_state`.
- **Tests:** New `test_native_boolean_property_set` in `test_native_dbus.c` — round-trip SET/get for true, false, word form, and invalid value rejection. Added `ManageAllDevices` assertion to `test_dbus_signatures.c`.
- **Verification:** `ctest --test-dir build-check -R 'test_dbus_signatures|test_native_dbus'` → 2/2 pass. Full suite: 96/96 pass (1 pre-existing skip).
- **Conformance:** DB-01 updated — boolean SET now native; remaining DB-01 gap is sender verification (Task 2).

## Next task

Task 2: Add sender verification for DBus InterfacesAdded/Removed signals.