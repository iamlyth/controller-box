# Subsystem Study Report: src/config

## Subsystem Study Report: src/config

The config subsystem implements all persistent on-disk configuration for
Controller-Box: YAML read/write for settings, controller assignments, InputPlumber
device profiles, and profile-metadata sidecars, plus XDG base-directory path
resolution. It is a pure C library layered on **libyaml** with strict security
constraints (no custom tags, max depth 50, max doc 1 MB, O_NOFOLLOW opens, atomic
mkstemp+rename writes).

All files live in `src/config/`. The subsystem is self-contained: it depends only on
`config.h` (generated install paths) and libyaml, and exposes a public `cbx_*` API
consumed broadly by the app, manager, overlay, identify, dbus, and ui modules.

---

## Module inventory

### 1. `config_paths.c` / `config_paths.h` — XDG path resolution

- **Purpose:** Resolve user config/data dirs from XDG env vars (`$HOME` fallback),
  create dirs, and expose compile-time system asset paths.
- **Public API (entry points):**
  - `cbx_resolve_config_dir(buf,size)` — `$XDG_CONFIG_HOME/controller-box` or `$HOME/.config/controller-box` (no side effects).
  - `cbx_resolve_user_profiles_dir(buf,size)` — `$XDG_DATA_HOME/inputplumber/profiles` or `$HOME/.local/share/inputplumber/profiles`.
  - `cbx_config_dir`, `cbx_user_profiles_dir` — resolve **and** `mkdir` (0700).
  - `cbx_ensure_dir(path,mode)` — recursive parent mkdir.
  - System accessors: `cbx_system_inputplumber_dir`, `cbx_system_profiles_dir`,
    `cbx_builtin_profiles_dir`, `cbx_system_devices_dir`,
    `cbx_system_capability_maps_dir`, `cbx_data_dir`, `cbx_icon_dir`, `cbx_font_dir`.
  - `cbx_font_path()` — runtime search of 8 font dirs for `DejaVuSans.ttf`
    (uses a **static** buffer; valid until next call).
- **Internal state:** `try_font_path` uses `static char font_buf[PATH_MAX]` — the
  single shared mutable buffer in the subsystem. Thread-safety of `cbx_font_path`
  relies on the caller doing no concurrent calls.
- **Error handling:** negative errno (`-ENAMETOOLONG`, `-ENOENT`, `-ENOTDIR`).
- **Fallbacks:** `cbx_builtin_profiles_dir`/`cbx_icon_dir` check installed asset
  first, then source-tree (`BUILTIN_PROFILE_DIR`/`SOURCE_PROFILE_DIR`), then a
  relative path — keeps dev builds working pre-`cmake --install`.
- **Consumers:** `app/main.c`, `overlay_service.c`, `ui/text.c` (font),
  `icons/icon_lookup.c`, `ui/theme.c`, and every config YAML module.

### 2. `config_settings.c` / `config_settings.h` — `settings.yaml` (§7.3, §5.5)

- **Purpose:** Persist app settings: `overlay_trigger`, `launch_at_boot`, `theme`,
  `overlay_opacity`, `virtual_controllers{count,types}`, and icon overrides.
- **Public API:** `cbx_settings_defaults`, `cbx_settings_load`, `cbx_settings_save`,
  `cbx_settings_validate`, `cbx_is_known_controller_type`,
  `cbx_settings_icon_override`, `cbx_settings_set_icon_override`,
  `cbx_settings_remove_icon_override`.
- **Defaults §7.3:** `overlay_trigger="Select+A"`, `launch_at_boot=true`,
  `theme="default"`, `overlay_opacity=0.85`, `count=4`, all types `xb360`.
- **Known types:** `xb360, ds5, deck, gamepad, mouse, keyboard, touchscreen`
  (`static const char *const known_types[]`).
- **Load semantics:** starts from defaults, YAML overwrites present fields, then
  `clamp_settings()` clamps opacity/count and coerces unknown/blank types to
  `DEFAULT_TYPE` (`xb360`), then pads types array to `count`.
