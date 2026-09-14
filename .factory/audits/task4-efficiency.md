# Efficiency Audit — Task 4: Finish Host Mode editing and visible transition rendering

Scope: performance, resource usage, and algorithmic complexity of the current
codebase state for host-mode editing, transition rendering, and the production
loop that drives it (`src/overlay/host_mode.c`, `src/overlay/grid_render.c`,
`src/overlay/lifecycle.c`, `src/overlay/surface_build.c`, `src/ui/text.c`,
`src/app/overlay_service.c`, `src/dbus/ip_intercept_poll.c`).
This report supersedes the previous round's file for the narrower
"re-render on host-mode entry" task; findings that remain valid are carried
forward and re-verified against current source.

## BLOCKER — O(C²) InterceptMode polling: every poll's timer ticks *all* polls

- **Files:** `src/dbus/ip_intercept_poll.c:28-40` (`sdl_timer_cb`, pushes one
  event per poll, all sharing `poll->sdl_event_type`, with the owning poll in
  `event.user.data1`), `src/dbus/ip_intercept_poll.c:105-125`
  (`ip_intercept_poll_start` creates one SDL timer per composite), and
  `src/app/overlay_service.c:1390-1394` (step handler):
  ```c
  if (ev.type == svc->poll_event_type) {
      for (int i = 0; i < svc->poll_count; i++)
          ip_intercept_poll_tick(&svc->polls[i]);
  }
  ```
- **Issue:** Each of the C armed composites owns a 50 ms SDL timer, and each
  timer event causes the handler to tick **all** C polls. Every
  `ip_intercept_poll_tick` on a non-IDLE poll performs a *synchronous* DBus
  property read (`backend->get_property` on `InterceptMode`,
  `ip_intercept_poll.c:158-165`) on the main/render thread. Polls are armed
  continuously in production (steady state `PASS_WAIT`, re-armed by
  `overlay_service.c:1491-1497` when idle), so steady-state DBus traffic is
  C² reads per 50 ms: at C=4 that is 320 synchronous round trips/s; at the
  declared maximum `CBX_MAX_COMPOSITES` = 16 (`src/dbus/ip_device_model.h:28`)
  it is 5,120 round trips/s on the same thread that must render host-mode
  transitions. This starves the 10 ms step loop and directly degrades the
  visible transition rendering this task is required to deliver; it also
  floods the bus with duplicate reads of identical properties.
- **Recommendation:** The owning poll pointer is already carried in
  `ev.user.data1`; tick only that poll in the step handler, or replace the
  per-poll timers with a single shared timer that ticks all polls once per
  interval. Either change reduces traffic from O(C²) to O(C) with no test
  impact (tests call `ip_intercept_poll_tick` directly).

## WARN — Per-cell icon resolution re-done for every row in the render path

- **Files:** `src/overlay/grid_render.c:522-540` (cell loop calling
  `cbx_settings_icon_override` + `cbx_icon_lookup`), `src/icons/icon_lookup.c`
  (`cbx_icon_map_lookup` linear scan + repeated hash lookups per call),
  `src/config/config_settings.c:88-99` (linear override scan).
- **Issue:** Inside the per-row, per-cell loop, `cbx_icon_lookup` (linear
  icon-map scan, 1–3 icon-cache hash lookups, snprintf of the display label)
  is executed for every `col > 0` cell of every row on every re-render. The
  result depends only on the **column's** device type plus the settings
  override, both of which are constant across rows within a render pass.
  With the current bounds that is 16 rows × 16 slot columns = 256 redundant
  resolutions (≈ thousands of strcmp/hash probes) per dirty render, versus 16
  if hoisted.
- **Recommendation:** Resolve icon name/override/texture per column once
  before the row loop (a small `col_icons[CBX_MAX_CONTROLLERS + 1]` array),
  then only the `SDL_RenderCopy` remains per cell.

## WARN — Position indicators: up to ~10 draw calls per cell for a shape that is not a circle

- **Files:** `src/overlay/grid_render.c:299-333` (`draw_filled_circle`,
  `draw_hollow_circle`), called at `:579`, `:583`, `:586`.
- **Issue:** `half_w = (int)(radius * 0.7071)` is computed **once, outside**
  the `dy` loop, so every scanline uses the same half-width:
  `draw_filled_circle(r=6)` emits 9 separate `SDL_RenderFillRect` calls that
  together draw a 9×9 *square*, and `draw_hollow_circle` emits 18 one-pixel
  fills that draw two vertical 9-px *bars* — not a circle/ring in either
  case. Every non-current cell of every row draws a hollow indicator, so one
  full re-render issues roughly rows × cols × 10 ≈ 2,500+ `RenderFillRect`
  calls just for indicators. This is both wasted draw-call overhead in the
  transition render path and a semantic mismatch with the "position
  indicator circle" comment (the golden images have locked in the square).
