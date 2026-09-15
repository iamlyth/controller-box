/*
 * profile_editor_seq.c — Sequential binding mode for the profile editor
 * (Task 38).
 *
 * Implements Mode 2 — Sequential binding.  The editor prompts for each
 * button in order (all 17 cbx_diag_button entries); the diagram lights
 * up the current button.  Press a physical button -> captured ->
 * auto-advance.  B skips the current button.  Start cancels.
 *
 * The implementation operates on cbx_profile_editor from profile_editor_list.h.
 * The progress bar widget (cbx_progress) is initialised in the editor's
 * init and shows completion as buttons are bound.
 *
 * Task 38 — Profile editor — sequential binding mode and validation.
 */
#include "manager/profile_editor_seq.h"

#include <SDL2/SDL.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "dbus/ip_composite.h"

/* ------------------------------------------------------------------ */
/*  Layout constants (match list mode)                                 */
/* ------------------------------------------------------------------ */

#define CBX_PE_SEQ_PROGRESS_W  580
#define CBX_PE_SEQ_PROGRESS_H  24
#define CBX_PE_SEQ_PROGRESS_Y  (60 + 420 + 8)  /* below binding list */

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

/*
 * The sequential button order follows the cbx_diag_button enum:
 * UP=0, DOWN=1, LEFT=2, RIGHT=3, A=4, B=5, X=6, Y=7,
 * START=8, SELECT=9, GUIDE=10, L1=11, R1=12, L2=13, R2=14, L3=15, R3=16.
 * Total: CBX_DIAG_BTN_COUNT (17).
 *
 * Mapping lookup/creation is shared with binding-list mode via
 * cbx_profile_editor_find_or_create_mapping() (BUG-0016), so sequential
 * capture and unbound-row activation never duplicate a mapping.
 */

/*
 * Update the UI for the current sequential step:
 * - Highlight the current button on the diagram
 * - Update the status label with a prompt
 * - Update the progress bar
 */
