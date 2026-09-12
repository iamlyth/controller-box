# Subsystem Study Report: src/icons

## Scope

`src/icons/` implements Controller-Box's icon pipeline: parse the
`controller-icons.yaml` DeviceType→icon mapping (SPEC §8.4), rasterize
SVG assets into SDL2 textures (SPEC §8.3), and resolve a device type to a
texture+label at runtime applying profile overrides (SPEC §8.5). It is a
self-contained, pure C library with no internal callers into other
modules — it **calls out** to two dependencies: `config/config_paths.h`
(safe-directory resolution) and the `config.h` generated `DATA_DIR`.

Four files (6 source/header pairs):

- `src/icons/icon_map.c/.h` — YAML mapping parser + DeviceType lookup.
- `src/icons/icon_cache.c/.h` — SVG→SDL2_Texture rasterization cache.
- `src/icons/icon_lookup.c/.h` — runtime resolution (device→texture+label), security path validation.

## 1. Files

### `src/icons/icon_map.h` / `icon_map.c` — mapping table

**Purpose:** Load/parse `controller-icons.yaml` (libyaml, event-based) and
resolve an InputPlumber `DeviceType` string to a stable icon id + display name.

Structures:
- `cbx_icon_entry` — one row: `type[64]`, `icon[256]` (public icon id),
  `asset[256]` (explicit installed SVG basename), `name[128]`.
- `cbx_icon_map` — `entries[64]` (cap `CBX_ICON_MAP_MAX_ENTRIES`),
  `count`, `loaded`, `yaml_path[512]`.

Public API (entry points):
- `cbx_icon_map_init` — zero the struct.
- `cbx_icon_map_load(map, path)` — `open(O_RDONLY|O_NOFOLLOW)`, size-cap 1 MB
  (`MAX_DOC_SIZE`), read whole file, `parse_icon_map_from_string`.
- `cbx_icon_map_parse(map, yaml, len)` — parse from memory (used by tests).
- `cbx_icon_map_lookup(map, type, out_icon, out_name)` — returns matching
  entry, else defaults: `CBX_ICON_DEFAULT_ICON` ("generic-gamepad") + raw
  type as name. Returns `-EINVAL` on NULL type (note: header doc claims
  "always succeeds" — mismatch).
- `cbx_icon_map_default_path(out)` — prefers `DATA_DIR/controller-icons.yaml`,
  falls back to `SOURCE_DATA_DIR/...`; rejects symlinks via `lstat`.

Key internals:
- `parse_icon_map_events` — hand-rolled event state machine
  (`MAP_STATE_TOP/VIRTUAL_TYPES/CUSTOM_ICONS/ENTRY`), max depth 50,
  rejects all YAML tags (`check_event_tags` → `-EPERM`), truncates fields
  via `safe_copy`. Only entries with a non-empty `type` (i.e. the
  `virtual_types` section) are stored; `custom_icons` are ignored.

Security: NOFOLLOW open, 1 MB cap, depth cap, tag rejection, bounded
buffers. Solid.

### `src/icons/icon_cache.h` / `icon_cache.c` — SVG→texture cache

**Purpose:** Rasterize SVG assets into SDL2 textures with one reusable
nanosvg rasterizer and cache them in an open-addressing (linear-probe,
djb2 hash) table keyed by icon name.

Structures:
- `cbx_icon_cache_entry` — `name[CBX_ICON_ICON_LEN]`, `SDL_Texture*`,
  `width`, `height`.
- `cbx_icon_cache` — `renderer`, opaque `NSVGrasterizer*`,
  `entries[CBX_ICON_CACHE_HASH_SIZE=256]`, `count`, `icon_dir[512]`,
  `target_size`.
- Constants: `CBX_ICON_CACHE_MAX 128`, hash size 256 (power of two → bitmask).

Public API (entry points):
- `cbx_icon_cache_init(cache, renderer, icon_dir, target_size)` — creates
  rasterizer; re-clears if already initialised. `target_size` = max pixel
  dimension (aspect preserved).
- `cbx_icon_cache_load(cache, map)` — rasterize every `virtual_types` icon,
  skipping already-cached (dedup). **Best-effort**: logs per-icon failure,
  always returns 0.
- `cbx_icon_cache_get(cache, name)` — open-addressing probe → `SDL_Texture*`.
- `cbx_icon_cache_get_dims(cache, name, &w, &h)`.
- `cbx_icon_cache_load_one(cache, name)` — single icon; strips `"cc-"` prefix
  to derive the SVG basename (`cc-ps5` → `ps5.svg`), appends `.svg`, delegates
  to `load_asset`.
- `cbx_icon_cache_load_asset(cache, name, filename)` — strict explicit-basename
  path used by the profile editor; validates simple basename + `.svg` suffix.
