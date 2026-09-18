/*
 * theme.h — Colour theme system for Controller-Box (Task 19).
 *
 * Loads theme colours from the settings struct (SPEC §7.3: theme name +
 * overlay_opacity).  The "default" theme provides a dark palette
 * optimised for the overlay character-select screen.  The theme struct
 * holds all colours the widget toolkit and overlay surface need:
 *
 *   - Background / overlay background (with opacity applied)
 *   - Panel / card backgrounds
 *   - Text colours (primary, secondary, accent)
 *   - Border / separator colours
 *   - Focus highlight (for keyboard/controller navigation)
 *   - Conflict colour (red highlight for slot collisions, SPEC §4.5)
 *   - Icon tint (for recolourable SVG icons)
 *
 * The theme is applied by the renderer at init time and can be
 * re-applied if settings change.  Colours are stored as SDL_Colour
 * (0–255 per channel).  Overlay opacity is applied separately by the
 * renderer via SDL_SetRenderDrawColor alpha when drawing the overlay
 * background.
 */
#ifndef CBX_UI_THEME_H
#define CBX_UI_THEME_H

#include <SDL2/SDL.h>
#include "config/config_settings.h"

/* Maximum number of named themes. */
#define CBX_THEME_MAX 8

/* Theme name length (NUL-terminated). */
#define CBX_THEME_NAME_LEN 32

typedef struct {
    /*
     * Core background colours.
     * overlay_bg is the semi-transparent overlay backdrop; its alpha
     * channel is set from overlay_opacity at load time.
     */
    SDL_Color bg;             /* main background (opaque)              */
    SDL_Color overlay_bg;     /* overlay backdrop (alpha = opacity)    */
    SDL_Color panel_bg;       /* card / panel background               */
    SDL_Color panel_bg_hover; /* hovered panel background              */

    /*
     * Text colours.
     */
    SDL_Color text_primary;   /* primary text (white-ish)              */
    SDL_Color text_secondary;  /* secondary / muted text               */
    SDL_Color text_accent;     /* accent / highlighted text             */
    SDL_Color text_disabled;   /* disabled / placeholder text           */

    /*
     * Border / separator.
     */
    SDL_Color border;         /* normal border                         */
    SDL_Color border_focus;   /* focus border (brighter)               */

    /*
     * Functional colours.
     */
    SDL_Color focus;           /* focus highlight rectangle              */
    SDL_Color conflict;       /* red highlight for slot collisions     */
    SDL_Color success;        /* green for success / connected         */

    /*
     * Icon tint for recolourable SVG icons (SPEC §8.3).
     */
    SDL_Color icon_tint;      /* tint applied via SDL_SetTextureColorMod */
} cbx_theme;

/*
 * Load a theme by name from the settings struct.  Sets all colours in
 * the theme struct.  The overlay_bg alpha channel is set from
 * settings->overlay_opacity (clamped 0–255).  If the theme name is not
 * recognised, falls back to "default".
 *
 * @param theme    Output struct (overwritten).
 * @param settings  Application settings (provides theme name + opacity).
 * @return 0 on success, -EINVAL on NULL args.
 */
int cbx_theme_load(cbx_theme *theme, const cbx_settings *settings);

/*
 * Get the default dark theme.  Called internally by cbx_theme_load when
 * the theme name is "default" or unrecognised.
 */
void cbx_theme_default(cbx_theme *theme);

/*
 * Apply overlay opacity to the theme's overlay_bg colour.  Clamps
 * opacity to 0.0–1.0 and sets the alpha channel accordingly.
 * Called by cbx_theme_load; exposed for testing.
 */
void cbx_theme_apply_opacity(cbx_theme *theme, double opacity);

/*
 * Check if a theme name is known.  Returns true if the name matches a
 * built-in theme (currently only "default").
 */
bool cbx_theme_is_known(const char *name);

#endif /* CBX_UI_THEME_H */