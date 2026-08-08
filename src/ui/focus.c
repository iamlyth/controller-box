/*
 * focus.c — Focus chain manager with directional (spatial) navigation.
 *
 * Task 22 — Focus chain system and input event mapping.
 */
#include "ui/focus.h"

#include <errno.h>
#include <math.h>
#include <string.h>

/* --- Helpers ------------------------------------------------------------- */

static void
entry_center(const cbx_focus_entry *e, int *cx, int *cy)
{
    *cx = e->rect.x + e->rect.w / 2;
    *cy = e->rect.y + e->rect.h / 2;
}

/*
 * Check if candidate is in the given direction relative to current.
 * Uses centre-to-centre comparison.
 */
static bool
is_in_direction(cbx_nav_direction dir,
                  int cur_cx, int cur_cy,
                  int cand_cx, int cand_cy)
{
    switch (dir) {
    case CBX_NAV_UP:    return cand_cy < cur_cy;
    case CBX_NAV_DOWN:  return cand_cy > cur_cy;
    case CBX_NAV_LEFT:  return cand_cx < cur_cx;
    case CBX_NAV_RIGHT: return cand_cx > cur_cx;
    }
    return false;
}

/*
 * Compute the score (lower is better) for a candidate in the given
 * direction.  The score weights the primary axis distance more heavily
 * than the lateral offset, so that a directly-above candidate beats a
 * diagonally-above one at the same Euclidean distance.
 */
static double
direction_score(cbx_nav_direction dir,
                 int cur_cx, int cur_cy,
                 int cand_cx, int cand_cy)
{
    int dx = cand_cx - cur_cx;
    int dy = cand_cy - cur_cy;

    switch (dir) {
    case CBX_NAV_UP:
    case CBX_NAV_DOWN:
        /* primary = |dy|, lateral = |dx| */
        return (double)abs(dy) + (double)abs(dx) * 1.5;
    case CBX_NAV_LEFT:
    case CBX_NAV_RIGHT:
        /* primary = |dx|, lateral = |dy| */
        return (double)abs(dx) + (double)abs(dy) * 1.5;
    }
    return 1e18;
}

/*
 * Find the best spatial neighbor in the given direction.
 * In Player Mode, up/down is restricted to the same row.
 * Returns the entry index, or -1 if no candidate found.
 */
static int
find_neighbor(const cbx_focus_chain *chain, cbx_nav_direction dir)
{
    if (chain->focused < 0 || chain->focused >= chain->count)
        return -1;

    const cbx_focus_entry *cur = &chain->entries[chain->focused];
    int cur_cx, cur_cy;
    entry_center(cur, &cur_cx, &cur_cy);

    int  best_idx  = -1;
    double best_score = 1e18;

    for (int i = 0; i < chain->count; i++) {
        if (i == chain->focused)
            continue;

        const cbx_focus_entry *cand = &chain->entries[i];

        /* Player Mode: restrict up/down to the same row. */
        if (chain->mode == CBX_FOCUS_MODE_PLAYER &&
            (dir == CBX_NAV_UP || dir == CBX_NAV_DOWN) &&
            cand->row != cur->row)
            continue;

        /* Host Mode: restrict left/right to the same row so
         * horizontal navigation stays within a button group;
         * vertical navigation can still cross rows. */
        if (chain->mode == CBX_FOCUS_MODE_HOST &&
            (dir == CBX_NAV_LEFT || dir == CBX_NAV_RIGHT) &&
            cand->row != cur->row)
            continue;

        int cand_cx, cand_cy;
        entry_center(cand, &cand_cx, &cand_cy);

        if (!is_in_direction(dir, cur_cx, cur_cy, cand_cx, cand_cy))
            continue;

        double score = direction_score(dir, cur_cx, cur_cy,
                                         cand_cx, cand_cy);
        if (score < best_score) {
            best_score = score;
            best_idx   = i;
        }
    }

    return best_idx;
}

/* --- Lifecycle ----------------------------------------------------------- */

void
cbx_focus_chain_init(cbx_focus_chain *chain)
{
    if (!chain)
        return;
    memset(chain, 0, sizeof(*chain));
    chain->focused = -1;
    chain->mode    = CBX_FOCUS_MODE_PLAYER;
}