- `cbx_icon_cache_insert(cache, key, tex, w, h)` — cache an externally-created
  texture (PNG); takes ownership; replaces on key collision.
- `cbx_icon_cache_cleanup(cache)` — destroy all textures + rasterizer.

Key internals:
- `rasterize_svg_file` — path built as `%s/svg/%s` (640-byte buffer);
  `lstat` rejects non-regular/symlink assets; `nsvgParseFromFile(path,"px",96)`;
  guards non-finite/zero dimensions; scale to `target_size`; defends against
  float→int overflow, pixel-alloc overflow (`SIZE_MAX/4`), and rejects
  fully-transparent rasterizations (byte-3 alpha scan) — the latter is a
  defensive measure so the profile-editor production diagram never adopts a
  blank texture. Creates `SDL_PIXELFORMAT_ABGR8888` STATIC texture,
  `SDL_SetTextureBlendMode(BLEND)`, `SDL_SetTextureColorMod` for theming.
- `insert_entry` — linear probe; claims empty/tombstone slot; replaces in place
  on same name (does not double-count).

### `src/icons/icon_lookup.h` / `icon_lookup.c` — runtime resolution + security

**Purpose:** Single entry point resolving `device_type` (+ optional profile
`icon_override`) → `cbx_icon_result {texture,width,height,label}`.

Public API (entry points):
- `cbx_icon_lookup(cache, map, device_type, icon_override, result)` — SPEC §8.5
  resolution order: (1) override absolute-path PNG (validated) or built-in
  icon name; (2) `cbx_icon_map_lookup`; (3) fallback `generic-gamepad` + raw
  type. Returns 0 with `texture==NULL` if nothing resolved (label still set).
- `cbx_icon_validate_path(abs_path, resolved, size)` — standalone security check.

Key internals:
- `load_png` — validates path, `IMG_Load`, `SDL_CreateTextureFromSurface`,
  caches under `cache_key` (original input path).
- `path_is_safe` — rejects leading `..` and any `/..` full component.
- `path_within` — canonical prefix boundary check.
- `cbx_icon_validate_path` — requires absolute; rejects traversal; `realpath`;
  re-checks canonical path; must fall within one of four safe dirs: user config
  dir, user profiles dir, `cbx_data_dir()` (/usr/share/controller-box), or
  `cbx_system_inputplumber_dir()`. Returns `-EINVAL/-EACCES/-ENAMETOOLONG`.

## 2. Entry points (called from outside)

| Function | Called from |
|----------|-------------|
| `cbx_icon_map_init` | `overlay_service.c:1473`, `profile_editor_list.c:286` |
| `cbx_icon_map_default_path` | `overlay_service.c:1475`, `profile_editor_list.c:285` |
| `cbx_icon_map_load` | `overlay_service.c:1476`, `profile_editor_list.c:283` |
| `cbx_icon_cache_init` | `overlay_service.c:1476` (icon_dir, 48), `profile_editor_list.c:299` (icon_dir, scaled raster size) |
| `cbx_icon_cache_load` | `overlay_service.c:1478` |
| `cbx_icon_lookup` | `overlay/grid_render.c:534` (per rendered cell, with settings override) |
| `cbx_icon_cache_load_asset` | `profile_editor_list.c:498` (strict diagram catalog asset) |
| `cbx_icon_map_lookup` | `profile_editor_list.c:472` (display name), grid path via `cbx_icon_lookup` |

Callers:
- **Overlay service** (`src/app/overlay_service.c`) — owns both the `icon_map`
  and `icon_cache`; the grid renderer (`src/overlay/grid_render.c`) calls
  `cbx_icon_lookup` at draw time, passing a settings-level icon override
  (`cbx_settings_icon_override`).
- **Profile editor** (`src/manager/profile_editor_list.c`) — maintains its own
  `icon_map`+`icon_cache`, but resolves the production diagram asset through
  the **licensed-diagram catalog** (`cbx_profile_diagram_catalog_asset`,
  `cbx_profile_diagram_device_geometry_known`, data files
  `licensed-diagram-authority.json`/`oracle.json`) and uses
  `cbx_icon_cache_load_asset` with explicit `asset` filenames.

## 3. Internal state & lifecycle

- No file-scope globals — all state lives in caller-owned structs
  (`cbx_icon_cache` has the rasterizer + renderer pointer; caller keeps the
  renderer alive until `cbx_icon_cache_cleanup`).
- Lifecycle: `overlay_service`: `map_init → map_default_path → map_load →`
  `cache_init(renderer, cbx_icon_dir(), 48) → cache_load`. Same pattern in the
  profile editor but with a renderer-scale-dependent `target_size`.
- The cache is loaded once at startup (best-effort) and only grows via
  on-demand `load_one`/`load_asset`/`insert` during interaction. No teardown
  ordering hazard: `cache_cleanup` resets struct to reusable state.

