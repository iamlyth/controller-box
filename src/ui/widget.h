/*
 * widget.h — Widget base struct with function-pointer vtable and concrete
 * widget declarations (Button, Label, Image, Panel).
 *
 * The base struct `cbx_widget` is embedded as the first member of every
 * concrete widget (C struct inheritance).  Generic code calls the dispatcher
 * functions (cbx_widget_draw, …) which forward through the vtable.
 *
 * Task 20 — Widget base and concrete widgets.
 */
#ifndef CBX_WIDGET_H
#define CBX_WIDGET_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#include "text.h"
#include "theme.h"

/* ------------------------------------------------------------------ */
/*  Base widget                                                       */
/* ------------------------------------------------------------------ */

typedef struct cbx_widget cbx_widget;

/** Vtable — concrete widgets populate this and assign to base.vt. */
typedef struct {
    void     (*draw)(cbx_widget *w, SDL_Renderer *r);
    bool     (*handle_event)(cbx_widget *w, const SDL_Event *ev);   /* true = consumed */
    void     (*focus)(cbx_widget *w);
    void     (*blur)(cbx_widget *w);
    void     (*get_rect)(const cbx_widget *w, SDL_Rect *out);
    void     (*set_rect)(cbx_widget *w, const SDL_Rect *rect);
    void     (*destroy)(cbx_widget *w);
} cbx_widget_vtable;

/** Base widget — embed as first member of every concrete widget. */
struct cbx_widget {
    const cbx_widget_vtable *vt;
    bool  focused;
    bool  visible;
    SDL_Rect rect;
};

/* Generic dispatchers (NULL-safe; no-op if vtable entry is NULL). */
void cbx_widget_draw(cbx_widget *w, SDL_Renderer *r);
bool cbx_widget_handle_event(cbx_widget *w, const SDL_Event *ev);
void cbx_widget_focus(cbx_widget *w);
void cbx_widget_blur(cbx_widget *w);
void cbx_widget_get_rect(const cbx_widget *w, SDL_Rect *out);
void cbx_widget_set_rect(cbx_widget *w, const SDL_Rect *rect);
void cbx_widget_destroy(cbx_widget *w);

/* Common accessors. */
bool cbx_widget_is_focused(const cbx_widget *w);
bool cbx_widget_is_visible(const cbx_widget *w);
void cbx_widget_set_visible(cbx_widget *w, bool visible);

/* ------------------------------------------------------------------ */
/*  Button                                                            */
/* ------------------------------------------------------------------ */

/** Press callback.  `w` is the button widget; `user_data` is opaque. */
typedef void (*cbx_button_press_cb)(cbx_widget *w, void *user_data);

#define CBX_BUTTON_LABEL_LEN 128

typedef struct {
    cbx_widget base;
    cbx_text_cache   *text_cache;   /* borrowed — not owned */
    const cbx_theme  *theme;        /* borrowed — not owned */
    char  label[CBX_BUTTON_LABEL_LEN];
    SDL_Texture *label_tex;        /* from text cache — not owned */
    int   label_w;
    int   label_h;
    int   font_id;
    bool  pressed;
    cbx_button_press_cb on_press;
    void *user_data;
} cbx_button;

/**
 * Initialise a button.  Renders the label via the text cache.
 * Returns 0, -EINVAL, or -ENOMEM.
 */
int  cbx_button_init(cbx_button *btn, const char *label, int font_id,
                     cbx_text_cache *cache, const cbx_theme *theme,
                     cbx_button_press_cb cb, void *user_data);

/** Update label text (re-renders via the text cache). */
void cbx_button_set_label(cbx_button *btn, const char *label);

/** Change the press callback / user data. */
void cbx_button_set_press_cb(cbx_button *btn, cbx_button_press_cb cb,
                              void *user_data);

/** Manually set the pressed visual state (for programmatic use). */
void cbx_button_set_pressed(cbx_button *btn, bool pressed);

/* ------------------------------------------------------------------ */
/*  Label                                                             */
/* ------------------------------------------------------------------ */

#define CBX_LABEL_TEXT_LEN 256

typedef struct {
    cbx_widget base;
    cbx_text_cache   *text_cache;   /* borrowed */
    const cbx_theme  *theme;        /* borrowed */
    char  text[CBX_LABEL_TEXT_LEN];
    int   font_id;
    SDL_Color color;
    bool  multiline;
} cbx_label;

/**
 * Initialise a label.  The label renders text on draw via the text cache.
 * Returns 0, -EINVAL, or -ENOMEM.
 */
