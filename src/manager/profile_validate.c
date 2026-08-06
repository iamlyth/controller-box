/*
 * profile_validate.c — Profile validation: NES minimum binding set
 * (Task 38).
 *
 * Implements the validation logic that checks whether a profile has
 * all required NES minimum bindings: A, B, D-Pad Up, Down, Left, Right.
 */
#include "manager/profile_validate.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/*  NES minimum required buttons table                                 */
/* ------------------------------------------------------------------ */

static const cbx_diag_button nes_minimum[] = {
    CBX_DIAG_BTN_A,
    CBX_DIAG_BTN_B,
    CBX_DIAG_BTN_UP,
    CBX_DIAG_BTN_DOWN,
    CBX_DIAG_BTN_LEFT,
    CBX_DIAG_BTN_RIGHT,
};

const cbx_diag_button *
cbx_nes_minimum_buttons(void)
{
    return nes_minimum;
}

const char *
cbx_nes_minimum_button_name(int index)
{
    if (index < 0 || index >= CBX_NES_MINIMUM_COUNT)
        return NULL;
    return cbx_profile_diagram_button_name(nes_minimum[index]);
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

/*
 * Find the "button" or "axis" property in a source event's props.
 * Returns the value string, or NULL if not found.
 */
static const char *
source_event_button_name(const cbx_source_event *se)
{
    if (!se)
        return NULL;
    for (int i = 0; i < se->prop_count; i++) {
        if (strcmp(se->props[i].key, "button") == 0)
            return se->props[i].value;
    }
    for (int i = 0; i < se->prop_count; i++) {
        if (strcmp(se->props[i].key, "axis") == 0)
            return se->props[i].value;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

bool
cbx_profile_has_binding(const cbx_profile *p, cbx_diag_button btn)
{
    if (!p || btn == CBX_DIAG_BTN_NONE)
        return false;

    const char *target_name = cbx_profile_diagram_button_name(btn);
    if (!target_name)
        return false;

    for (int i = 0; i < p->mapping_count; i++) {
        const char *btn_name = source_event_button_name(
            &p->mappings[i].source_event);
        if (btn_name && strcmp(btn_name, target_name) == 0)
            return true;
    }

    return false;
}

int
cbx_profile_validate_nes_minimum(const cbx_profile *p,
                                    char *missing_buf, size_t buflen)
{
    if (!p)
        return -EINVAL;

    int missing_count = 0;

    if (missing_buf && buflen > 0)
        missing_buf[0] = '\0';

    for (int i = 0; i < CBX_NES_MINIMUM_COUNT; i++) {
        cbx_diag_button btn = nes_minimum[i];
        if (!cbx_profile_has_binding(p, btn)) {
            missing_count++;
            if (missing_buf && buflen > 0) {
                const char *name = cbx_profile_diagram_button_name(btn);
                if (name) {
                    if (missing_buf[0] != '\0')
                        strncat(missing_buf, ", ",
                                 buflen - strlen(missing_buf) - 1);
                    strncat(missing_buf, name,
                             buflen - strlen(missing_buf) - 1);
                }
            }
        }
    }

    if (missing_count > 0)
        return -EINVAL;

    return 0;
}

int
cbx_profile_validate_missing_count(const cbx_profile *p)
{
    if (!p)
        return -EINVAL;

    int count = 0;
    for (int i = 0; i < CBX_NES_MINIMUM_COUNT; i++) {
        if (!cbx_profile_has_binding(p, nes_minimum[i]))
            count++;
    }
    return count;
}