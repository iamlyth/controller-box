/*
 * profile_diagram.h — SVG-based controller diagram with individually
 * highlightable buttons (Task 37, left panel).
 *
 * A custom cbx_widget that renders a controller outline (from an SVG
 * rasterized to an SDL_Texture, or a plain background rectangle if no
 * SVG is available) and can highlight individual buttons by drawing
 * a coloured overlay at their normalised position.
 *
 * Button positions are defined in a static lookup table keyed by
 * cbx_diag_button (which mirrors ip_input_id for buttons).  The table
 * uses normalised coordinates (0.0–1.0) relative to the diagram rect.
 *
 * The diagram and binding list stay synchronised: the editor calls
 * cbx_profile_diagram_highlight() whenever the list selection changes.
 *
 * Task 37 — Profile editor — controller diagram and binding list mode.
 */
#ifndef CBX_PROFILE_DIAGRAM_H
#define CBX_PROFILE_DIAGRAM_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#include "ui/widget.h"
#include "ui/theme.h"

/* ------------------------------------------------------------------ */
/*  Button identifiers (mirror ip_input_id for button-type inputs)     */
/* ------------------------------------------------------------------ */

typedef enum {
    CBX_DIAG_BTN_NONE = -1,
    CBX_DIAG_BTN_UP = 0,
    CBX_DIAG_BTN_DOWN,
    CBX_DIAG_BTN_LEFT,
    CBX_DIAG_BTN_RIGHT,
    CBX_DIAG_BTN_A,
    CBX_DIAG_BTN_B,
    CBX_DIAG_BTN_X,
    CBX_DIAG_BTN_Y,
    CBX_DIAG_BTN_START,
    CBX_DIAG_BTN_SELECT,
    CBX_DIAG_BTN_GUIDE,
    CBX_DIAG_BTN_L1,
    CBX_DIAG_BTN_R1,
    CBX_DIAG_BTN_L2,
    CBX_DIAG_BTN_R2,
    CBX_DIAG_BTN_L3,
    CBX_DIAG_BTN_R3,
    CBX_DIAG_BTN_COUNT
} cbx_diag_button;

/* ------------------------------------------------------------------ */
/*  Button position table entry                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    cbx_diag_button id;
    const char *name;       /* canonical name (e.g. "A", "Start") */
    float x, y, w, h;       /* normalised position within diagram (0.0–1.0) */
} cbx_diag_button_pos;

/* ------------------------------------------------------------------ */
/*  Diagram widget                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    cbx_widget base;
    SDL_Texture *base_texture;   /* controller image (owned if owns_base) */
    bool owns_base_texture;
    const cbx_diag_button_pos *btn_table; /* active button-position table */
    cbx_diag_button highlighted;  /* currently highlighted button, -1 = none */
    SDL_Color highlight_color;
    const cbx_theme *theme;       /* borrowed */
} cbx_profile_diagram;

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

/*
 * Initialise the diagram widget.  If svg_path is non-NULL and the file
 * exists, the SVG is rasterised to an SDL_Texture and used as the base
 * image.  If svg_path is NULL or loading fails, the widget draws a
 * plain background rectangle (useful for headless tests).
 *
 * @param diag     Output struct (overwritten).
 * @param renderer SDL renderer for texture creation (borrowed).
 * @param svg_path Path to a controller SVG, or NULL.
 * @param theme    Theme (borrowed, for colours).
 * @return 0 on success, -EINVAL on bad args.
 */
int cbx_profile_diagram_init(cbx_profile_diagram *diag,
                               SDL_Renderer *renderer,
                               const char *svg_path,
                               const cbx_theme *theme);

/*
 * Shut down and free resources.  Safe on a zeroed struct.
 */
void cbx_profile_diagram_shutdown(cbx_profile_diagram *diag);

/* ------------------------------------------------------------------ */
/*  Highlight control                                                  */
/* ------------------------------------------------------------------ */

/* Highlight a single button.  Pass CBX_DIAG_BTN_NONE to clear. */
void cbx_profile_diagram_highlight(cbx_profile_diagram *diag,
                                      cbx_diag_button btn);

