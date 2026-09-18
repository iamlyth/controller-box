/*
 * theme.c — Colour theme system for Controller-Box (Task 19).
 *
 * Implements the theme loader.  Currently only the "default" dark theme
 * is defined.  The theme struct is populated from a static colour table
 * and the overlay opacity from settings is applied to the overlay
 * background alpha channel.
 *
 * Future themes can be added by extending the switch in cbx_theme_load()
 * and adding a corresponding cbx_theme_<name>() function.  The theme
 * format is TBD per SPEC §12, so this implementation keeps it simple:
 * hard-coded colour palettes for built-in themes.
 */
#include "ui/theme.h"

#include <string.h>
#include <stdbool.h>
#include <errno.h>

/*
 * Default dark theme — a fighting-game-style dark palette.
 * Colours are tuned for contrast on a dark overlay background.
 */
void cbx_theme_default(cbx_theme *t)
{
    /* Core backgrounds. */
    t->bg         = (SDL_Color){ 18,  18,  28, 255 };  /* near-black blue */
    t->overlay_bg = (SDL_Color){ 10,  10,  20, 217 };  /* alpha set from opacity */
    t->panel_bg   = (SDL_Color){ 30,  30,  42, 255 };
    t->panel_bg_hover = (SDL_Color){ 42, 42, 58, 255 };

    /* Text. */
    t->text_primary   = (SDL_Color){ 240, 240, 245, 255 };
    t->text_secondary = (SDL_Color){ 160, 160, 175, 255 };
    t->text_accent    = (SDL_Color){ 100, 180, 255, 255 };
    t->text_disabled  = (SDL_Color){  90,  90, 100, 255 };

    /* Borders / separators. */
    t->border       = (SDL_Color){ 50,  50,  65, 255 };
    t->border_focus = (SDL_Color){ 100, 180, 255, 255 };

    /* Functional. */
    t->focus    = (SDL_Color){ 100, 180, 255, 180 };
    t->conflict = (SDL_Color){ 220,  60,  60, 255 };
    t->success  = (SDL_Color){  80, 200, 100, 255 };

    /* Icon tint (white = no tint change, icons keep their colours). */
    t->icon_tint = (SDL_Color){ 255, 255, 255, 255 };
}

void cbx_theme_apply_opacity(cbx_theme *t, double opacity)
{
    if (!t) return;
    if (opacity < 0.0) opacity = 0.0;
    if (opacity > 1.0) opacity = 1.0;
    t->overlay_bg.a = (Uint8)(opacity * 255.0 + 0.5);
}

bool cbx_theme_is_known(const char *name)
{
    if (!name) return false;
    return strcmp(name, "default") == 0;
}

int cbx_theme_load(cbx_theme *theme, const cbx_settings *settings)
{
    if (!theme || !settings)
        return -EINVAL;

    /* Currently only "default" is supported.  Unknown names fall back. */
    cbx_theme_default(theme);

    /* Apply overlay opacity from settings. */
    cbx_theme_apply_opacity(theme, settings->overlay_opacity);

    return 0;
}