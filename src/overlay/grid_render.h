/*
 * grid_render.h — Character select grid data model and rendering.
 *
 * Task 29 — Character select grid rendering and Player Mode navigation.
 *
 * The select grid is the core data model for the overlay (SPEC §4.1):
 *   - Rows = physical controllers (composite devices)
 *   - Columns = player slots (virtual controllers) + Unassigned (leftmost)
 *   - Each cell shows the virtual device type icon (SPEC §8.1)
 *   - Current position highlighted per controller
 *   - Profile label per row (controller model name + current profile)
 *   - No user-assigned names or colors (§4.8 — positional display only)
 *
 * The data model is pure (no I/O, no DBus) for testability.  The caller
 * gathers composite info, settings, assignments, and the profile list,
 * then calls cbx_select_grid_build.  Navigation functions modify the
 * in-memory grid; side effects (assignment update, LoadProfilePath) are
 * handled by the caller via callbacks in player_mode.h.
 *
 * Rendering is via cbx_select_grid_render, which draws the grid into an
 * SDL_Renderer using the icon cache, text cache, and theme.  The render
 * function is NULL-safe (no-op if renderer is NULL).
 */
#ifndef CBX_OVERLAY_GRID_RENDER_H
#define CBX_OVERLAY_GRID_RENDER_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#include "config/config_assignments.h"
#include "config/config_settings.h"
#include "config/config_profile_list.h"
#include "dbus/ip_device_model.h"
#include "icons/icon_cache.h"
#include "icons/icon_map.h"
#include "ui/text.h"
#include "ui/theme.h"

/* Forward declaration — conflict.h includes this header. */
struct cbx_conflict_list;

/* Forward declaration — host_mode.h includes this header. */
typedef struct cbx_host_mode cbx_host_mode;