- **Recommendation:** Compute the per-scanline half-width
  (`half_w = (int)sqrtf((float)(radius*radius - dy*dy))`) so the shape is an
  actual circle, and/or draw the filled indicator as a single `RenderFillRect`
  square if a square is the accepted visual. Either fix collapses the
  per-cell cost from ~10 renderer calls to 1–3.

## WARN — Text cache has no eviction; once full, every render leaks a GPU texture

- **Files:** `src/ui/text.c:194-247` (`cbx_text_render`: cache full or text
  ≥ `CBX_TEXT_MAX_LEN` → `render_to_texture` **without caching**), callers in
  `src/overlay/grid_render.c:400-410, 456-472, 596-612` (never destroy the
  returned texture).
- **Issue:** The cache is bounded at 256 entries with no eviction
  (`src/ui/text.c:14-17` documents this). In the uncached path a fresh SDL
  texture is created per call and no caller ever destroys it — an unbounded
  texture leak per dirty render once the cache saturates. Task 4's host-mode
  transitions multiply cache pressure: entering host mode re-renders every
  row's label in the *frozen* text color and exiting re-renders in the normal
  color (colors are part of the cache key, `grid_render.c:451-455`), and each
  L1/R1 profile change creates a new label string per row. With
  rows × profiles × 2 label variants × 2 color variants, saturation of 256
  entries is plausible in a long-running daemon, after which every
  re-render leaks ~2 textures per row.
- **Recommendation:** Add bounded eviction (LRU or clear-on-theme/mode
  transition), or make the uncached path reuse a single scratch texture that
  the cache owns and re-renders in place. At minimum, document and assert the
  expected working set stays below 256.

## WARN — Redundant per-frame `SDL_QueryTexture` for text whose dims are already cached

- **Files:** `src/overlay/grid_render.c:407, 464, 602`; cached dims available
  via `cbx_text_get_dims` (`src/ui/text.c:250-277`, populated at insert time
  `src/ui/text.c:239`).
- **Issue:** Every re-render calls `SDL_QueryTexture` for the header labels,
  each row label, and each profile label even on cache hits, although the
  cache entry already stores `width`/`height`. Minor per-call cost, but it is
  pure redundant work repeated rows×3 times per frame in the transition
  render path.
- **Recommendation:** Use `cbx_text_get_dims` (or return the dims alongside
  the texture) for cached entries.

## WARN — Blocking, per-press persistence and multi-second save stalls on the main thread

- **Files:** `src/app/overlay_service.c:196-235` (`cbx_overlay_on_save`
  Phase 1: for *every* composite, a synchronous `TargetDevices` set plus
  `wait_for_attachment` poll loop, each bounded by
  `CBX_RECONCILE_TIMEOUT_MS` = 2000 ms, `overlay_service.h:254-255`),
  `src/app/overlay_service.c:291-309` (`cbx_overlay_on_profile_change`:
  synchronous `LoadProfilePath` + `cbx_assignments_save` atomic YAML write on
  **every** L1/R1 press), `src/config/config_assignments.c:605+`
  (mkstemp/write/rename per call).
- **Issue:** (a) The close/save path performs 2 sequential blocking
  wait-loops per composite on the SDL event/render thread — worst case
  16 × 2 × 2 s ≈ 64 s of frozen UI (no SDL event drain, no frame present)
  when the engine is slow to confirm. (b) Host-mode profile cycling does a
  DBus round trip *and* a full validated atomic file rewrite per button
  press; holding L1 cycles the profile repeatedly, each press re-writing
  `assignments.yaml`.
- **Recommendation:** Bound the aggregate save deadline (e.g. a single
  deadline shared across the per-composite waits) and consider batching the
  Phase-1 clears (one `GetManagedObjects`/enumerate to confirm all clears,
  rather than a per-composite poll loop). For profile cycling, debounce the
  disk persist (persist the final choice on close/idle) while keeping the
  live `LoadProfilePath` apply, or persist only when the profile actually
  changed.

## WARN — `assigned_composite_for_slot` re-reads PersistentId over DBus O(assignments × composites) times

