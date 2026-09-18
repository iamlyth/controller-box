/*
 * focus.h — Focus chain manager with directional (spatial) navigation.
 *
 * Maintains a flat list of focusable widgets with their screen rectangles
 * and supports up/down/left/right navigation between them.  The manager
 * computes spatial neighbors: given a direction, it finds the nearest
 * widget whose centre is in that direction relative to the current widget.
 *
 * SPEC §4.3 (Player Mode): each controller navigates its own row only —
 * left/right moves between columns, up/down cycles within the row.
 * SPEC §4.4 (Host Mode): the host can navigate to any row — up/down moves
 * between rows, left/right moves between columns.
 *
 * The `row` field on each entry groups widgets by row.  In Player Mode,
 * up/down navigation is restricted to the same row; in Host Mode, it can
 * cross row boundaries.
 *
 * Task 22 — Focus chain system and input event mapping.
 */
#ifndef CBX_FOCUS_H
#define CBX_FOCUS_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#include "ui/widget.h"

/* --- Types --------------------------------------------------------------- */

typedef enum {
    CBX_FOCUS_MODE_PLAYER,     /* up/down restricted to current row */
    CBX_FOCUS_MODE_HOST,       /* up/down can cross rows */
} cbx_focus_mode;

typedef enum {
    CBX_NAV_UP,
    CBX_NAV_DOWN,
    CBX_NAV_LEFT,
    CBX_NAV_RIGHT,
} cbx_nav_direction;

typedef struct {
    cbx_widget *widget;        /* borrowed — not owned */
    SDL_Rect    rect;          /* current screen rectangle */
    int         row;           /* row group (default 0) */
} cbx_focus_entry;

#define CBX_FOCUS_MAX 64

typedef struct {
    cbx_focus_entry entries[CBX_FOCUS_MAX];
    int             count;
    int             focused;   /* index into entries, -1 = none */
    cbx_focus_mode  mode;
} cbx_focus_chain;

/* --- Lifecycle ----------------------------------------------------------- */

void cbx_focus_chain_init(cbx_focus_chain *chain);

/*
 * Add a focusable widget to the chain.
 * `rect` may be NULL (defaults to the widget's current rect).
 * `row` groups widgets for Player/Host mode (use 0 if irrelevant).
 * Returns the entry index, or -ENOMEM if full, -EINVAL on bad args.
 */
int  cbx_focus_chain_add(cbx_focus_chain *chain, cbx_widget *widget,
                          const SDL_Rect *rect, int row);

void cbx_focus_chain_clear(cbx_focus_chain *chain);

int  cbx_focus_chain_count(const cbx_focus_chain *chain);

/* --- Mode ---------------------------------------------------------------- */

void        cbx_focus_chain_set_mode(cbx_focus_chain *chain,
                                      cbx_focus_mode mode);
cbx_focus_mode cbx_focus_chain_get_mode(const cbx_focus_chain *chain);

/* --- Query --------------------------------------------------------------- */

int         cbx_focus_chain_get_focused(const cbx_focus_chain *chain);
cbx_widget *cbx_focus_chain_get_focused_widget(const cbx_focus_chain *chain);
const cbx_focus_entry *cbx_focus_chain_get_entry(
    const cbx_focus_chain *chain, int index);

/* --- Focus --------------------------------------------------------------- */

/*
 * Focus the entry at `index`.  Blurs the previously focused widget and
 * focuses the new one via the vtable.  Returns 0 on success, -EINVAL.
 */
int  cbx_focus_chain_focus(cbx_focus_chain *chain, int index);

/*
 * Focus the first entry in the chain.  Returns the index, or -1 if empty.
 */
int  cbx_focus_chain_focus_first(cbx_focus_chain *chain);

/*
 * Focus the entry whose widget matches.  Returns the index, or -ENOENT.
 */
int  cbx_focus_chain_focus_widget(cbx_focus_chain *chain,
                                    const cbx_widget *widget);

/*
 * Blur the currently focused entry (if any).  Returns 0.
 */
int  cbx_focus_chain_blur(cbx_focus_chain *chain);

/* --- Navigation ---------------------------------------------------------- */

/*
 * Navigate in a direction.  Computes the spatial nearest widget in the
 * given direction and focuses it.  In Player Mode, up/down is restricted
 * to the same row group as the currently focused entry.
 *
 * Returns the new focused index on success, or -1 if no candidate was
 * found in that direction (focus unchanged).
 */
int  cbx_focus_chain_navigate(cbx_focus_chain *chain,
                                cbx_nav_direction dir);

/* --- Rect update --------------------------------------------------------- */

/*
 * Update the stored rectangle for an entry (e.g., after layout change).
 * Returns 0 on success, -EINVAL.
 */
int  cbx_focus_chain_update_rect(cbx_focus_chain *chain, int index,
                                   const SDL_Rect *rect);

#endif /* CBX_FOCUS_H */