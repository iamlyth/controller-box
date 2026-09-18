/*
 * test_focus.c — Tests for the focus chain manager with directional
 * (spatial) navigation.
 *
 * Uses lightweight dummy widgets (stack-allocated) with the base vtable
 * to test focus/blur and navigation without needing a renderer.
 *
 * Task 22 — Focus chain system and input event mapping.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <errno.h>
#include <string.h>

#include "ui/widget.h"
#include "ui/focus.h"

/* --- dummy widget for testing -------------------------------------------- */

/*
 * Minimal concrete widget that records focus/blur calls.
 * Uses the base widget vtable dispatchers which call through the vtable.
 */
static void dummy_focus(cbx_widget *w)   { w->focused = true; }
static void dummy_blur(cbx_widget *w)     { w->focused = false; }

static void dummy_get_rect(const cbx_widget *w, SDL_Rect *out) {
    *out = w->rect;
}

static void dummy_set_rect(cbx_widget *w, const SDL_Rect *rect) {
    w->rect = *rect;
}

static const cbx_widget_vtable s_dummy_vt = {
    .draw       = NULL,
    .handle_event = NULL,
    .focus      = dummy_focus,
    .blur       = dummy_blur,
    .get_rect   = dummy_get_rect,
    .set_rect   = dummy_set_rect,
    .destroy    = NULL,
};

static void
make_dummy(cbx_widget *w, int x, int y, int rw, int rh)
{
    memset(w, 0, sizeof(*w));
    w->vt      = &s_dummy_vt;
    w->focused = false;
    w->visible = true;
    w->rect.x  = x;
    w->rect.y  = y;
    w->rect.w  = rw;
    w->rect.h  = rh;
}

/* --- init / basic tests --------------------------------------------------- */

static void
test_init(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);
    assert_int_equal(chain.count, 0);
    assert_int_equal(chain.focused, -1);
    assert_int_equal(chain.mode, CBX_FOCUS_MODE_PLAYER);
}

static void
test_init_null(void **state)
{
    (void)state;
    cbx_focus_chain_init(NULL);   /* should not crash */
}

static void
test_add(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget w;
    make_dummy(&w, 0, 0, 100, 50);

    int idx = cbx_focus_chain_add(&chain, &w, NULL, 0);
    assert_int_equal(idx, 0);
    assert_int_equal(chain.count, 1);
    assert_int_equal(chain.entries[0].widget, &w);
    assert_int_equal(chain.entries[0].row, 0);
    assert_int_equal(chain.entries[0].rect.w, 100);
}

static void
test_add_with_rect(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget w;
    make_dummy(&w, 0, 0, 10, 10);

    SDL_Rect custom = { 10, 20, 30, 40 };
    int idx = cbx_focus_chain_add(&chain, &w, &custom, 2);
    assert_int_equal(idx, 0);
    assert_int_equal(chain.entries[0].rect.x, 10);
    assert_int_equal(chain.entries[0].rect.y, 20);
    assert_int_equal(chain.entries[0].rect.w, 30);
    assert_int_equal(chain.entries[0].rect.h, 40);
    assert_int_equal(chain.entries[0].row, 2);
}

static void
test_add_null_args(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    assert_int_equal(cbx_focus_chain_add(NULL, NULL, NULL, 0), -EINVAL);
    assert_int_equal(cbx_focus_chain_add(&chain, NULL, NULL, 0), -EINVAL);
}

static void
test_add_overflow(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget widgets[CBX_FOCUS_MAX + 1];
    for (int i = 0; i < CBX_FOCUS_MAX; i++) {
        make_dummy(&widgets[i], i * 10, 0, 10, 10);
        int idx = cbx_focus_chain_add(&chain, &widgets[i], NULL, 0);
        assert_int_equal(idx, i);
    }
    assert_int_equal(chain.count, CBX_FOCUS_MAX);

    make_dummy(&widgets[CBX_FOCUS_MAX], 0, 0, 10, 10);
    int idx = cbx_focus_chain_add(&chain, &widgets[CBX_FOCUS_MAX], NULL, 0);
    assert_int_equal(idx, -ENOMEM);
}

static void
test_clear(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget w;
    make_dummy(&w, 0, 0, 10, 10);
    cbx_focus_chain_add(&chain, &w, NULL, 0);
    cbx_focus_chain_focus_first(&chain);
    assert_int_equal(chain.count, 1);
    assert_int_equal(chain.focused, 0);

    cbx_focus_chain_clear(&chain);
    assert_int_equal(chain.count, 0);
    assert_int_equal(chain.focused, -1);
}