#ifdef __cplusplus
extern "C" {
#endif

/* --- Limits -------------------------------------------------------------- */

#define CBX_GRID_MAX_ROWS     CBX_MAX_COMPOSITES
#define CBX_GRID_MAX_COLS     (CBX_MAX_CONTROLLERS + 1)  /* +1 Unassigned */
#define CBX_GRID_MAX_PROFILES CBX_MAX_PROFILES
#define CBX_GRID_PROFILE_LEN  CBX_MAX_PROFILE_LEN

/* Column 0 is always Unassigned.  Columns 1..N are player slots P1..PN. */
#define CBX_GRID_UNASSIGNED_COL  0

/* --- Data structures ----------------------------------------------------- */

/* Per-composite info needed to build a grid row.  The caller gathers
 * this (identity, model name, DBus path) and passes it to _build. */
typedef struct {
    char id[CBX_MAX_ID_LEN];               /* identity ID (or "" if unknown) */
    char model_name[CBX_MAX_NAME_LEN];      /* controller model display name */
    char composite_path[CBX_MAX_PATH_LEN];  /* DBus object path */
} cbx_grid_composite_info;

/* A controller row in the grid. */
typedef struct {
    char id[CBX_MAX_ID_LEN];               /* controller identity */
    char model_name[CBX_MAX_NAME_LEN];      /* controller model name */
    char profile[CBX_GRID_PROFILE_LEN];     /* current profile name */
    int  cur_col;                            /* 0=Unassigned, 1+=Pn */
    char composite_path[CBX_MAX_PATH_LEN];  /* DBus path for LoadProfilePath */
} cbx_grid_row;

/* A column definition (player slot). */
typedef struct {
    char device_type[CBX_MAX_TYPE_LEN];     /* virtual device type (e.g. "xb360") */
} cbx_grid_col;

/* The full select grid state. */
typedef struct {
    cbx_grid_row rows[CBX_GRID_MAX_ROWS];
    int          row_count;
    cbx_grid_col cols[CBX_GRID_MAX_COLS];
    int          col_count;                  /* = num_virtual_controllers + 1 */

    /* Profile list for cycling (filenames without .yaml extension). */
    char profiles[CBX_GRID_MAX_PROFILES][CBX_GRID_PROFILE_LEN];
    int  profile_count;
} cbx_select_grid;

/* Render context for the grid render callback. */
typedef struct {
    cbx_select_grid   *grid;
    cbx_icon_cache    *icon_cache;     /* optional — NULL skips icons */
    const cbx_icon_map *icon_map;      /* optional — NULL skips icons */
    const cbx_theme   *theme;          /* optional — NULL uses defaults */
    cbx_text_cache    *text_cache;     /* optional — NULL skips text */
    int                font_id;        /* font ID for text cache */
    const struct cbx_conflict_list *conflicts; /* optional — NULL skips red */
    const cbx_settings *settings;      /* optional — NULL skips icon overrides */
    const cbx_host_mode *hm;           /* optional — NULL skips host-mode visuals */
} cbx_grid_render_ctx;

/* --- Lifecycle ---------------------------------------------------------- */

void cbx_select_grid_init(cbx_select_grid *g);

/* Build the grid from composite info, settings, and assignments.
 *
 * - Creates one row per composite (from composites array)
 * - Creates columns: Unassigned (col 0) + one per virtual controller
 *   (from settings.virtual_controllers)
 * - For each row, looks up the assignment by identity ID
 * - Sets cur_col from assignment slot (+1, or 0 if unassigned/not found)
 * - Sets profile from assignment (or "default" if not found)
 *
 * Returns 0 on success, -EINVAL on bad args. */
int cbx_select_grid_build(cbx_select_grid *g,
                          const cbx_grid_composite_info *composites,
                          int composite_count,
                          const cbx_settings *settings,
                          const cbx_assignments *assignments);

/* --- Profile list management -------------------------------------------- */

int  cbx_select_grid_add_profile(cbx_select_grid *g, const char *name);
void cbx_select_grid_clear_profiles(cbx_select_grid *g);

/* Find profile index by name.  Returns -1 if not found. */
int cbx_select_grid_find_profile(const cbx_select_grid *g,
                                   const char *name);

/* --- Navigation (pure data, no I/O) ------------------------------------ */

/* Move controller (row_idx) left/right across columns.
 * Returns 0 on success (position changed), -EINVAL on bad args,
 * -ERANGE if at boundary (can't move further). */
int cbx_select_grid_move_left(cbx_select_grid *g, int row_idx);
int cbx_select_grid_move_right(cbx_select_grid *g, int row_idx);

/* Cycle profile for controller (row_idx) up/down through the profile list.
 * Updates the row's profile field.
 * Returns 0 on success, -EINVAL on bad args, -ENOENT if no profiles. */
int cbx_select_grid_cycle_profile_up(cbx_select_grid *g, int row_idx);
int cbx_select_grid_cycle_profile_down(cbx_select_grid *g, int row_idx);

/* --- Accessors ---------------------------------------------------------- */

int cbx_select_grid_get_row_count(const cbx_select_grid *g);
int cbx_select_grid_get_col_count(const cbx_select_grid *g);
const cbx_grid_row *cbx_select_grid_get_row(const cbx_select_grid *g, int idx);
const cbx_grid_col *cbx_select_grid_get_col(const cbx_select_grid *g, int idx);
int  cbx_select_grid_get_cur_col(const cbx_select_grid *g, int row_idx);
const char *cbx_select_grid_get_profile(const cbx_select_grid *g, int row_idx);

/* Slot ↔ column conversion.
 * col 0 (Unassigned) → slot -1 (unassigned)
 * col 1 → slot 0 (P1), col 2 → slot 1 (P2), etc. */
int cbx_select_grid_col_to_slot(int col);
int cbx_select_grid_slot_to_col(int slot);

/* --- Rendering ---------------------------------------------------------- */

/* Render the grid into an SDL renderer.
 *
 * Draws:
 *   - Column headers (Unassigned, P1, P2, ...)
 *   - Each row: model name + profile label, cell backgrounds + icons,
 *     position indicator (filled circle on current cell)
 *   - Highlight rectangle on each controller's current column
 *
 * Returns 0 on success, negative errno on error.
 * NULL-safe: returns 0 if renderer or grid is NULL (no-op). */
int cbx_select_grid_render(SDL_Renderer *r,
                           const SDL_Rect *clip,
                           cbx_grid_render_ctx *ctx);

/* Render callback wrapper compatible with cbx_overlay_render_fn. */
int cbx_select_grid_render_cb(SDL_Renderer *r,
                               const SDL_Rect *clip,
                               void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* CBX_OVERLAY_GRID_RENDER_H */