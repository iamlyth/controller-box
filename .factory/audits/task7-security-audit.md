# Task 7 Security Audit — Make readiness and signal trust fail closed

Scope: readiness gating, owner/sender trust lifecycle, signal validation,
recovery/retry bounds, in `src/dbus/` (ip_connection, ip_input_signal,
ip_properties, ip_hotplug, dbus_client) and the two application modes
(`src/app/overlay_service.c`, `src/manager/manager.c`, profile editors).

Verification spot-check: targeted suite re-run against the current tree —
`ctest --test-dir build-t7 -R 'test_connection|test_input_signal|test_dbus_signatures|test_native_dbus|test_overlay_native|test_manager_native'`
→ 7/7 passed (note: the top-level `build/` dir is stale — its CMake cache
points at `/workspace/controller-box`, which no longer exists; binaries and
tests must be built via a fresh configure, see INFO-5).

No BLOCKER findings. Fail-closed behavior holds at the connection level
(unverified owner never published ready, credentials cleared on loss, UID
policy rejects squatters, bounded retry windows in both modes) and at the
app level (typed property validation, path/interface classification, event
allow-lists, value range checks). Two defense-in-depth gaps remain in the
transport-trust lifecycle, detailed below.

---

## WARN-1: Transport sender trust published before Version validation and not cleared when the Version check fails

**Files:** `src/dbus/ip_connection.c` (`ip_connection_handle_name_changed`,
`clear_owner_state`), `src/dbus/dbus_client.c` (`sd_get_connection_creds`,
`sd_trust_owner_creds`, `sd_sender_ok`)

**Description.** In the NameOwnerChanged re-acquisition path,
`ip_connection_handle_name_changed` calls `verify_sender(conn, new_owner)`
*before* re-reading the `Version` property. In the production backend,
`verify_sender` → `sd_get_connection_creds` → `sd_trust_owner_creds`
immediately publishes the owner into the backend trust store
(`sd_bus_wrapper.expected_sender`). If the subsequent Version read fails
(transient error) or reports an incompatible version, the code clears only
the *connection-level* state (`clear_owner_state`, `conn->sender_verified`,
`conn->unique_name`) — there is no backend vtable operation to clear the
transport-level trust store, so `sd_bus_wrapper.expected_sender` keeps
naming the owner.

Consequence: while the application reports DEGRADED (and readiness fails
closed), `sd_sender_ok` still accepts and the sd-bus callbacks still fully
parse and dispatch signals (InputEvent, PropertiesChanged,
InterfacesAdded/Removed) from a UID-trusted but version-incompatible or
unreachable-version owner. This also directly contradicts the task's
acceptance wording "do not publish transport sender trust before
validation" — Version validation is part of validation.

Note the contrast: on owner *loss* the sd backend does clear transport
trust (`sd_noc_callback` → `sd_clear_trusted_owner` when `new_owner` is
empty), and a failed credential query clears it too. Only the
credential-success-then-Version-failure path leaves it published. The
initial `ip_connection_connect` path is correctly ordered (Version read
before owner resolution/credential publication).

**Recommendation.** Either (a) reorder `ip_connection_handle_name_changed`
to read `Version` first and only then run `verify_sender` (matching the
connect path), or (b) add a backend vtable op (e.g. `clear_sender_trust`)
implemented as `sd_clear_trusted_owner` and call it from
`clear_owner_state()` so connection-level clearing always also clears the
transport store. Add a regression test asserting that a
credential-verified owner with an incompatible Version never becomes a
transport-accepted sender.