static void
test_count(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);
    assert_int_equal(cbx_focus_chain_count(&chain), 0);

    cbx_widget w1, w2;
    make_dummy(&w1, 0, 0, 10, 10);
    make_dummy(&w2, 10, 0, 10, 10);
    cbx_focus_chain_add(&chain, &w1, NULL, 0);
    cbx_focus_chain_add(&chain, &w2, NULL, 0);
    assert_int_equal(cbx_focus_chain_count(&chain), 2);
    assert_int_equal(cbx_focus_chain_count(NULL), 0);
}

/* --- mode tests ---------------------------------------------------------- */

static void
test_set_get_mode(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);
    assert_int_equal(cbx_focus_chain_get_mode(&chain), CBX_FOCUS_MODE_PLAYER);

    cbx_focus_chain_set_mode(&chain, CBX_FOCUS_MODE_HOST);
    assert_int_equal(cbx_focus_chain_get_mode(&chain), CBX_FOCUS_MODE_HOST);

    cbx_focus_chain_set_mode(&chain, CBX_FOCUS_MODE_PLAYER);
    assert_int_equal(cbx_focus_chain_get_mode(&chain), CBX_FOCUS_MODE_PLAYER);
}

static void
test_get_mode_null(void **state)
{
    (void)state;
    assert_int_equal(cbx_focus_chain_get_mode(NULL), CBX_FOCUS_MODE_PLAYER);
}

/* --- focus / blur tests -------------------------------------------------- */

static void
test_focus_first(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget w1, w2;
    make_dummy(&w1, 0, 0, 10, 10);
    make_dummy(&w2, 20, 0, 10, 10);
    cbx_focus_chain_add(&chain, &w1, NULL, 0);
    cbx_focus_chain_add(&chain, &w2, NULL, 0);

    int idx = cbx_focus_chain_focus_first(&chain);
    assert_int_equal(idx, 0);
    assert_int_equal(chain.focused, 0);
    assert_true(w1.focused);
    assert_false(w2.focused);
}

static void
test_focus_first_empty(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);
    assert_int_equal(cbx_focus_chain_focus_first(&chain), -1);
}

static void
test_focus_by_index(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget w1, w2, w3;
    make_dummy(&w1, 0, 0, 10, 10);
    make_dummy(&w2, 20, 0, 10, 10);
    make_dummy(&w3, 40, 0, 10, 10);
    cbx_focus_chain_add(&chain, &w1, NULL, 0);
    cbx_focus_chain_add(&chain, &w2, NULL, 0);
    cbx_focus_chain_add(&chain, &w3, NULL, 0);

    assert_int_equal(cbx_focus_chain_focus(&chain, 1), 0);
    assert_int_equal(chain.focused, 1);
    assert_true(w2.focused);
    assert_false(w1.focused);
    assert_false(w3.focused);
}

static void
test_focus_invalid_index(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget w;
    make_dummy(&w, 0, 0, 10, 10);
    cbx_focus_chain_add(&chain, &w, NULL, 0);

    assert_int_equal(cbx_focus_chain_focus(&chain, -1), -EINVAL);
    assert_int_equal(cbx_focus_chain_focus(&chain, 1), -EINVAL);
    assert_int_equal(cbx_focus_chain_focus(NULL, 0), -EINVAL);
}

static void
test_focus_switches_blur(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget w1, w2;
    make_dummy(&w1, 0, 0, 10, 10);
    make_dummy(&w2, 20, 0, 10, 10);
    cbx_focus_chain_add(&chain, &w1, NULL, 0);
    cbx_focus_chain_add(&chain, &w2, NULL, 0);

    cbx_focus_chain_focus(&chain, 0);
    assert_true(w1.focused);

    cbx_focus_chain_focus(&chain, 1);
    assert_false(w1.focused);
    assert_true(w2.focused);
}

static void
test_focus_widget(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget w1, w2;
    make_dummy(&w1, 0, 0, 10, 10);
    make_dummy(&w2, 20, 0, 10, 10);
    cbx_focus_chain_add(&chain, &w1, NULL, 0);
    cbx_focus_chain_add(&chain, &w2, NULL, 0);

    int idx = cbx_focus_chain_focus_widget(&chain, &w2);
    assert_int_equal(idx, 1);
    assert_int_equal(chain.focused, 1);
    assert_true(w2.focused);
}

