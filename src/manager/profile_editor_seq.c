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
 */

/*
 * Find or create a mapping for a given button in the profile.
 * Returns the mapping index, or -1 if the profile is full.
 */
static int
find_or_create_mapping(cbx_profile *p, cbx_diag_button btn)
{
    if (!p || btn == CBX_DIAG_BTN_NONE)
        return -1;

    const char *btn_name = cbx_profile_diagram_button_name(btn);
    if (!btn_name)
        return -1;

    /* Search for an existing mapping with this button */
    for (int i = 0; i < p->mapping_count; i++) {
        for (int j = 0; j < p->mappings[i].source_event.prop_count; j++) {
            if ((strcmp(p->mappings[i].source_event.props[j].key,
                         "button") == 0
                 || strcmp(p->mappings[i].source_event.props[j].key,
                            "axis") == 0)
                && strcmp(p->mappings[i].source_event.props[j].value,
                           btn_name) == 0) {
                return i;
            }
        }
    }

    /* Create a new mapping */
    if (p->mapping_count >= CBX_MAX_MAPPINGS)
        return -1;

    int idx = p->mapping_count;
    cbx_profile_mapping *m = &p->mappings[idx];
    memset(m, 0, sizeof(*m));

    /* Set the mapping name to the button name */
    strncpy(m->name, btn_name, sizeof(m->name) - 1);

    /* Set source event: gamepad button */
    strncpy(m->source_event.device_class, "gamepad",
             sizeof(m->source_event.device_class) - 1);
    m->source_event.prop_count = 1;
    strncpy(m->source_event.props[0].key, "button",
             sizeof(m->source_event.props[0].key) - 1);
    strncpy(m->source_event.props[0].value, btn_name,
             sizeof(m->source_event.props[0].value) - 1);

    p->mapping_count++;
    return idx;
}

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

    /* Initialize input event handler for capture */
    if (ed->backend && ed->bus) {
        ip_input_events_init(&ed->input_events, ed->backend, ed->bus,
                              IP_DBUS_NAME,
                              cbx_profile_editor_on_input_event, ed);
        ip_input_events_subscribe(&ed->input_events);
    }

    update_seq_ui(ed);

    return 0;
}

void
cbx_profile_editor_cancel_sequential(cbx_profile_editor *ed)
{
    if (!ed)
        return;

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

    /* If the user pressed Start, cancel sequential mode */
    if (strcmp(raw_event, "Start") == 0) {
        cbx_profile_editor_cancel_sequential(ed);
        return;
    }

    /* If the user pressed B, skip the current button */
    if (strcmp(raw_event, "B") == 0) {
        cbx_profile_editor_seq_skip(ed);
        return;
    }

    /* Get the current button being prompted */
    cbx_diag_button current_btn = (cbx_diag_button)ed->seq_step;
    const char *current_name = cbx_profile_diagram_button_name(current_btn);
    if (!current_name)
        return;

    /* Create or find the mapping for the current button */
    int map_idx = find_or_create_mapping(&ed->profile, current_btn);
    if (map_idx < 0)
        return;

    /*
     * Set the source event's button to the captured raw event.
     * This allows remapping (e.g., pressing X when prompted for A
     * maps X to the A slot).  The mapping's name stays as the
     * canonical button name; the source event gets the physical button.
     */
    cbx_profile_mapping *m = &ed->profile.mappings[map_idx];

    /* Find or create the "button" prop */
    int prop_idx = -1;
    for (int i = 0; i < m->source_event.prop_count; i++) {
        if (strcmp(m->source_event.props[i].key, "button") == 0
            || strcmp(m->source_event.props[i].key, "axis") == 0) {
            prop_idx = i;
            break;
        }
    }
    if (prop_idx < 0) {
        if (m->source_event.prop_count < CBX_MAX_EVENT_PROPS) {
            prop_idx = m->source_event.prop_count++;
            strncpy(m->source_event.props[prop_idx].key, "button",
                     sizeof(m->source_event.props[prop_idx].key) - 1);
            m->source_event.props[prop_idx].key
                [sizeof(m->source_event.props[prop_idx].key) - 1] = '\0';
        } else {
            return;
        }
    }

    strncpy(m->source_event.props[prop_idx].value, raw_event,
             sizeof(m->source_event.props[prop_idx].value) - 1);
    m->source_event.props[prop_idx].value
        [sizeof(m->source_event.props[prop_idx].value) - 1] = '\0';

    /* Auto-advance to next step */
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