Severity rationale: the affected sender must already satisfy the UID trust
policy (root or same euid), so this is not exploitable by an untrusted
process; it is a state-consistency/defense-in-depth defect, not a
spoofing hole (the app-level expected_sender gate rejects these signals
today because the stale buffer never matches the new owner's unique name).

---

## WARN-2: Overlay degraded handler does not clear the app-level sender-trust buffer (former-owner events still pass the app-level gate)

**Files:** `src/app/overlay_service.c` (`overlay_backend_degraded`),
contrast `src/manager/manager.c` (`cbx_manager_backend_degraded`)

**Description.** On any degradation (name loss, failed recovery, DBus
processing error), the manager explicitly clears
`mgr->expected_sender[0] = '\0'` with the comment "A lost owner must not
leave a stale unique name trusted for signal sender verification." The
overlay's `overlay_backend_degraded` clears `backend_ready`, hides the
window and records a reason, but leaves `svc->expected_sender` naming the
former owner. `svc->input_events`, `svc->hp` and `svc->props` all hold a
pointer to that buffer and remain subscribed (deliberately, for
re-subscription reuse), so an event from the *former owner* still passes
`ip_input_events_handle` / `sender_ok` / `ip_hotplug` sender checks at the
application layer. In production the transport layer rejects these
signals (WARN-1's store is cleared on name loss), so this is a layered
defense gap rather than an exploitable hole — but the acceptance criterion
"reject former-owner events" is currently enforced by one layer only in
the overlay mode, and any future backend (or the injected/mock path) that
does not mirror the sd trust store would accept former-owner events.

**Recommendation.** Mirror the manager: in `overlay_backend_degraded`,
set `svc->expected_sender[0] = '\0'` (the buffer is re-populated by
`overlay_wire_required_steps` on every successful recovery, so nothing
needs re-wiring). Add a test asserting that after a name-loss
degradation, an injected signal whose sender equals the former owner's
unique name is dropped in overlay mode.

---

## INFO-1: TOCTOU between name resolution and credential verification; method replies are not sender-pinned

**Files:** `src/dbus/ip_connection.c` (`ip_connection_connect`,
`verify_sender`), `src/dbus/dbus_client.c` (`sd_get_unique_name`,
`sd_get_property`, `sd_get_managed_objects`)

`GetNameOwner` and `GetConnectionCredentials` are two separate daemon
calls; the well-known name can change owners in between, so the recorded
`unique_name`/credentials can belong to a former owner. Similarly, all
method/property traffic (`Version`, `GetManagedObjects`, property reads,
invalidation re-reads in `ip_properties.c:dispatch_invalidated_read`) is
addressed to the well-known name `org.shadowblip.InputPlumber`, so the
reply's sender is never checked against the credential-verified owner —
the anti-squatting verification applies to *signals* only. Windows are
milliseconds and a squatter must already be root/same-uid under the UID
policy, so impact is negligible; noting for completeness. A future
hardening could use `org.freedesktop.DBus.GetNameOwner` + creds in one
query (`sd_bus_get_name_creds` on the well-known name) or pin method-call
destinations to the verified unique name.

## INFO-2: Input-event rate limiter fails open when the 64-path table is full

**Files:** `src/dbus/ip_input_signal.c` (`find_rate_limiter`,
`rate_limit_check`)

When more than `IP_INPUT_MAX_DEVICES` (64) distinct device paths are seen,
`find_rate_limiter` returns NULL and `rate_limit_check` *allows* the event
("can't track — allow"). A sender that is trusted (only such senders reach
this code) could exceed the 200 Hz/device cap with ≥65 distinct paths.
The bounded drain (`IP_INPUT_DRAIN_MAX`, 64 messages/tick, plus the 64/tick
loop-drain in the service step) is the effective protection. Suggest
evicting the least-recently-used slot instead of failing open, or at least
documenting the drain bound as the actual rate limit.

## INFO-3: Version compatibility parser accepts trailing garbage and bare major bumps

**File:** `src/dbus/ip_connection.c` (`ip_version_is_compatible`)

`sscanf("%d.%d.%d")` accepts `"0.78.0-anything"`, `"0.78.abc"` (patch
parses as 0), `"1"` (major>0 ⇒ compatible), and ignores trailing text. The
string originates from the credential-verified owner, so this is hygiene,
not a hole. Use `strtod`-style end-pointer validation
(`%n` or manual parse) to reject malformed versions.

## INFO-4: Transient Version failure at name acquisition degrades until the next owner change

**File:** `src/dbus/ip_connection.c` (`ip_connection_handle_name_changed`),
`src/app/overlay_service.c` (`overlay_recovery_tick`)

If the Version read fails transiently (e.g. NoReply) during a
NameOwnerChanged acquisition, the connection clears verification and
enters DEGRADED; `overlay_recovery_tick` then stops retrying because
`ip_connection_is_connected` is false, and no further NameOwnerChanged
arrives while the same owner keeps the name. Result is correctly
fail-closed, but not self-healing until the service restarts — slightly at
odds with the "reacquisition succeeds within two seconds when the service
is healthy" goal if "healthy" is reached without a name change. Consider
re-arming the bounded retry for `IP_ERR_NO_REPLY`-class version failures.

## INFO-5: Stale top-level `build/` directory

The committed `build/` CMake cache points at `/workspace/controller-box`
(repo moved); running the task's verification command verbatim
(`ctest --test-dir build ...`) currently reports "Could not find
executable". A fresh `nix-shell --run 'cmake -S . -B build ...'` configure
is needed before the documented commands work as written from a clean
checkout. Not a code vulnerability; flagged because the recorded evidence
("verification exit 0 on local") cannot be reproduced from this state by
the literal command.

---

## Positive observations (no action required)

- `sd_sender_ok` fails closed when no trusted sender is tracked (NULL
  ⇒ reject), so spoofed signals during the degraded window are dropped
  before any message body is parsed.
- NameOwnerChanged match rule is pinned with `sender='org.freedesktop.DBus'`
  and `arg0='org.shadowblip.InputPlumber'`, so the owner-change trust
  transitions cannot be driven by arbitrary clients.
- All string handling in the audited paths uses bounded `snprintf`/`strncpy`
  with explicit NUL termination, `strtok_r`, `open_memstream`, and
  bounds-checked `memcpy` (e.g. `ip_gamepad_order.c`, `ip_manager.c` check
  `len >= sizeof(path)` first). No `sprintf`/`strcpy`/`strcat` found in
  `src/`.
- Object-path classification (`ip_hotplug.c`, `ip_objectmanager.c`) uses
  exact-prefix position checks rather than substring search, preventing
  path-confusion misclassification; composite indices parsed with
  range-checked `strtol`.
- Property changes are validated against an allow-list with interface,
  object-path class, type and length constraints
  (`ip_properties.c`), and input events against a fixed event-name
  table with finite-value and range checks
  (`ip_input_signal.c`).
- DBus drain loops in both application modes are bounded (64/tick) and
  propagate processing errors into degraded state instead of swallowing
  them; retry windows are bounded (8 attempts / 2 s / 250 ms spacing) and
  re-arming is idempotent so a per-frame failure cannot reset the budget.
- `CBX_TESTING` instrumentation is compiled only into the separate
  `controllerbox_testing` library; the production binary is built without
  it (CMakeLists.txt §189-273), so no test-only bypass ships.