- **Parser state machine** (`parse_settings_yaml`): tracks `in_vc_map`,
  `in_types_seq`, `in_icon_ovr_seq`, `in_icon_ovr_item`, `have_key`, `current_key`,
  `type_count`, `ovr_type_idx`, `ovr_key`. Icon override commit is a fragile hack —
  increments `icon_override_count` only when both `type` and `icon` are non-empty
  (see Issues #8).
- **Save:** validates strictly first (`cbx_settings_validate`), atomic write 0600.
  `fsync` return value is intentionally ignored as non-fatal.
- **Consumers:** `manager/settings_tab.c`, `overlay/dynamic_columns.c`,
  `overlay/grid_render.c`, `app/overlay_service`.

### 3. `config_assignments.c` / `config_assignments.h` — `assignments.yaml` (§7.4)

- **Purpose:** Persist controller→slot assignment table and persisted gamepad order
  (gap #2 workaround).
- **Public API:** `cbx_assignments_init`, `cbx_assignments_load`, `cbx_assignments_save`,
  `cbx_assignments_validate`, `cbx_validate_id`, `cbx_validate_profile`.
- **ID formats** (`cbx_validate_id`): `BT:xx:xx:xx:xx:xx:xx` (6 hex octets),
  `USB:xxxxx` (serial), `USB:phys:xxxxx`, `ORDER:n`. Profiles match
  `^[a-zA-Z0-9_-]+$`. Note: `BT` requires exactly 6 hex pairs; `ORDER` requires a
  non-negative integer.
- **Load semantics:** `cbx_assignments_load` does **not** run
  `cbx_assignments_validate` after parsing (see Issues #2).
- **Save:** validates strictly first; atomic write 0600. `fsync` return ignored.
- **Parser:** event-based with a `parse_ctx` state machine (root key → sequences →
  per-assignment mappings). Unknown/malformed nesting is tolerated (treated as
  no-op/ignore) rather than rejected.
- **Consumers:** `identify/assign_persist.c`, `identify/assign.c`,
  `identify/identity.c`, `dbus/ip_gamepad_order.c`, `overlay/profile_cycle.c`,
  `overlay/close.c`.

### 4. `config_profile.c` / `config_profile.h` — InputPlumber `device_profile_v1`

- **Purpose:** Parse/serialize InputPlumber DeviceProfile YAML (§7.6). No duplicate
  format; GUI edits the real InputPlumber format.
- **Structs:** `cbx_profile`, `cbx_profile_mapping`, `cbx_source_event`,
  `cbx_target_event`, `cbx_event_prop`. Bounds: 128 mappings, 16 target events /
  mapping, 8 source props.
- **Public API:** `cbx_profile_init`, `cbx_profile_load`, `cbx_profile_parse`,
  `cbx_profile_validate`, `cbx_profile_save`, `cbx_profile_serialize`.
- **Serialize/parse:** libyaml document-based emitter (not string concat) and
  event-based parser. YAML target shape: `version:1, kind:DeviceProfile, name,
  description, mapping:[{name, source_event:{gamepad:{button:Start}},
  target_events:[{keyboard:KeyEsc}]}]`.
- **Complex target-event handling:** advanced InputPlumber mappings (chord,
  delayed_chord) are **accepted but not fully parsed** — a `skip_depth` mechanism
  stores the device class with an empty value and skips nested content.
- **Save:** validates `version==1 && kind==DeviceProfile`; atomic write **0644**
  (profile files must be readable by other InputPlumber tools — differs from 0600
  used elsewhere). Extract-dir-of-path logic builds `<dir>/.<base>.XXXXXX` temp.
- **Consumers:** `manager/profile_editor_list.c`, `manager/profile_editor_seq.c`,
  `manager/profile_save.c`, `manager/profiles_tab.c`, `identify/...`,
  `overlay/profile_cycle.c`, `profile_diagram.c`.

### 5. `config_profile_meta.c` / `config_profile_meta.h` — sidecar (§7.5)

- **Purpose:** Read/write optional `<name>.meta.yaml` sidecars in
  `~/.config/controller-box/profile-metadata/` that override GUI display properties:
  `display_name`, `icon`, `display_order`, `description`.
- **Public API:** `cbx_profile_meta_init/load/parse/save/serialize`,
  `cbx_profile_meta_load_for/save_for`, `cbx_validate_filename`.
- **has_* flags** disambiguate "field absent" vs "empty".
- **Path security** (`build_sidecar_path`): validates filename `^[a-zA-Z0-9_-]+$`,
  ensures `<config>/profile-metadata/` exists (0700), then `realpath()`s both the
  meta dir and config dir and verifies meta is within config (else `-EACCES`).
- **Save:** atomic 0600. `cbx_profile_meta_load_for/save_for` construct the path and
  delegate to load/save.
- **Consumers:** `manager/profiles_tab.c`, `overlay/dynamic_columns.c`
  (via `profile_list`).

### 6. `config_profile_list.c` / `config_profile_list.h` — enumeration + merge

- **Purpose:** Enumerate `.yaml` profiles from user dir + system dir (and shipped
  default), merge sidecar metadata, sort by `display_order` then `display_name`.
  Also enumerates device configs and capability maps (Gap #4).
- **Public API:** `cbx_file_list_enumerate`, `cbx_device_config_list_enumerate`,
  `cbx_capability_map_list_enumerate`, `cbx_profile_list_enumerate`,
  `cbx_profile_list_enumerate_dirs` (explicit dirs for testing).
- **Dedup / priority:** user dir wins over system; shipped `default` scanned first so
  it cannot be shadowed. Entries marked `read_only = is_system || is_default`.
- **Security:** only `*.yaml` enumerated, paths built by joining dir+d_name,
  `lstat` requires regular file (skips symlinks), profile/sidecar opened with
  `O_NOFOLLOW`.
- **Merge:** sidecar (`display_name`/`icon`/`display_order`/`description`) overrides
  profile YAML name/desc; falls back to filename if profile nameless.
- **Consumers:** `manager/profiles_tab.c`, `manager/profile_editor_*`,
  `overlay/dynamic_columns.c`.

---

## Cross-module interface summary

| Header | Primary consumers (outside subsystem) |
|---|---|
| `config_paths.h` | app, overlay_service, icons, ui, theme |
| `config_settings.h` | settings_tab, overlay (dynamic_columns, grid_render) |
| `config_assignments.h` | identify/*, ip_gamepad_order, overlay (profile_cycle, close) |
| `config_profile.h` | manager/profile_editor_*, profile_save, profile_diagram |
| `config_profile_meta.h` | profiles_tab, profile_list |
| `config_profile_list.h` | profiles_tab, profile_editor_*, overlay |

The config subsystem never calls into other subsystems; it is a dependency sink.

---

## Test coverage

Dedicated unit tests (all under `tests/`):
- `test_config_paths.c` (22 cbx_ refs) — XDG resolution, ensure_dir, system dirs.
- `test_font_path.c`, `test_font_init.c` — font discovery.
- `test_settings.c` (55) — defaults, load/save round-trip, validate, icon overrides.
- `test_assignments.c` (60) — load/save/validate/round-trip.
- `test_profile_yaml.c` (69), `test_profile_save.c` (80), `test_profile_validate.c`
  — profile parse/serialize/validate/save.
- `test_profile_list.c` (37) — enumeration, dedup, sidecar merge, file lists.
- Higher-level consumers exercising config paths: `test_assign_persist`,
  `test_gamepad_order`, `test_order_restore`, `test_settings_tab`,
  `test_profiles_tab`, `test_icon_cache`, `test_icon_lookup`, `test_host_mode`,
  `test_installed_*`, `test_manager_*`, `test_overlay_*`.

---

## Potential issues / observations

1. **`cbx_assignments_load` does not validate parsed output** (`config_assignments.c`).
   `parse_assignments_yaml` is lenient and tolerates malformed nesting; the loaded
   struct is returned without a `cbx_assignments_validate` pass. `save` validates,
   so invalid data could round-trip in memory until a save re-rejects it. Consider
   validating in `load` (settings/profile both tolerate malformed fields by design,
   but assignments carries strict format invariants).

2. **Silent overflow on load (`config_assignments.c` `process_scalar`).** When
   `gamepad_order_count`/`assignment_count` reaches its max, extra YAML entries are
   silently dropped but the file still loads "successfully" (rc 0). No indication to
   the caller that data was truncated.

3. **`fsync` return values ignored** (`config_assignments_save`,
   `config_settings_save`). `config_settings_save` documents this as deliberately
   non-fatal, but `config_assignments_save` (and `config_profile_save`,
   `config_profile_meta`'s `save_atomic`) ignore it without a comment. `fsync`
   failure after `fchmod`+write means durability is not guaranteed.

4. **Profile save uses 0644 while others use 0600.** Deliberate (InputPlumber
   compatibility) but an inconsistency worth documenting in any task touching file
   modes.

5. **`cbx_font_path` static buffer** is not thread-safe and returns a pointer valid
   only until the next call. Any caller that stores the pointer across font-path
   calls will get a stale/shared buffer.

6. **`skip_depth` complex-target-event handling (`config_profile.c`)** is heuristic;
   depth bookkeeping is duplicated between the switch and the post-switch "detect
   entry" blocks. Off-by-one bugs here would corrupt only chord/delayed_chord
   profiles, which are accepted-but-unparsed by design. Worth a targeted test if the
   GUI ever needs to preserve them.

7. **`cbx_settings_load` clamping runs `clamp_settings` then a second pad-loop.**
   The two loops overlap in effect (both coerce blank/unknown types to
   `DEFAULT_TYPE`); harmless but redundant. The parser's `process_scalar_value`
   ignores a malformed `count` (`strtol` endp check) and leaves the default — the
   count/types arrays can momentarily disagree until clamp.

8. **Icon-override parsing (`config_settings.c`) is the most fragile state logic.**
   It uses `ovr_type_idx` + `ovr_key` to detect key→value pairs inside an override
   item and commits only when both `type` and `icon` are non-empty. If a YAML
   override item contains an extra key-value pair, `icon_override_count` can be
   bumped on the wrong pair; an item missing one field is silently dropped. Also,
   duplicate `type` entries in a file are both stored (only `set` helper dedupes).
   `cbx_settings_validate` only checks non-empty type/icon, so unknown types
   validate as OK.

9. **Load does not reject unknown controller `type` values** — they are silently
   coerced to `xb360` by `clamp_settings`, so a typo in a hand-edited
   `settings.yaml` is masked rather than reported.

10. **`config_profile_list.h` and other headers `#define PATH_MAX 4096`** if not
    already defined. This is a portability guard; consumers that do not include
    `<limits.h>` first rely on it. Not a bug but a coupling note.

11. **`build_sidecar_path` realpath ordering (`config_profile_meta.c`).** Calls
    `cbx_ensure_dir(meta_dir, 0700)` *before* `realpath(config_dir)` so config_dir is
    created by recursion first; but if `config_dir` still fails to resolve, it bails
    with `-errno` and a comment noting the edge. Low-risk but a place to watch.

12. **`cbx_validate_id` for `BT:` requires exactly 6 octets** and rejects
    `ORDER:` with a leading `+`/sign (digits only). These strictness choices are
    intentional but must be kept in sync with InputPlumber-side canonicalization in
    `identify/*`.

---

## Recommended focus areas for the planner

- **Tighten `cbx_assignments_load`** to validate after parse (or document the
  lenient contract) — closes a gap between load and save strictness.
- **Refactor icon-override YAML parsing** in `config_settings.c` to a per-item
  temporary struct + explicit commit, removing the `ovr_type_idx`/`ovr_key` hack.
- **Audit/decide on `fsync`** error handling across all four save paths for
  consistency.
- **Thread-safety contract for `cbx_font_path`** — either document the single-call
  contract or return into caller buffer.
- **Add tests** for truncation-at-max on assignments load and for chord/delayed_chord
  profile round-trips.
