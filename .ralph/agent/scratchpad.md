# Implementation Scratchpad — Controller-Box v1

## Task 2 complete: Sender verification for InterfacesAdded/Removed signals

- **Commit:** 3889ec0 on develop
- **What:** Added `sd_sender_ok()` helper to `dbus_client.c` — verifies signal sender against InputPlumber's tracked unique bus name before processing in `sd_interfaces_added_callback` and `sd_interfaces_removed_callback`. Spoofed signals are silently dropped (return 0).
- **Mechanism:**
  - `sd_bus_wrapper` gains `expected_sender` field, set in `sd_get_unique_name()` and updated in `sd_noc_callback()` on NameOwnerChanged (IP restart)
  - `sd_signal_data` gains `wrapper` pointer for callback access
  - `sd_sender_ok()`: rejects mismatched senders when `expected_sender` is non-NULL; allows through when NULL (degraded mode — downstream `ip_hotplug sender_ok()` still rejects)
  - Freed in `sd_disconnect()`
- **Tests:** Added `test_inject_added_wrong_sender` and `test_inject_removed_wrong_sender` to `test_hotplug.c` — inject spoofed signals through `inject_signal`, verify model unchanged.
- **Verification:** `ctest --test-dir build-check -R test_hotplug` → pass. Full suite: 96/96 pass (1 pre-existing skip).
- **Conformance:** DB-01 updated from partial to verified — sender verification now in both sd-bus callbacks (defense-in-depth) and downstream `ip_hotplug`/`ip_properties` handlers.

## Next task

Task 3: Implement real overlay lifecycle in installed tests and compositor-visible overlay activation.