/* Clear any highlight. */
void cbx_profile_diagram_clear_highlight(cbx_profile_diagram *diag);

/* Get the currently highlighted button (-1 = none). */
cbx_diag_button cbx_profile_diagram_get_highlight(
    const cbx_profile_diagram *diag);

/* ------------------------------------------------------------------ */
/*  Button position lookup                                             */
/* ------------------------------------------------------------------ */

/* Get the position entry for a button.  Returns NULL for invalid IDs. */
const cbx_diag_button_pos *cbx_profile_diagram_get_button_pos(
    cbx_diag_button btn);

/* Look up a button by canonical name (case-sensitive, matches ip_input
 * event strings like "A", "Start", "Up").  Returns CBX_DIAG_BTN_NONE
 * if the name is not recognised. */
cbx_diag_button cbx_profile_diagram_button_from_name(const char *name);

/* Get the canonical name for a button.  Returns NULL for invalid IDs. */
const char *cbx_profile_diagram_button_name(cbx_diag_button btn);

/* Total number of highlightable buttons (excludes NONE). */
int cbx_profile_diagram_button_count(void);

/* ------------------------------------------------------------------ */
/*  Device-mapped base image & marker layout (BUG-0018)               */
/* ------------------------------------------------------------------ */
/*
 * Adopt a base image texture that is owned by the production icon cache
 * (i.e. resolved through cbx_icon_lookup / cbx_icon_map / cbx_icon_cache)
 * rather than rasterised ad-hoc from a hand-built path.  The texture is
 * BORROWED: the icon cache keeps ownership and destroys it, so the diagram
 * must not.  Any texture the diagram currently owns is freed first.
 *
 * A NULL tex is a no-op (the diagram keeps whatever base it has).
 */
void cbx_profile_diagram_set_base_image(cbx_profile_diagram *diag,
                                        SDL_Texture *tex);

/*
 * Select the marker layout for a device icon name resolved through the
 * production icon map (e.g. "generic-gamepad" or "cc-xbox-360").  Only
 * icons with a registered, geometry-verified button-position table switch
 * the active table; every other/unknown name selects the generic table.
 * This keeps every marker anchored to a physical control on the rendered
 * asset (BUG-0018): a device silhouette whose control geometry is not yet
 * registered never gets markers that could float off the controls.
 */
void cbx_profile_diagram_set_device(cbx_profile_diagram *diag,
                                    const char *icon_name);

/*
 * Return true iff `icon_name` has a registered, geometry-verified
 * button-position table (i.e. it is safe to display that device's SVG
 * with markers aligned to its controls).  A NULL/empty name resolves to
 * the default generic-gamepad device, which is always registered.
 */
bool cbx_profile_diagram_device_geometry_known(const char *icon_name);

/* Active-table position lookup for a button.  Returns NULL for invalid IDs. */
const cbx_diag_button_pos *cbx_profile_diagram_active_button_pos(
    const cbx_profile_diagram *diag, cbx_diag_button btn);

/* ------------------------------------------------------------------ */
/*  Geometry helpers (BUG-0018)                                       */
/* ------------------------------------------------------------------ */

/*
 * Compute the on-screen content rectangle used to draw the base texture
 * within `rect`, preserving aspect ratio and centring (letterboxing).
 * Returns false if there is no base texture or `rect` is empty, in which
 * case `out` is left untouched.
 *
 * The production renderer (diag_draw) anchors every mapped-button marker
 * inside this same content rect, so a highlighted marker always lands on
 * the rendered control.  Tests use this helper to compute marker geometry
 * exactly as the renderer does (BUG-0018).
 */
bool cbx_profile_diagram_content_rect(const cbx_profile_diagram *diag,
                                      const SDL_Rect *rect,
                                      SDL_Rect *out);

/*
 * Return the base texture's raster dimensions (pixels) into *w and *h.
 * Returns false if there is no base texture. Used to verify that the
 * rasterised resolution is not below the displayed size (pixelation
 * guard) and that the aspect ratio is preserved (stretch guard).
 */
bool cbx_profile_diagram_base_texture_size(const cbx_profile_diagram *diag,
                                           int *w, int *h);

#endif /* CBX_PROFILE_DIAGRAM_H */