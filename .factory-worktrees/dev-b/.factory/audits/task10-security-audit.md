# Security Audit — Task 10: Repair interception polling ownership and event hygiene

Scope: interception-poll lifecycle (`src/dbus/ip_intercept_poll.c/.h`), overlay
service step loop and poll arming/rearm paths (`src/app/overlay_service.c`),
event dispatch (`src/dbus/ip_input_signal.c`, `src/dbus/dbus_client.c`),
and the input paths that feed them (hotplug/properties signals, device model,
trigger/assignment writes).

Reviewed against the current `develop` tree. The production binary links
`controllerbox` **without** `CBX_TESTING` (verified in `CMakeLists.txt`),
so the test-exported callbacks (`on_intercept_*`) remain static in release
builds — no test-only production bypass found.

## Positive observations (no action required)

- **DBus sender trust is layered and fail-closed**: transport-level
  `sd_sender_ok()` (`src/dbus/dbus_client.c`) rejects all signals unless a
  credential-verified owner is published via `GetConnectionCredentials` +
  UID policy (`sd_trust_owner_creds`), and handler-level checks in
  `ip_input_signal.c`, `ip_hotplug.c`, `ip_properties.c` each re-verify the
  sender. Name loss / failed verification clears trust (`sd_clear_trusted_owner`),
  so no name-squatting window exists. The intercept-poll `get_property` reads
  are method calls to a fixed destination, which sd-bus reply-sender-checks.
- **No unsafe string/memory primitives** in the polling paths: all copies use
  bounded `snprintf`/`strncpy`+NUL with explicit truncation guards;
  `cbx_overlay_on_save`'s stack `order[]` build is length-checked before each
  `memcpy` (`src/app/overlay_service.c`); `sd_read_string_array_csv`
  (`src/dbus/dbus_client.c`) reallocs with correct accounting; `parse_intercept_mode`
  range-checks the external property string to 0..255.
- **Assignment persistence** (`cbx_assignments_save`, `src/config/config_assignments.c`)
  uses `mkstemp` + `fchmod(0600)` + `rename` in the same directory — atomic,
  no symlink/tmp predictability issue.
- **Bounded drains** in the step loop (64 process iterations, `IP_INPUT_DRAIN_MAX`)
  prevent a signal flood from starving the UI; DBus processing errors degrade
  fail-closed instead of being swallowed.
- **Generation/ownership validation** of queued poll events
  (`poll_event_targets_live_poll`) correctly rejects stale events after
  stop/rearm/rebuild, and the fixed-array `overlay_stop_all_polls` prevents
  timer leaks on partial rearm failure and shutdown.

## Findings

### 1. WARN — Timer-callback lifetime race at shutdown (and rearm memset)

**Files**: `src/dbus/ip_intercept_poll.c` (`sdl_timer_cb`, lines ~33–50;
`ip_intercept_poll_init` memset, line ~105; `ip_intercept_poll_stop`,
line ~160), `src/app/overlay_service.c` (shutdown sequence, lines 2252–2266).

`SDL_RemoveTimer` only sets a cancellation flag — it does **not** wait for a
callback currently executing on the SDL timer thread (verified in the SDL3
timer implementation shipped via sdl2-compat: `src/timer/SDL_timer.c` invokes
the callback with no lock held, and `SDL_RemoveTimer` returns after flipping
`canceled`). Consequently:

- At shutdown, `overlay_stop_all_polls(svc)` (line 2254) can return while
  `sdl_timer_cb` is still running and dereferencing `poll` (inside the heap
  `svc`). `free(svc)` at line 2265 then makes that an in-flight use-after-free
  read of the atomic `sdl_event_type`/`generation` fields. The process never
  calls `SDL_Quit()` (which would join the timer thread via
  `SDL_QuitTimers`/`SDL_WaitThread`), so nothing closes this window.
- During rearm, `ip_intercept_poll_init`'s `memset(poll, 0, sizeof(*poll))`
  (line ~105) can run concurrently with the previous arm's in-flight callback
  `atomic_load`s — a formal data race (non-atomic memset vs. atomic load).
  Both loads are of naturally aligned 32-bit atomics, so this is benign in
  practice, but it is still UB by the memory model.

Impact is limited to a rare shutdown-time crash / UB-sanitizer report; there is
no attacker-controlled input path to the SDL event queue (process-local), and
the pushed stale event itself is already neutralized by the generation check.
The task acceptance criterion asserts "no lifetime race," so this residual
window should be closed rather than argued away.

**Recommendation**: at the end of `run_overlay_service`, call `SDL_Quit()`
(which shuts down and joins the SDL timer thread) *before* `free(svc)` — or
simply leak `svc` at process exit (it is the final statement before `return`).
For the init path, move the generation read/`SDL_RemoveTimer` dance so the
memset cannot interleave with a live callback (e.g., rely on stop-before-init
ordering and document it, or add an SDL-side drain).

### 2. WARN — Hotplug reconcile ignores poll-rearm failure