static void
test_focus_widget_not_found(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget w1, w2;
    make_dummy(&w1, 0, 0, 10, 10);
    make_dummy(&w2, 20, 0, 10, 10);
    cbx_focus_chain_add(&chain, &w1, NULL, 0);

    assert_int_equal(cbx_focus_chain_focus_widget(&chain, &w2), -ENOENT);
}

static void
test_blur(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget w;
    make_dummy(&w, 0, 0, 10, 10);
    cbx_focus_chain_add(&chain, &w, NULL, 0);
    cbx_focus_chain_focus_first(&chain);
    assert_true(w.focused);

    cbx_focus_chain_blur(&chain);
    assert_int_equal(chain.focused, -1);
    assert_false(w.focused);
}

static void
test_blur_no_focus(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);
    assert_int_equal(cbx_focus_chain_blur(&chain), 0);
}

static void
test_get_focused_widget(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    assert_null(cbx_focus_chain_get_focused_widget(&chain));

    cbx_widget w1, w2;
    make_dummy(&w1, 0, 0, 10, 10);
    make_dummy(&w2, 20, 0, 10, 10);
    cbx_focus_chain_add(&chain, &w1, NULL, 0);
    cbx_focus_chain_add(&chain, &w2, NULL, 0);
    cbx_focus_chain_focus(&chain, 1);

    assert_ptr_equal(cbx_focus_chain_get_focused_widget(&chain), &w2);
}

static void
test_get_focused(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);
    assert_int_equal(cbx_focus_chain_get_focused(&chain), -1);

    cbx_widget w;
    make_dummy(&w, 0, 0, 10, 10);
    cbx_focus_chain_add(&chain, &w, NULL, 0);
    cbx_focus_chain_focus_first(&chain);
    assert_int_equal(cbx_focus_chain_get_focused(&chain), 0);
}

static void
test_get_entry(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget w;
    make_dummy(&w, 10, 20, 30, 40);
    cbx_focus_chain_add(&chain, &w, NULL, 1);

    const cbx_focus_entry *e = cbx_focus_chain_get_entry(&chain, 0);
    assert_non_null(e);
    assert_ptr_equal(e->widget, &w);
    assert_int_equal(e->rect.x, 10);
    assert_int_equal(e->row, 1);

    assert_null(cbx_focus_chain_get_entry(&chain, 1));
    assert_null(cbx_focus_chain_get_entry(NULL, 0));
}

/* --- navigation tests (single row) -------------------------------------- */

static void
test_navigate_right(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget w1, w2, w3;
    make_dummy(&w1, 0,   0, 50, 50);
    make_dummy(&w2, 100, 0, 50, 50);
    make_dummy(&w3, 200, 0, 50, 50);
    cbx_focus_chain_add(&chain, &w1, NULL, 0);
    cbx_focus_chain_add(&chain, &w2, NULL, 0);
    cbx_focus_chain_add(&chain, &w3, NULL, 0);

    cbx_focus_chain_focus_first(&chain);     /* focus w1 */
    int idx = cbx_focus_chain_navigate(&chain, CBX_NAV_RIGHT);
    assert_int_equal(idx, 1);               /* w2 */
    assert_true(w2.focused);
    assert_false(w1.focused);

    idx = cbx_focus_chain_navigate(&chain, CBX_NAV_RIGHT);
    assert_int_equal(idx, 2);               /* w3 */
    assert_true(w3.focused);
}

static void
test_navigate_left(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget w1, w2, w3;
    make_dummy(&w1, 0,   0, 50, 50);
    make_dummy(&w2, 100, 0, 50, 50);
    make_dummy(&w3, 200, 0, 50, 50);
    cbx_focus_chain_add(&chain, &w1, NULL, 0);
    cbx_focus_chain_add(&chain, &w2, NULL, 0);
    cbx_focus_chain_add(&chain, &w3, NULL, 0);

    cbx_focus_chain_focus(&chain, 2);         /* focus w3 */
    int idx = cbx_focus_chain_navigate(&chain, CBX_NAV_LEFT);
    assert_int_equal(idx, 1);               /* w2 */
    assert_true(w2.focused);

    idx = cbx_focus_chain_navigate(&chain, CBX_NAV_LEFT);
    assert_int_equal(idx, 0);               /* w1 */
    assert_true(w1.focused);
}

