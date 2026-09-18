# Task 16 Security Audit — Native physical capture → portable mappings

Scope: commit `e94828aa` (task 16) — `src/manager/controllers_tab.c/.h`,
`src/manager/manager.c`, `src/manager/profiles_tab.c/.h`,
`src/manager/profile_editor_list.c/.h`, `src/manager/profile_editor_seq.c`,
plus the DBus plumbing it rides on (`src/dbus/ip_input_signal.c`,
`src/dbus/ip_composite.c`, `src/dbus/dbus_client.c`).

## What is solid

- **Buffer safety (new code)**: every new copy is bounded — `snprintf` for
  `composite_path`/`prior_intercept_mode`/`expected_sender`,
  `strncpy` + explicit NUL termination for captured `raw_event` and the
  sequential target event. `parse_dbus_devices_csv`
  (src/manager/profile_editor_list.c:365) re-checks `len < sizeof(buf)`
  before `memcpy` + NUL; no overflow found. `cbx_controllers_tab_selected_composite`
  bounds the selected-device index against `target_count` before indexing.
- **DBus sender authentication**: `ip_input_events_handle`
  (src/dbus/ip_input_signal.c:246) rejects a NULL/empty `expected_sender`
  (fail-closed), and `sd_input_event_callback` +
  `sd_sender_ok`/`sd_query_owner_creds` (src/dbus/dbus_client.c:68, 510)
  verify the unique bus name *and* PID/EUID credentials before any payload
  is parsed. Spoofed InputEvent signals from an arbitrary bus client are
  correctly rejected at two layers.
- **CSV parsing of DBus data**: DBus object paths cannot contain `,`, space
  or tab, so the CSV parsers (`csv_contains_path`, `parse_dbus_devices_csv`)
  cannot be confused by delimiters embedded in a path element.
- **InterceptMode set path** validates strictly as uint32
  (`sd_set_property`, src/dbus/dbus_client.c:1073); the restore value
  `prior_intercept_mode[16]` cannot inject a malformed property.
- **Subscription idempotency**: `sd_subscribe_signal`
  (src/dbus/dbus_client.c:773) refreshes the existing (iface, member) slot
  instead of stacking duplicates, so repeated acquire paths cannot
  double-dispatch.
- **B/Start capture affordances**: the SDL navigation stream is consumed by
  `cbx_profiles_tab_cancel`/`handle_key`, the DBus InputEvent stream drives
  capture; the two cannot be confused, and B/Start remain bindable when
  they are the prompted button.

## Findings

### BLOCKER — Device filter fails open when the DBusDevices read fails or yields an empty list

- **Files**: `src/manager/profile_editor_list.c` —
  `event_device_accepted()` (line 395) and `cbx_profile_editor_acquire_interception()`
  (line 421, the `ip_composite_get_dbus_devices` call at line ~470).
- **Issue**: `event_device_accepted()` returns `true` for **every** device
  when `ed->dbus_device_count == 0`. That is correct for the designed
  degraded mode (no composite selected), but the count is also 0 when a
  composite **is** selected, interception **is** active (`InterceptMode=3`),
  and `ip_composite_get_dbus_devices()` failed, returned NULL, or returned
  an empty CSV — the failure is silently ignored:
  ```c
  if (ip_composite_get_dbus_devices(...) == 0 && devs)
      parse_dbus_devices_csv(ed, devs);
  free(devs);
  ```
  Additionally `parse_dbus_devices_csv` drops entries ≥ 256 chars or beyond
  32 entries; if the real composite's devices are the dropped ones, the
  filter again degenerates to count==0 → accept-all. In these states any
  DBusDevice's button press (e.g. a second controller belonging to a
  different composite) is accepted as the capture source, and sequential
  capture writes it into the profile. This violates the acceptance
  requirement "load its virtual capabilities and **reject other devices'
  events**" on the error path, silently. This codebase already
  established the required pattern as a task-10 BLOCKER
  (`cbx_overlay_input_build_map` in src/app/overlay_service.c now returns
  the first DbusDevices probe error and the overlay fails closed into a
  readable degraded state instead of proceeding open).
- **Recommendation**: in `cbx_profile_editor_acquire_interception`, a
  composite being selected makes the DBusDevices read a *required* step:
  propagate `rc != 0` and an empty parsed list as an error, restore the
  intercept mode, and abort capture/sequential with the same
  "Capture unavailable: input intercept failed" status path used for
  subscribe/set failures. Add a regression test driving a one-shot
  `get_dbus_devices` error (the native server already supports
  `g_nip_fail_next_dbus_devices` per the task-10 repair evidence) and
  assert capture refuses to start / aborts rather than accepting a foreign
  device's event.

### WARN — InterceptMode restore guesses "1" (PASS) when the prior-mode read fails

- **Files**: `src/manager/profile_editor_list.c:444-456`
  (`cbx_profile_editor_acquire_interception`), line 411-412
  (`cbx_profile_editor_release_interception`).
- **Issue**: if `ip_composite_get_intercept_mode` fails or returns empty,
  `prior_intercept_mode` is defaulted to `"1"` (PASS). On release the editor
  then *writes* PASS, so a composite that was actually in NONE (0) or ALL (2)
  is left with a different mode than it started with — a persistent,
  owner-imposed state change, not the "restore the prior interception
  ownership" the acceptance demands (and the write happens even if the
  capture itself was cancelled without changing anything else).
- **Recommendation**: when a composite is selected, a failed prior-mode
  read must fail the acquisition (abort capture, mode untouched) rather
  than guess; the read happens *before* `set_intercept_mode("3")`, so
  aborting is cheap and side-effect free.