**File**: `src/app/overlay_service.c`, `cbx_overlay_reconcile_hotplug`,
line 1278.

`cbx_overlay_rearm_polls(svc);` is called without checking its return value,
unlike the fail-closed treatment in `overlay_wire_required_steps`
(line 1399) which records the failure and keeps operations disabled. If
rearm fails on the hotplug path (e.g. `SDL_AddTimer` failure or
`poll_event_type == (uint32_t)-1`), the function still returns 0: the step
loop treats reconciliation as successful and the service stays
`backend_ready`, but every intercept poll has been torn down
(`overlay_stop_all_polls` in the failure path).

Security consequence: the trigger stays registered
(`SetInterceptActivation` was just re-applied a few lines above), so the next
trigger press flips InterceptMode to ALL — input is intercepted — but no poll
detects the activation, the overlay never opens, and nothing ever restores
PASS. The user's controller input is captured with no visible surface to close,
i.e. a self-inflicted input DoS until InputPlumber restart. This directly
undermines the task's invariant that poll ownership and trigger registration
stay consistent.

**Recommendation**: check the return of `cbx_overlay_rearm_polls` in
`cbx_overlay_reconcile_hotplug` and on failure record the readiness detail and
return the error so the caller degrades and schedules bounded recovery —
mirroring `overlay_wire_required_steps`.

### 3. WARN — InputEvent rate limiter fails open and is never recycled in production

**File**: `src/dbus/ip_input_signal.c`, `find_rate_limiter`/
`rate_limit_check` (lines ~130–180); `IP_INPUT_MAX_DEVICES == 64`.

When the per-device rate-limiter table is full and the incoming `device_path`
is not already present, `rate_limit_check` explicitly **allows** the event
(`return true; /* can't track — allow the event */`, line 178). The table is
never trimmed: `ip_input_events_reset_rate_limiters` has no production caller
(only `tests/test_input_signal.c`), and hotplug reconciliation rebuilds the
input map without resetting the limiter table. A controller session with
device churn (or one buggy InputPlumber emitting many distinct paths) can
permanently consume all 64 slots, after which every subsequent event from new
devices bypasses the 200 events/s limit entirely.

The exposure is bounded by sender verification (only the credential-verified
InputPlumber connection is accepted), so this degrades a flood-hygiene control
rather than creating an external attack path — but the control then silently
fails open exactly when a compromised/buggy InputPlumber would need it.

**Recommendation**: evict entries whose 1-second window expired long ago
(the window fields already exist), or call
`ip_input_events_reset_rate_limiters` when the input map is rebuilt in
`cbx_overlay_reconcile_hotplug` / `overlay_recover`. Fail closed (or
least-recently-used eviction) instead of unconditionally allowing when full.

### 4. INFO — DBusDevice path mapping silently truncates at 255 bytes

**File**: `src/app/overlay_service.c`, `cbx_overlay_input_add_mapping`
(lines ~1638–1650); `cbx_overlay_input_ctx.device_paths[][256]` in
`src/app/overlay_service.h`.

Device paths from the `DbusDevices` property are copied with `snprintf` into
fixed 256-byte slots. A path longer than 255 bytes is silently truncated; a
truncated entry can then collide with a genuinely different path prefix, so
InputEvent signals from one DBusDevice could be dispatched to another
controller's grid row (input cross-drive). Paths come from the
sender-verified InputPlumber and DBus object paths are normally well under
this limit, so this is a robustness note.

**Recommendation**: check the `snprintf` return for truncation and reject
over-long paths (dropping the mapping fails closed — unknown paths are
already dropped at dispatch), or use `CBX_MAX_PATH_LEN` consistently with the
rest of the model.

### 5. INFO — Narrow stale-arm extra-tick window in `ip_intercept_poll_start`

**File**: `src/dbus/ip_intercept_poll.c`, `sdl_timer_cb` + `ip_intercept_poll_start`.

An in-flight callback of the just-removed previous timer can observe the new
`generation` (bumped before `SDL_AddTimer`) and push an event that the step
loop accepts, ticking the newly armed poll once, off-schedule (~one extra
DBus read, violating the strict "ticks only its live originating arm"
invariant for that one event). The window is nanoseconds, cannot be triggered
by external input, and the extra tick is a harmless property read. A complete
fix would require the callback to re-validate generation after push, which is
not possible with the async SDL event API. Documenting the bounded
one-extra-read residual is acceptable; noting it here so the invariant claim
in the acceptance notes is not overstated.

## Verdict

No BLOCKER findings: no attacker-reachable memory corruption, injection,
privilege escalation, or path traversal was found in the interception polling
and event-hygiene paths. The DBus trust model (credential-verified owner,
per-handler sender re-checks, fail-closed degradation) is sound. The three
WARN findings (shutdown timer-callback UAF, unchecked hotplug rearm failure,
fail-open rate limiter) should be addressed in a follow-up round; finding 2
is the one that most directly contradicts the task's acceptance invariants.