static void
test_navigate_right_at_boundary(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget w1, w2;
    make_dummy(&w1, 0,   0, 50, 50);
    make_dummy(&w2, 100, 0, 50, 50);
    cbx_focus_chain_add(&chain, &w1, NULL, 0);
    cbx_focus_chain_add(&chain, &w2, NULL, 0);

    cbx_focus_chain_focus(&chain, 1);         /* focus w2 (rightmost) */
    int idx = cbx_focus_chain_navigate(&chain, CBX_NAV_RIGHT);
    assert_int_equal(idx, -1);              /* no candidate to the right */
    assert_int_equal(chain.focused, 1);     /* unchanged */
}

static void
test_navigate_left_at_boundary(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget w1, w2;
    make_dummy(&w1, 0,   0, 50, 50);
    make_dummy(&w2, 100, 0, 50, 50);
    cbx_focus_chain_add(&chain, &w1, NULL, 0);
    cbx_focus_chain_add(&chain, &w2, NULL, 0);

    cbx_focus_chain_focus_first(&chain);     /* focus w1 (leftmost) */
    int idx = cbx_focus_chain_navigate(&chain, CBX_NAV_LEFT);
    assert_int_equal(idx, -1);
    assert_int_equal(chain.focused, 0);
}

static void
test_navigate_no_focus(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget w;
    make_dummy(&w, 0, 0, 50, 50);
    cbx_focus_chain_add(&chain, &w, NULL, 0);

    /* No focus set — navigate should return -1. */
    assert_int_equal(cbx_focus_chain_navigate(&chain, CBX_NAV_RIGHT), -1);
}

static void
test_navigate_null_chain(void **state)
{
    (void)state;
    assert_int_equal(cbx_focus_chain_navigate(NULL, CBX_NAV_UP), -1);
}

/* --- navigation tests (multi-row, Player vs Host) ------------------------ */

static void
test_navigate_down_host_mode(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);
    cbx_focus_chain_set_mode(&chain, CBX_FOCUS_MODE_HOST);

    /* 3 rows × 3 columns. */
    cbx_widget widgets[9];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            make_dummy(&widgets[r * 3 + c],
                        c * 100, r * 100, 50, 50);
            cbx_focus_chain_add(&chain, &widgets[r * 3 + c], NULL, r);
        }
    }

    /* Focus row 0, col 1 (index 1). */
    cbx_focus_chain_focus(&chain, 1);

    /* Down should go to row 1, col 1 (index 4). */
    int idx = cbx_focus_chain_navigate(&chain, CBX_NAV_DOWN);
    assert_int_equal(idx, 4);
    assert_true(widgets[4].focused);

    /* Down again → row 2, col 1 (index 7). */
    idx = cbx_focus_chain_navigate(&chain, CBX_NAV_DOWN);
    assert_int_equal(idx, 7);
    assert_true(widgets[7].focused);

    /* Down at boundary → -1. */
    idx = cbx_focus_chain_navigate(&chain, CBX_NAV_DOWN);
    assert_int_equal(idx, -1);
}

static void
test_navigate_up_host_mode(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);
    cbx_focus_chain_set_mode(&chain, CBX_FOCUS_MODE_HOST);

    cbx_widget widgets[9];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            make_dummy(&widgets[r * 3 + c],
                        c * 100, r * 100, 50, 50);
            cbx_focus_chain_add(&chain, &widgets[r * 3 + c], NULL, r);
        }
    }

    /* Focus row 2, col 0 (index 6). */
    cbx_focus_chain_focus(&chain, 6);

    int idx = cbx_focus_chain_navigate(&chain, CBX_NAV_UP);
    assert_int_equal(idx, 3);   /* row 1, col 0 */
}

static void
test_navigate_down_player_mode_restricted(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);
    /* Player mode is default. */
    assert_int_equal(chain.mode, CBX_FOCUS_MODE_PLAYER);

    cbx_widget widgets[6];
    /* Row 0: indices 0,1,2; Row 1: indices 3,4,5. */
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 3; c++) {
            make_dummy(&widgets[r * 3 + c],
                        c * 100, r * 100, 50, 50);
            cbx_focus_chain_add(&chain, &widgets[r * 3 + c], NULL, r);
        }
    }

    /* Focus row 0, col 1 (index 1). */
    cbx_focus_chain_focus(&chain, 1);

    /* Down in Player mode should stay in row 0 — no candidate below in
     * the same row, so returns -1. */
    int idx = cbx_focus_chain_navigate(&chain, CBX_NAV_DOWN);
    assert_int_equal(idx, -1);
    assert_int_equal(chain.focused, 1);     /* unchanged */
}