int  cbx_label_init(cbx_label *lbl, const char *text, int font_id,
                    cbx_text_cache *cache, const cbx_theme *theme);

void cbx_label_set_text(cbx_label *lbl, const char *text);
void cbx_label_set_color(cbx_label *lbl, SDL_Color color);
void cbx_label_set_multiline(cbx_label *lbl, bool multiline);

/* ------------------------------------------------------------------ */
/*  Image                                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    CBX_IMAGE_SCALE_FIT,     /* preserve aspect, fit within rect */
    CBX_IMAGE_SCALE_FILL,    /* stretch to fill rect */
    CBX_IMAGE_SCALE_CENTER,  /* 1:1 centred within rect */
} cbx_image_scale_mode;

typedef struct {
    cbx_widget base;
    SDL_Texture *texture;
    int   tex_w;
    int   tex_h;
    cbx_image_scale_mode scale_mode;
    bool  owns_texture;   /* if true, destroy() frees the texture */
} cbx_image;

/**
 * Initialise an image widget wrapping an SDL_Texture.
 * If owns_texture is true the widget will SDL_DestroyTexture on destroy.
 * Returns 0, -EINVAL.
 */
int  cbx_image_init(cbx_image *img, SDL_Texture *texture, bool owns_texture);

void cbx_image_set_texture(cbx_image *img, SDL_Texture *texture,
                            bool owns_texture);
void cbx_image_set_scale_mode(cbx_image *img, cbx_image_scale_mode mode);
int  cbx_image_get_natural_dims(const cbx_image *img, int *w, int *h);

/* ------------------------------------------------------------------ */
/*  Panel (container)                                                 */
/* ------------------------------------------------------------------ */

#define CBX_PANEL_MAX_CHILDREN 32

typedef struct {
    cbx_widget base;
    const cbx_theme *theme;       /* borrowed */
    cbx_widget *children[CBX_PANEL_MAX_CHILDREN];
    int   child_count;
    int   focused_child;   /* index, -1 = none */
    int   padding;
    bool  draw_bg;
    bool  draw_border;
} cbx_panel;

/**
 * Initialise a panel.  Returns 0, -EINVAL.
 */
int  cbx_panel_init(cbx_panel *panel, const cbx_theme *theme);

/**
 * Add a child widget.  Panel does NOT own the child (caller manages lifetime).
 * Returns 0, -EINVAL, -ENOMEM (full).
 */
int  cbx_panel_add_child(cbx_panel *panel, cbx_widget *child);

/**
 * Remove a child widget.  Returns 0, -EINVAL, -ENOENT.
 */
int  cbx_panel_remove_child(cbx_panel *panel, cbx_widget *child);

int        cbx_panel_child_count(const cbx_panel *panel);
cbx_widget *cbx_panel_get_child(cbx_panel *panel, int index);
cbx_widget *cbx_panel_get_focused_child(cbx_panel *panel);

/** Focus management — returns index of newly focused child, or -1. */
int  cbx_panel_focus_first(cbx_panel *panel);
int  cbx_panel_focus_next(cbx_panel *panel);
int  cbx_panel_focus_prev(cbx_panel *panel);
void cbx_panel_clear_focus(cbx_panel *panel);

void cbx_panel_set_padding(cbx_panel *panel, int padding);
void cbx_panel_set_draw_bg(cbx_panel *panel, bool draw_bg);
void cbx_panel_set_draw_border(cbx_panel *panel, bool draw_border);

/* ------------------------------------------------------------------ */
/*  List (scrollable, up/down navigation, highlight, optional icon)  */
/* ------------------------------------------------------------------ */

#define CBX_LIST_MAX_ITEMS 64
#define CBX_LIST_LABEL_LEN 128

typedef struct {
    char   label[CBX_LIST_LABEL_LEN];
    SDL_Texture *icon;    /* optional — borrowed, not owned */
    void  *user_data;      /* opaque per-item data */
} cbx_list_item;

typedef void (*cbx_list_select_cb)(cbx_widget *w, int index,
                                    void *user_data);

typedef struct {
    cbx_widget base;
    cbx_text_cache   *text_cache;   /* borrowed */
    const cbx_theme  *theme;        /* borrowed */
    int   font_id;
    cbx_list_item items[CBX_LIST_MAX_ITEMS];
    int   item_count;
    int   selected;          /* highlighted index, -1 = none */
    int   scroll_offset;      /* first visible item */
    int   visible_count;     /* computed from rect height / item_h */
    int   item_h;            /* pixel height per item */
    int   icon_size;         /* icon dimension (square) */
    cbx_list_select_cb on_select;
} cbx_list;

