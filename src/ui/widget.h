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

#endif /* CBX_WIDGET_H */