static void
test_navigate_down_player_mode_same_row(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);
    /* Player mode. */

    /* Two widgets in the same row (row 0), vertically stacked. */
    cbx_widget w1, w2;
    make_dummy(&w1, 0, 0,   50, 50);
    make_dummy(&w2, 0, 100, 50, 50);
    cbx_focus_chain_add(&chain, &w1, NULL, 0);
    cbx_focus_chain_add(&chain, &w2, NULL, 0);

    cbx_focus_chain_focus_first(&chain);     /* focus w1 */
    int idx = cbx_focus_chain_navigate(&chain, CBX_NAV_DOWN);
    assert_int_equal(idx, 1);               /* w2 — same row, allowed */
    assert_true(w2.focused);
}

static void
test_navigate_up_player_mode_restricted(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget widgets[6];
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 3; c++) {
            make_dummy(&widgets[r * 3 + c],
                        c * 100, r * 100, 50, 50);
            cbx_focus_chain_add(&chain, &widgets[r * 3 + c], NULL, r);
        }
    }

    /* Focus row 1, col 1 (index 4). */
    cbx_focus_chain_focus(&chain, 4);

    /* Up in Player mode should stay in row 1 — no candidate above in
     * the same row, so returns -1. */
    int idx = cbx_focus_chain_navigate(&chain, CBX_NAV_UP);
    assert_int_equal(idx, -1);
}

static void
test_navigate_left_right_ignore_mode(void **state)
{
    (void)state;
    /* Left/right should work the same in both modes (no row restriction). */
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);
    cbx_focus_chain_set_mode(&chain, CBX_FOCUS_MODE_PLAYER);

    cbx_widget w1, w2;
    make_dummy(&w1, 0,   0, 50, 50);
    make_dummy(&w2, 100, 0, 50, 50);
    cbx_focus_chain_add(&chain, &w1, NULL, 0);
    cbx_focus_chain_add(&chain, &w2, NULL, 1);  /* different row! */

    cbx_focus_chain_focus_first(&chain);
    int idx = cbx_focus_chain_navigate(&chain, CBX_NAV_RIGHT);
    assert_int_equal(idx, 1);   /* works despite different rows */
}

static void
test_navigate_picks_nearest(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    /* Current at (0,0). Two candidates to the right:
     *   w2 at (100, 0) — directly right, distance 100
     *   w3 at (200, 50) — right and down, distance > 100
     * Navigate right should pick w2 (nearest). */
    cbx_widget w1, w2, w3;
    make_dummy(&w1, 0,   0, 50, 50);
    make_dummy(&w2, 100, 0, 50, 50);
    make_dummy(&w3, 200, 50, 50, 50);
    cbx_focus_chain_add(&chain, &w1, NULL, 0);
    cbx_focus_chain_add(&chain, &w2, NULL, 0);
    cbx_focus_chain_add(&chain, &w3, NULL, 0);

    cbx_focus_chain_focus_first(&chain);
    int idx = cbx_focus_chain_navigate(&chain, CBX_NAV_RIGHT);
    assert_int_equal(idx, 1);   /* w2 is nearest to the right */
}

static void
test_navigate_diagonal_prefers_aligned(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    /* Current at (50,50) centre. Two candidates below:
     *   w2 at (0, 100) centre=(25,125) — directly below-left
     *   w3 at (100, 100) centre=(125,125) — directly below-right
     * Both are at the same vertical distance. The score weights lateral
     * offset, so the one with less lateral offset wins.
     * Current centre = (75, 75).
     * w2 centre = (25, 125): primary=50, lateral=50, score=50+75=125
     * w3 centre = (125, 125): primary=50, lateral=50, score=50+75=125
     * Tie — first found wins (index 1). */
    cbx_widget w1, w2, w3;
    make_dummy(&w1, 50,  50,  50, 50);   /* centre (75, 75) */
    make_dummy(&w2, 0,   100, 50, 50);   /* centre (25, 125) */
    make_dummy(&w3, 100, 100, 50, 50);   /* centre (125, 125) */
    cbx_focus_chain_add(&chain, &w1, NULL, 0);
    cbx_focus_chain_add(&chain, &w2, NULL, 0);
    cbx_focus_chain_add(&chain, &w3, NULL, 0);

    cbx_focus_chain_focus_first(&chain);
    int idx = cbx_focus_chain_navigate(&chain, CBX_NAV_DOWN);
    /* Both at same score; first (index 1) should win. */
    assert_int_equal(idx, 1);
}