int  cbx_list_init(cbx_list *lst, int font_id,
                    cbx_text_cache *cache, const cbx_theme *theme);

int  cbx_list_add_item(cbx_list *lst, const char *label,
                        SDL_Texture *icon, void *user_data);
void cbx_list_clear(cbx_list *lst);
int  cbx_list_item_count(const cbx_list *lst);
int  cbx_list_get_selected(const cbx_list *lst);
void cbx_list_set_selected(cbx_list *lst, int index);
int  cbx_list_scroll_up(cbx_list *lst);
int  cbx_list_scroll_down(cbx_list *lst);
void cbx_list_set_select_cb(cbx_list *lst, cbx_list_select_cb cb);

/* ------------------------------------------------------------------ */
/*  Grid (N rows × M columns, independent row/col nav)                */
/* ------------------------------------------------------------------ */

#define CBX_GRID_MAX_CELLS 256

typedef struct {
    cbx_widget base;
    const cbx_theme *theme;    /* borrowed */
    cbx_widget *cells[CBX_GRID_MAX_CELLS];
    int   cell_count;
    int   rows;
    int   cols;
    int   cur_row;
    int   cur_col;
    int   cell_w;       /* computed from rect / cols */
    int   cell_h;       /* computed from rect / rows */
} cbx_grid;

int  cbx_grid_init(cbx_grid *grid, const cbx_theme *theme);
int  cbx_grid_set_dims(cbx_grid *grid, int rows, int cols);
int  cbx_grid_set_cell(cbx_grid *grid, int row, int col,
                       cbx_widget *cell);
cbx_widget *cbx_grid_get_cell(cbx_grid *grid, int row, int col);
int  cbx_grid_get_cursor(const cbx_grid *grid, int *row, int *col);
int  cbx_grid_move_up(cbx_grid *grid);
int  cbx_grid_move_down(cbx_grid *grid);
int  cbx_grid_move_left(cbx_grid *grid);
int  cbx_grid_move_right(cbx_grid *grid);
void cbx_grid_clear(cbx_grid *grid);

/* ------------------------------------------------------------------ */
/*  TabBar (horizontal tabs, left/right, callback on change)         */
/* ------------------------------------------------------------------ */

#define CBX_TABBAR_MAX_TABS 16
#define CBX_TABBAR_LABEL_LEN 64

typedef void (*cbx_tabbar_change_cb)(cbx_widget *w, int new_tab,
                                       void *user_data);

typedef struct {
    char label[CBX_TABBAR_LABEL_LEN];
    void *user_data;
} cbx_tab;

typedef struct {
    cbx_widget base;
    cbx_text_cache   *text_cache;   /* borrowed */
    const cbx_theme  *theme;        /* borrowed */
    int   font_id;
    cbx_tab tabs[CBX_TABBAR_MAX_TABS];
    int   tab_count;
    int   active_tab;       /* index, -1 = none */
    cbx_tabbar_change_cb on_change;
} cbx_tabbar;

int  cbx_tabbar_init(cbx_tabbar *tb, int font_id,
                      cbx_text_cache *cache, const cbx_theme *theme);
int  cbx_tabbar_add_tab(cbx_tabbar *tb, const char *label, void *user_data);
int  cbx_tabbar_tab_count(const cbx_tabbar *tb);
int  cbx_tabbar_get_active(const cbx_tabbar *tb);
void cbx_tabbar_set_active(cbx_tabbar *tb, int index);
int  cbx_tabbar_move_left(cbx_tabbar *tb);
int  cbx_tabbar_move_right(cbx_tabbar *tb);
void cbx_tabbar_set_change_cb(cbx_tabbar *tb,
                                cbx_tabbar_change_cb cb);

/* ------------------------------------------------------------------ */
/*  ProgressBar (fill bar 0.0–1.0, configurable color)              */
/* ------------------------------------------------------------------ */

typedef struct {
    cbx_widget base;
    const cbx_theme *theme;    /* borrowed */
    double fraction;          /* 0.0 – 1.0 */
    SDL_Color bar_color;     /* configurable fill color */
    SDL_Color bg_color;      /* configurable bg color */
} cbx_progress;

int  cbx_progress_init(cbx_progress *prog, const cbx_theme *theme);
void cbx_progress_set_fraction(cbx_progress *prog, double frac);
double cbx_progress_get_fraction(const cbx_progress *prog);
void cbx_progress_set_bar_color(cbx_progress *prog, SDL_Color color);
void cbx_progress_set_bg_color(cbx_progress *prog, SDL_Color color);

#endif /* CBX_WIDGET_H */