### WARN — "Subscription ownership" is not actually released on completion (deferred task-15 WARN #3 only nominally addressed)

- **Files**: `src/manager/profile_editor_list.c` —
  `cbx_profile_editor_release_interception()` (line 406); no
  `ip_input_events_unsubscribe` exists anywhere (`rg` shows none in
  `src/`).
- **Issue**: the task-15 audit deferred "InputEvent subscription release"
  to task 16, whose acceptance owns "restore the prior
  interception/**subscription** ownership on completion, cancel, editor
  close, disconnect, shutdown and failed initialization". Task 16 restores
  the *interception* mode but never releases the *subscription*: the
  daemon-side match rule and the editor's signal callback stay armed for
  the editor's whole lifetime after every completion/cancel/close. Today
  this is benign only because (a) `sd_subscribe_signal` is an idempotent
  refresh of the same slot, and (b) `cbx_profile_editor_on_input_event`
  drops events via `mode`/`capture_active`/`seq_active` guards. But the
  acceptance clause is satisfied only nominally, and the editor continues
  to receive and parse InputEvent payloads (rate-limited, sender-checked)
  even while sitting idle in LIST mode.
- **Recommendation**: either add a real release — a backend-level
  unsubscribe (or at minimum clear `ed->input_events` on release so stale
  callbacks cannot fire into a re-entering capture) — or document in
  `profile_editor_list.h` that subscription release is defined as
  idempotent re-init on the next acquire plus mode-guarded dispatch, and
  get that sign-off explicitly. The first option is the honest
  implementation of the acceptance text.

### WARN — No ownership check on InterceptMode: release can stomp a concurrent owner (overlay service)

- **Files**: `src/manager/profile_editor_list.c:406-418`; interacts with
  `src/app/overlay_service.c` and SPEC §10.1 (InterceptMode has no
  PropertiesChanged signal — gap #1).
- **Issue**: the editor saves the composite's InterceptMode once at acquire
  and writes it back verbatim at release, without re-reading. If the
  overlay service (or any other client) switched the composite to ALL (2)
  while the editor captures, release forcibly returns it to the saved
  value, silently breaking the other owner; the reverse order means the
  editor's capture silently loses interception with no signal (gap #1) to
  detect it. There is no owner token in the DBus API, so full arbitration
  is impossible client-side, but the current code does not even detect the
  stomp.
- **Recommendation**: on release, re-read the current mode and only write
  the restore value if the composite still reports the mode this editor set
  ("3"); if it differs, leave it alone (another owner has taken the
  composite) and log the collision. At minimum document the single-writer
  assumption between the manager editor and the overlay service for the
  same composite.

### INFO — `acquire_interception` has no re-entrancy guard

- **Files**: `src/manager/profile_editor_list.c:421`.
- **Issue**: calling acquire while `intercept_active` is already true would
  save "3" (the mode this editor set) as the *prior* mode, permanently
  intercepting after release. No current path double-acquires (release runs
  before every re-arm in `set_dbus`/`reset_mode`/capture completion), so
  this is defense-in-depth only, but the invariant is implicit.
- **Recommendation**: add `if (ed->intercept_active) return 0;` at the top
  (or assert it) to make the ownership state machine self-enforcing.

### INFO — Fixed 512-byte composite path buffers silently truncate long object paths

- **Files**: `src/manager/profiles_tab.h:120` (`current_composite_path[512]`),
  `src/manager/manager.c:1502` (stack buffer),
  `src/manager/controllers_tab.c:1245` (`snprintf(out, outsz, ...)`).
- **Issue**: an object path longer than 511 chars is silently truncated and
  the truncated path is then used as the DBus target for intercept
  get/set and the DBusDevices read. DBus would reject a malformed path with
  an error (which, per BLOCKER above, must then abort capture), and real
  InputPlumber paths are far shorter, so this is an observation, not an
  exploitable path.
- **Recommendation**: none required for correctness at current path sizes;
  if hardening, refuse (return 0) when the resolved composite path does not
  fit rather than truncating.

### INFO — Blocking DBus reads on the UI thread per tab switch

- **Files**: `src/manager/controllers_tab.c:1245-1276`
  (`cbx_controllers_tab_selected_composite` loops over all composites with
  a live `ip_composite_get_target_devices` call each),
  `src/manager/manager.c:1496+` (`cbx_manager_on_tab_change`).
- **Issue**: every switch to the Profiles tab performs one synchronous
  property read per composite device; a slow or wedged system bus stalls
  the UI thread for the duration. Bounded by composite count and the
  sd-bus default timeout; a DoS-quality annoyance rather than a
  vulnerability in a single-user local app.
- **Recommendation**: prefer the already-maintained reactive
  `composites[].target_devices` cache with a single authoritative re-read
  (the pattern `csv_is_exact_singleton_path` was written for), or rate-limit
  the resolution.

## Pre-existing, not task-16 scope

- `find_rate_limiter` (src/dbus/ip_input_signal.c:131) fails **open** when
  its per-device table is full ("can't track — allow"). This is a DoS
  rate-limiter, not an authn check, and belongs to task 14; noting it so
  it is not lost. `IP_INPUT_MAX_DEVICES` bounds the table.

## Verdict

The interception acquisition/release state machine, sender authentication,
and buffer discipline are well executed, but the capture device filter —
the one control this task's acceptance names as mandatory ("reject other
devices' events") — fails open exactly when the DBusDevices read fails,
and the prior-mode restore guesses on read failure. Both must be made
fail-closed before task 16 can be considered complete; the WARNs are
quality-of-ownership improvements that the acceptance text also names.