static void
test_navigate_host_mode_crosses_rows(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);
    cbx_focus_chain_set_mode(&chain, CBX_FOCUS_MODE_HOST);

    /* Row 0: widget at (0, 0). Row 1: widget at (0, 100). */
    cbx_widget w1, w2;
    make_dummy(&w1, 0, 0,   50, 50);
    make_dummy(&w2, 0, 100, 50, 50);
    cbx_focus_chain_add(&chain, &w1, NULL, 0);
    cbx_focus_chain_add(&chain, &w2, NULL, 1);

    cbx_focus_chain_focus_first(&chain);     /* focus w1 (row 0) */
    int idx = cbx_focus_chain_navigate(&chain, CBX_NAV_DOWN);
    assert_int_equal(idx, 1);               /* w2 in row 1 — allowed in Host */
    assert_true(w2.focused);
}

/* --- rect update tests --------------------------------------------------- */

static void
test_update_rect(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget w;
    make_dummy(&w, 0, 0, 50, 50);
    cbx_focus_chain_add(&chain, &w, NULL, 0);

    SDL_Rect new_rect = { 10, 20, 100, 200 };
    assert_int_equal(cbx_focus_chain_update_rect(&chain, 0, &new_rect), 0);
    assert_int_equal(chain.entries[0].rect.x, 10);
    assert_int_equal(chain.entries[0].rect.w, 100);
}

static void
test_update_rect_invalid(void **state)
{
    (void)state;
    cbx_focus_chain chain;
    cbx_focus_chain_init(&chain);

    cbx_widget w;
    make_dummy(&w, 0, 0, 50, 50);
    cbx_focus_chain_add(&chain, &w, NULL, 0);

    SDL_Rect r = { 0, 0, 10, 10 };
    assert_int_equal(cbx_focus_chain_update_rect(&chain, -1, &r), -EINVAL);
    assert_int_equal(cbx_focus_chain_update_rect(&chain, 1, &r), -EINVAL);
    assert_int_equal(cbx_focus_chain_update_rect(NULL, 0, &r), -EINVAL);
    assert_int_equal(cbx_focus_chain_update_rect(&chain, 0, NULL), -EINVAL);
}

/* --- main ---------------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* init / basic */
        cmocka_unit_test(test_init),
        cmocka_unit_test(test_init_null),
        cmocka_unit_test(test_add),
        cmocka_unit_test(test_add_with_rect),
        cmocka_unit_test(test_add_null_args),
        cmocka_unit_test(test_add_overflow),
        cmocka_unit_test(test_clear),
        cmocka_unit_test(test_count),

        /* mode */
        cmocka_unit_test(test_set_get_mode),
        cmocka_unit_test(test_get_mode_null),

        /* focus / blur */
        cmocka_unit_test(test_focus_first),
        cmocka_unit_test(test_focus_first_empty),
        cmocka_unit_test(test_focus_by_index),
        cmocka_unit_test(test_focus_invalid_index),
        cmocka_unit_test(test_focus_switches_blur),
        cmocka_unit_test(test_focus_widget),
        cmocka_unit_test(test_focus_widget_not_found),
        cmocka_unit_test(test_blur),
        cmocka_unit_test(test_blur_no_focus),
        cmocka_unit_test(test_get_focused_widget),
        cmocka_unit_test(test_get_focused),
        cmocka_unit_test(test_get_entry),

        /* navigation (single row) */
        cmocka_unit_test(test_navigate_right),
        cmocka_unit_test(test_navigate_left),
        cmocka_unit_test(test_navigate_right_at_boundary),
        cmocka_unit_test(test_navigate_left_at_boundary),
        cmocka_unit_test(test_navigate_no_focus),
        cmocka_unit_test(test_navigate_null_chain),

        /* navigation (multi-row, Player vs Host) */
        cmocka_unit_test(test_navigate_down_host_mode),
        cmocka_unit_test(test_navigate_up_host_mode),
        cmocka_unit_test(test_navigate_down_player_mode_restricted),
        cmocka_unit_test(test_navigate_down_player_mode_same_row),
        cmocka_unit_test(test_navigate_up_player_mode_restricted),
        cmocka_unit_test(test_navigate_left_right_ignore_mode),
        cmocka_unit_test(test_navigate_picks_nearest),
        cmocka_unit_test(test_navigate_diagonal_prefers_aligned),
        cmocka_unit_test(test_navigate_host_mode_crosses_rows),

        /* rect update */
        cmocka_unit_test(test_update_rect),
        cmocka_unit_test(test_update_rect_invalid),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}