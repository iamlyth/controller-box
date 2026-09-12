# Efficiency Audit — Task 4: Re-render overlay on host-mode entry and reconcile dirty triggers

Scope: performance, resource usage, algorithmic complexity of the host-mode
entry re-render and dirty-trigger logic.

## No BLOCKER findings

The core hot path was reviewed carefully and is sound:

- The re-render in `cbx_overlay_service_step` (`src/app/overlay_service.c:1397`)
  is properly gated on BOTH `cbx_overlay_lifecycle_is_active()` AND
  `cbx_overlay_surface_is_dirty()`. When the overlay is idle/inactive or the
  surface is clean, no render or `SDL_RenderPresent` happens — no per-frame
  render waste, no idle spin.
- Dirty accumulation concern: host-mode change fires `cbx_overlay_on_host_mode_change`
  → `mark_dirty_all` even while the overlay is inactive. This is non-accumulating
  (`add_all` resets `count=0` then pushes one full rect), and the dirty rect is
  drained on the next active render. No unbounded growth.
- `cbx_dirty_rect_merge` is O(n²) worst-case, but count stays bounded (mostly 1
  full rect from `mark_dirty_all`), so it is effectively a no-op in this path.
- No new allocations/frees or file/DBus descriptor churn were introduced by the
  state-change callback; it only toggles the dirty flag.

## WARN — O(n²) conflict re-detect on every slot change fires on host-mode slots

- **Files:** `src/overlay/conflict.c:20` (`cbx_conflict_detect`), invoked from
  `src/app/overlay_service.c:326` (`cbx_overlay_on_slot_change`) which is wired
  to host-mode slot changes via `on_host_slot_change` (`overlay_service.c:358`).
- **Issue:** `cbx_conflict_detect` is a pure O(rows²) double loop
  (`for i { for j<i { get_cur_col(j) } }`), and it is re-run from scratch
  (`cbx_conflict_list_init` + full detect) whenever a host-mode LEFT/RIGHT slot
  change or a player slot change occurs.
- **Severity:** WARN — not a BLOCKER.
- **Recommendation:** `CBX_GRID_MAX_ROWS` = `CBX_MAX_COMPOSITES` = 16 (fixed
  constant, `grid_render.h:51`), so the absolute cost is trivially bounded and a
  full re-detect is only a few hundred comparisons on a user-driven (not
  per-frame) event. If this ever runs with an unbounded row count or on the
  render path, switch to an O(n) bucket/hash over columns; for the current
  bounds this is acceptable. Optionally dirty only the rows whose grid column
  changed rather than re-detecting the whole grid, but the win is negligible at
  n=16.

## WARN — small allocation leak in `fill_composite_info` (host-mode-entry setup path)

- **Files:** `src/app/overlay_service.c:394-421` (`fill_composite_info`).
- **Issue:** `ip_composite_get_persistent_id` allocates `id` on
  success (`*id = malloc(...)`). The success/free path is gated on
  `rc == 0 && id`, but when the call returns non-zero *and still produces a
  non-NULL out-pointer* (an allocation without a success code), the 
  `else`/failure branch does not `free(id)` — the same applies to `name` from
  `ip_composite_get_name`. If the backend can allocate the out-buffer but
  return an error, this leaks once per composite during setup.
- **Severity:** WARN — low frequency (one run at init on a slow/degraded-DBus
  path), not on the per-frame hot path. This is a resource leak that an auditor
  must flag.
- **Recommendation:** free unconditionally before the branch fallback:
  ```c
  if (ip_composite_get_persistent_id(backend, bus, entry->path, &id) == 0 && id) {
      snprintf(..., "%s", id);
  } else {
      snprintf(..., "composite-%d", entry->index);
  }
  free(id);           /* free NULL-safe */
  ```
  (and likewise for `name`). Verify the backend contract; if the out-pointer
  is guaranteed NULL on error, this is INF/not-an-issue.

## INFO — host-mode navigation re-renders the full surface per keypress

- **Files:** `src/app/overlay_service.c:1259-1262` and `:1320-1323`
  (MOVED/SLOT → `cbx_overlay_surface_mark_dirty_all`), plus
  `cbx_overlay_on_host_mode_change` (`overlay_service.c:375-383`).
- **Issue:** Every host-mode UP/DOWN move (selection change), LEFT/RIGHT slot
  change, and every enter/exit marks the *entire* surface dirty via
  `mark_dirty_all`, forcing a full redraw even though only the host,
  old-selected, and new-selected rows actually change visuals. The full
  dirty-rect machinery (`cbx_dirty_rect_add`/`mark_dirty`) exists but host mode
  never uses it.
- **Severity:** INFO — design trade-off, not a defect. At a bounded 16-row grid
  and 10 ms frame cadence, a full re-render is cheap, and whole-surface redraw
  is the established pattern elsewhere (slot/profile change also uses
  `mark_dirty_all`). Marking per-row rects would add branching for marginal
  gain.
- **Recommendation:** Leave as-is for current bounds. If the grid size is later
  un-bounded or the render callback becomes expensive, dirty only the affected
  rows' `SDL_Rect`s and call `cbx_overlay_surface_mark_dirty`, letting
  `cbx_dirty_rect_merge` coalesce them. The current `mark_dirty_all` is
  deliberate and consistent (matches task W2 intent).

## Summary of the task's efficiency contract

The task's acceptance (re-render on host-mode entry; consistent, deliberate
dirty triggers) is met without introducing any BLOCKER-level performance or
resource issue. The re-render is correctly gated and non-accumulating; dirty
marking is consistent. Two WARN-level items (bounded-quadratic conflict detect;
one init-time free-guard leak) and one INFO (full-surface host-mode navigation)
are recorded for the developer.