static void
update_seq_ui(cbx_profile_editor *ed)
{
    if (!ed)
        return;

    cbx_diag_button current = (cbx_diag_button)ed->seq_step;

    /* Highlight current button on diagram */
    cbx_profile_diagram_highlight(&ed->diagram, current);

    /* Update status label */
    const char *btn_name = cbx_profile_diagram_button_name(current);
    char prompt[CBX_PE_LABEL_LEN];
    if (btn_name)
        snprintf(prompt, sizeof(prompt),
                   "Press button for: %s  (B=Skip, Start=Cancel)", btn_name);
    else
        snprintf(prompt, sizeof(prompt), "Sequential binding: step %d",
                   ed->seq_step);
    cbx_label_set_text(&ed->status_lbl, prompt);

    /* Update progress bar */
    double frac = (double)ed->seq_step / (double)CBX_DIAG_BTN_COUNT;
    cbx_progress_set_fraction(&ed->progress_bar, frac);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int
cbx_profile_editor_begin_sequential(cbx_profile_editor *ed)
{
    if (!ed)
        return -EINVAL;

    /* Must have a profile loaded */
    if (!ed->profile_loaded)
        return -EINVAL;

    /* Acquire interception before entering the mode so a subscription or
     * intercept-mode failure aborts cleanly with the prior mode restored. */
    int rc = cbx_profile_editor_acquire_interception(ed);
    if (rc != 0) {
        ed->seq_active = false;
        ed->mode = CBX_EDITOR_MODE_LIST;
        cbx_label_set_text(&ed->status_lbl,
                             "Sequential unavailable: input intercept failed");
        return rc;
    }

    ed->seq_step = 0;
    ed->seq_active = true;
    ed->mode = CBX_EDITOR_MODE_SEQUENTIAL;
    ed->selected_index = -1;
    ed->editing_index = -1;

    /* Show progress bar */
    cbx_widget_set_visible(&ed->progress_bar.base, true);
    cbx_progress_set_fraction(&ed->progress_bar, 0.0);

    /* Hide binding list, keep diagram visible */
    cbx_widget_set_visible(&ed->binding_list.base, false);
    cbx_widget_set_visible(&ed->target_list.base, false);

    update_seq_ui(ed);

    return 0;
}

void
cbx_profile_editor_cancel_sequential(cbx_profile_editor *ed)
{
    if (!ed)
        return;

    /* Restore the interception this sequential run owned. */
    cbx_profile_editor_release_interception(ed);

    ed->seq_active = false;
    ed->seq_step = 0;
    ed->mode = CBX_EDITOR_MODE_LIST;

    /* Hide progress bar */
    cbx_widget_set_visible(&ed->progress_bar.base, false);

    /* Show binding list again */
    cbx_widget_set_visible(&ed->binding_list.base, true);

    cbx_label_set_text(&ed->status_lbl, "");

    /* Refresh binding list to show any captured bindings */
    cbx_profile_editor_refresh(ed);
}

int
cbx_profile_editor_seq_skip(cbx_profile_editor *ed)
{
    if (!ed || !ed->seq_active)
        return -ENOENT;

    /* Advance to next step */
    ed->seq_step++;
    if (ed->seq_step >= CBX_DIAG_BTN_COUNT) {
        /* Finished all buttons */
        cbx_profile_editor_cancel_sequential(ed);
        return 0;
    }

    update_seq_ui(ed);
    return 0;
}

void
cbx_profile_editor_seq_on_input(ip_input_id input,
                                   ip_input_category category,
                                   double value,
                                   const char *raw_event,
                                   const char *device_path,
                                   void *userdata)
{
    (void)input;
    (void)category;
    (void)device_path;

    cbx_profile_editor *ed = (cbx_profile_editor *)userdata;
    if (!ed || !ed->seq_active)
        return;

    /* Only capture button presses (value == 1.0), not releases */
    if (value != 1.0)
        return;

    if (!raw_event)
        return;

    /* Get the current button being prompted. */
    cbx_diag_button current_btn = (cbx_diag_button)ed->seq_step;
    const char *current_name = cbx_profile_diagram_button_name(current_btn);
    if (!current_name)
        return;

    /* B skips and Start cancels — but only while they are not the button
     * the user is being asked to bind.  Otherwise the required NES "B"
     * binding (and Start) could never be captured.  The SDL navigation
     * stream is filtered separately by the profiles tab, so a captured
     * press is never confused with navigation. */
    if (strcmp(raw_event, "Start") == 0 &&
        current_btn != CBX_DIAG_BTN_START) {
        cbx_profile_editor_cancel_sequential(ed);
        return;
    }
    if (strcmp(raw_event, "B") == 0 && current_btn != CBX_DIAG_BTN_B) {
        cbx_profile_editor_seq_skip(ed);
        return;
    }

    /* Find or create the mapping for the prompted virtual target.  The
     * prompt names the virtual button the game must see; the pressed
     * button is the physical source.  Source-keyed lookup preserves an
     * existing mapping's chosen output target. */
    int map_idx = cbx_profile_editor_find_or_create_mapping(&ed->profile,
                                                             current_btn);
    if (map_idx < 0)
        return;

    cbx_profile_mapping *m = &ed->profile.mappings[map_idx];

    /* Record the pressed physical source. */
    int prop_idx = cbx_profile_editor_source_button_prop(m);
    if (prop_idx < 0)
        return;

    strncpy(m->source_event.props[prop_idx].value, raw_event,
             sizeof(m->source_event.props[prop_idx].value) - 1);
    m->source_event.props[prop_idx].value
        [sizeof(m->source_event.props[prop_idx].value) - 1] = '\0';

    /* Record the prompted virtual target (identity gamepad event) for a
     * newly created mapping so a clean empty profile produces the button
     * the user was asked for.  An existing mapping keeps the output the
     * user already chose; sequential capture only re-records its source. */
    if (m->target_event_count == 0) {
        m->target_event_count = 1;
        strncpy(m->target_events[0].device_class, "gamepad",
                sizeof(m->target_events[0].device_class) - 1);
        m->target_events[0].device_class
            [sizeof(m->target_events[0].device_class) - 1] = '\0';
        strncpy(m->target_events[0].value, current_name,
                sizeof(m->target_events[0].value) - 1);
        m->target_events[0].value
            [sizeof(m->target_events[0].value) - 1] = '\0';
    }

    /* Auto-advance to next step */
    ed->dirty = true;           /* sequential capture modified the profile */
    ed->seq_step++;
    if (ed->seq_step >= CBX_DIAG_BTN_COUNT) {
        /* Finished all buttons */
        cbx_progress_set_fraction(&ed->progress_bar, 1.0);
        cbx_label_set_text(&ed->status_lbl,
                             "Sequential binding complete!");
        /* Return to list mode after a brief completion state */
        cbx_profile_editor_cancel_sequential(ed);
        return;
    }

    update_seq_ui(ed);
}

double
cbx_profile_editor_seq_progress(const cbx_profile_editor *ed)
{
    if (!ed || !ed->seq_active)
        return 0.0;
    return (double)ed->seq_step / (double)CBX_DIAG_BTN_COUNT;
}

cbx_diag_button
cbx_profile_editor_seq_current_button(const cbx_profile_editor *ed)
{
    if (!ed || !ed->seq_active || ed->seq_step >= CBX_DIAG_BTN_COUNT)
        return CBX_DIAG_BTN_NONE;
    return (cbx_diag_button)ed->seq_step;
}

int
cbx_profile_editor_seq_get_step(const cbx_profile_editor *ed)
{
    if (!ed)
        return -1;
    return ed->seq_step;
}

bool
cbx_profile_editor_seq_is_active(const cbx_profile_editor *ed)
{
    if (!ed)
        return false;
    return ed->seq_active;
}