## 4. Error handling

- All public functions return `0`/negative `errno` (`-EINVAL`, `-ENOENT`,
  `-ENOMEM`, `-EPERM`, `-EFBIG`, `-EACCES`, `-ENAMETOOLONG`, `-EOVERFLOW`,
  `-EIO`). Texture resolution failure in `cbx_icon_lookup` is non-fatal:
  returns 0 with `result->texture == NULL`.
- `cbx_icon_cache_load` swallows per-icon failures (logs to stderr,
  returns 0) — by design a batch best-effort loader.
- Runtime failures (missing SVG, unloadable PNG) fall through resolution tiers
  to `generic-gamepad`, so the UI never hard-fails on a missing asset.

## 5. Test coverage

- `tests/test_icon_map.c` (851 lines) — parse/load/lookup, NULL args, unknown
  types, field truncation, max-entries cap, YAML tag rejection, doc-size cap,
  temp-file load, `test_svg_all_compat` (parses every `data/icons/svg/*.svg`
  with nanosvg), real-YAML load.
- `tests/test_icon_cache.c` (552 lines) — init/load/lookup/dims/load_one/
  insert/cleanup, NULL args, idempotent load, dedup, hash-collision probe,
  recolour (color-mod), blend mode, traversal rejection, production-path load
  (BUG-0008/0009). Uses headless SDL dummy driver via `test_harness`.
- `tests/test_icon_lookup.c` (713 lines) — device lookup, override resolution
  (built-in + PNG path + cache), `cbx_icon_validate_path` traversal/safe-dir
  checks, label correctness, on-demand load, dims, fixture PNG `test_icon.png`
  (8×8) in a safe config dir.
- Indirect coverage: `test_grid_render.c`, `test_overlay_visual.c` /
  `test_golden.c` / `test_installed_diagram.sh`, `test_editor_list_mode.c`,
  `test_profiles_tab.c` (via profile editor), `test_overlay_service.c`.
- Registered in `tests/CMakeLists.txt` as `test_icon_map`, `test_icon_cache`,
  `test_icon_lookup` (each links `controllerbox`, cmocka; cache+lookup also
  link SDL2_image/nanosvg).

## 6. Potential issues / notes for planner

1. **PNG cache dedup is not canonical.** In `icon_lookup.c::load_png`, the
   texture is stored under `cache_key` = the *original* input path
   (`cbx_icon_lookup` passes `icon_override, icon_override`), **not** the
   `realpath`-resolved path, despite the comment claiming "we use the
   canonical path for deduplication". Two symlinked/aliased paths to the same
   PNG produce two cache entries. Low impact; worth a fix or comment correction.
2. **The `asset` field is under-used by the grid path.** Only 6 of 22 YAML
   entries declare `asset`; the overlay/grid load path (`cbx_icon_cache_load`
   → `load_one`) ignores `asset` entirely and relies on the **`"cc-"`-prefix
   strip convention** to derive the SVG basename. Verified all 10 distinct
   icon ids resolve to existing SVGs today, but this is a fragility: a future
   map where the icon id's filename is non-derivable would silently break the
   grid while the profile-editor's strict `asset`/catalog path still works.
   The `asset` field exists precisely to make naming explicit — the grid path
   could read `entries[i].asset` when present.
3. **`cbx_icon_map_lookup` doc mismatch:** header says "always succeeds —
   unknown types get defaults", but it returns `-EINVAL` on a NULL `type`.
   Trivial documentation fix.
4. **Parser state imprecision:** in `parse_icon_map_events`, after an entry's
   `MAPPING_END` the state is set unconditionally to `MAP_STATE_VIRTUAL_TYPES`
   even inside `custom_icons`. Functionally harmless (custom entries lack a
   `type` and are never stored; `SEQUENCE_END` still returns to `TOP`) but the
   state bookkeeping is loose — a future extension to `custom_icons` parsing
   (e.g. exposing custom overrides to the editor) would need to track which
   section an entry belongs to.
5. **Not thread-safe** (djb2 probe table, no locks) — safe today because both
   consumers load once at startup and render single-threaded; any future
   background loading must serialize access.
6. **Best-effort `cache_load` returns 0 always.** Callers (`overlay_service`)
   treat icon load failure as non-fatal by design (grid falls back to
   `generic-gamepad` at draw time). Deliberate; not a bug.
7. Security posture is strong and regression-guarded: NOFOLLOW opens, 1 MB/doc
   caps, depth cap, tag rejection, traversal + realpath safe-dir validation,
   transparent-pixel rejection, and multiple integer-overflow guards around
   pixel allocation. The transparent-pixel rejection and strict regular-file
   check on SVG assets were added specifically to protect the profile-editor
   production diagram; do not weaken.