- **Files:** `src/app/overlay_service.c:427-443`, called per slot from
  `cbx_reconcile_startup_targets` (`:562`, `:621`) and the type-correction
  loop (`:600`).
- **Issue:** Each call loops assignments × composites and performs a
  **synchronous** `ip_composite_get_persistent_id` DBus round trip inside the
  inner loop. With 16 assignments × 16 composites that is up to 256
  sequential round trips *per call site per reconcile* (startup and every
  backend recovery). The PersistentIds are per-composite constants;
  `fill_composite_info` (`:394-421`) already fetches and stores them at other
  call sites.
- **Recommendation:** Fetch each composite's PersistentId once per reconcile
  into a small array (16 calls instead of 256) and match assignments against
  that array in memory.

## INFO — Dirty-rect machinery is effectively dead: every trigger marks the whole surface

- **Files:** `src/app/overlay_service.c` (all triggers call
  `cbx_overlay_surface_mark_dirty_all`: prop change `:162`, save `:342`,
  slot change `:324`, host-mode change `:375`, MOVED `:1331`, `:1373`),
  `src/overlay/lifecycle.c:79` (`show_surface`).
- **Issue:** The Task-23 incremental dirty-rect system
  (`cbx_overlay_surface_mark_dirty` with per-region rects) is never used in
  production; host-mode selection moves and enter/exit transitions re-render
  the entire 800×600 texture. At the bounded grid size this is cheap and
  deliberate (consistent with the W2 reconciliation), but the merge/render
  infrastructure adds no production value as wired.
- **Recommendation:** Acceptable at current bounds. If the grid or render
  callback grows, dirty only the affected rows' rects (row geometry is
  computable from the same layout constants used in `grid_render.c`).

## INFO — Startup renders the surface twice

- **Files:** `src/app/overlay_service.c:1558-1570` (pre-build render),
  followed by `cbx_overlay_on_save` at `:1574-1590` which mutates the grid
  (conflict resolve + assignment apply) and re-marks everything dirty.
- **Issue:** The §4.9 pre-build render is immediately invalidated by the
  restore-save that follows; one full render pass of work is discarded.
- **Recommendation:** Run the restore (`on_save`) before the pre-build
  render, or skip the explicit pre-render when the restore path will run.

## INFO — `fill_composite_info` error-path free is defensive only

- **Files:** `src/app/overlay_service.c:394-421`; backend contract
  `src/dbus/ip_composite.c:206-218` (`*out_value = NULL` before the call)
  and `src/dbus/dbus_client.c` (`sd_get_property` frees and leaves the out
  pointer untouched on error).
- **Issue:** Carried forward from the prior audit: on an error return the
  out-pointer is NULL by contract, so the missing `free(id)`/`free(name)` on
  the fallback branch cannot leak via the sd backend. Only a non-conforming
  backend could trip it.
- **Recommendation:** Downgrade to defensive style — `free(id); free(name);`
  unconditionally after the branch (free is NULL-safe). Not a defect today.

## INFO — Fixed 100 Hz step loop; unconditional show/hide each step

- **Files:** `src/app/overlay_service.c:1752-1755` (`SDL_Delay(10)` loop),
  `:1436-1440` (`cbx_renderer_show`/`hide` every step → `SDL_ShowWindow`
  no-ops when state unchanged, `src/ui/renderer.c:190-200`).
- **Issue:** The loop wakes at 100 Hz even when idle with nothing to do;
  `SDL_ShowWindow`/`SDL_HideWindow` are called every iteration though SDL
  internally no-ops. Cost is small but constant.
- **Recommendation:** Optionally gate show/hide on a state-change edge and
  lengthen the idle delay (e.g. 50–100 ms when overlay inactive). Low
  priority.

## Summary

One BLOCKER: the InterceptMode poll fan-out makes steady-state DBus traffic
quadratic in composite count with synchronous round trips on the render
thread — at the declared maximum of 16 composites this saturates the loop
that must present host-mode transition frames; the fix is a one-line change
to tick only the poll named in `event.user.data1` (or a single shared timer).
Six WARNs cover per-cell redundant icon resolution, indicator draw-call
bloat (and a non-circular indicator shape), the text-cache leak-on-full
path that host-mode color-variant labels push toward, redundant
`SDL_QueryTexture`, per-press disk persistence and multi-second save stalls
on the main thread, and O(assignment × composite) PersistentId re-reads in
reconcile. INFO items record the dead dirty-rect machinery, the doubled
startup render, a defensive-only free, and the fixed-cadence loop.