int
cbx_focus_chain_add(cbx_focus_chain *chain, cbx_widget *widget,
                     const SDL_Rect *rect, int row)
{
    if (!chain || !widget)
        return -EINVAL;
    if (chain->count >= CBX_FOCUS_MAX)
        return -ENOMEM;

    cbx_focus_entry *e = &chain->entries[chain->count];
    e->widget = widget;
    e->row    = row;

    if (rect) {
        e->rect = *rect;
    } else {
        cbx_widget_get_rect(widget, &e->rect);
    }

    return chain->count++;
}

void
cbx_focus_chain_clear(cbx_focus_chain *chain)
{
    if (!chain)
        return;
    chain->count   = 0;
    chain->focused = -1;
}

int
cbx_focus_chain_count(const cbx_focus_chain *chain)
{
    return chain ? chain->count : 0;
}

/* --- Mode ---------------------------------------------------------------- */

void
cbx_focus_chain_set_mode(cbx_focus_chain *chain, cbx_focus_mode mode)
{
    if (chain)
        chain->mode = mode;
}

cbx_focus_mode
cbx_focus_chain_get_mode(const cbx_focus_chain *chain)
{
    return chain ? chain->mode : CBX_FOCUS_MODE_PLAYER;
}

/* --- Query --------------------------------------------------------------- */

int
cbx_focus_chain_get_focused(const cbx_focus_chain *chain)
{
    return chain ? chain->focused : -1;
}

cbx_widget *
cbx_focus_chain_get_focused_widget(const cbx_focus_chain *chain)
{
    if (!chain || chain->focused < 0 || chain->focused >= chain->count)
        return NULL;
    return chain->entries[chain->focused].widget;
}

const cbx_focus_entry *
cbx_focus_chain_get_entry(const cbx_focus_chain *chain, int index)
{
    if (!chain || index < 0 || index >= chain->count)
        return NULL;
    return &chain->entries[index];
}

/* --- Focus --------------------------------------------------------------- */

int
cbx_focus_chain_focus(cbx_focus_chain *chain, int index)
{
    if (!chain || index < 0 || index >= chain->count)
        return -EINVAL;

    /* Blur the old. */
    if (chain->focused >= 0 && chain->focused < chain->count) {
        cbx_widget *old = chain->entries[chain->focused].widget;
        if (old)
            cbx_widget_blur(old);
    }

    chain->focused = index;

    /* Focus the new. */
    cbx_widget *w = chain->entries[index].widget;
    if (w)
        cbx_widget_focus(w);

    return 0;
}

int
cbx_focus_chain_focus_first(cbx_focus_chain *chain)
{
    if (!chain || chain->count == 0)
        return -1;
    return (cbx_focus_chain_focus(chain, 0) == 0) ? 0 : -1;
}

int
cbx_focus_chain_focus_widget(cbx_focus_chain *chain,
                               const cbx_widget *widget)
{
    if (!chain || !widget)
        return -EINVAL;

    for (int i = 0; i < chain->count; i++) {
        if (chain->entries[i].widget == widget)
            return (cbx_focus_chain_focus(chain, i) == 0) ? i : -1;
    }
    return -ENOENT;
}

int
cbx_focus_chain_blur(cbx_focus_chain *chain)
{
    if (!chain || chain->focused < 0)
        return 0;

    if (chain->focused < chain->count) {
        cbx_widget *w = chain->entries[chain->focused].widget;
        if (w)
            cbx_widget_blur(w);
    }

    chain->focused = -1;
    return 0;
}

/* --- Navigation ---------------------------------------------------------- */

int
cbx_focus_chain_navigate(cbx_focus_chain *chain, cbx_nav_direction dir)
{
    if (!chain || chain->focused < 0)
        return -1;

    int target = find_neighbor(chain, dir);
    if (target < 0)
        return -1;

    if (cbx_focus_chain_focus(chain, target) != 0)
        return -1;

    return target;
}

/* --- Rect update --------------------------------------------------------- */

int
cbx_focus_chain_update_rect(cbx_focus_chain *chain, int index,
                              const SDL_Rect *rect)
{
    if (!chain || index < 0 || index >= chain->count || !rect)
        return -EINVAL;

    chain->entries[index].rect = *rect;
    return 0;
}