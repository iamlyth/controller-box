/*
 * interaction_inventory.h — Machine-readable interaction acceptance inventory
 * (Task 1, SPEC §5.7).
 *
 * Enumerates every interactive manager control (M01–M39), overlay action
 * (O01–O13), and disabled/degraded/operation-failure scenario (D01–D08)
 * with both input paths, expected semantic outcome, and verification status.
 *
 * Tests can iterate the array to drive automated traversal through normal
 * SDL events and production dispatch.
 */
#ifndef CBX_INTERACTION_INVENTORY_H
#define CBX_INTERACTION_INVENTORY_H

#include <stddef.h>

/* Control category: manager, overlay, or disabled/degraded scenario. */
typedef enum {
    CBX_CAT_MANAGER_TABBAR = 0,   /* M01–M03 */
    CBX_CAT_MANAGER_CTRL,         /* M04–M09  */
    CBX_CAT_MANAGER_PROF,        /* M10–M20  */
    CBX_CAT_MANAGER_SETTINGS,    /* M21–M27, M39 */
    CBX_CAT_MANAGER_EDITOR,      /* M28–M38  */
    CBX_CAT_OVERLAY,              /* O01–O13  */
    CBX_CAT_DISABLED              /* D01–D08  */
} cbx_inv_category;

/* Widget type within the UI hierarchy. */
typedef enum {
    CBX_WIDGET_TAB = 0,
    CBX_WIDGET_LIST,
    CBX_WIDGET_BUTTON,
    CBX_WIDGET_PICKER,
    CBX_WIDGET_NAME_INPUT,
    CBX_WIDGET_CONFIRM_DELETE,
    CBX_WIDGET_EDIT_MODE,
    CBX_WIDGET_BINDING,
    CBX_WIDGET_CAPTURE,
    CBX_WIDGET_SEQUENTIAL,
    CBX_WIDGET_EDITOR,
    CBX_WIDGET_OVERLAY_ACTION,
    CBX_WIDGET_SCENARIO
} cbx_inv_widget_type;

/* Current verification status of the control's acceptance test. */
typedef enum {
    CBX_VERIFY_UNVERIFIED = 0,    /* No test yet (pending task) */
    CBX_VERIFY_VERIFIED,          /* Test passes with semantic evidence */
    CBX_VERIFY_NOT_APPLICABLE,   /* Pointer path n/a for controller-only */
    CBX_VERIFY_DEFERRED          /* Documented as deferred per §13 */
} cbx_inv_verify_status;

/* Whether the pointer path is applicable to this control. */
typedef enum {
    CBX_PATH_NA = 0,              /* Controller-only (e.g. name input, seq) */
    CBX_PATH_AVAILABLE            /* Both controller and pointer paths */
} cbx_inv_path_availability;

/* A single inventory entry. */
typedef struct {
    const char             *id;              /* "M01", "O01", "D01", etc. */
    cbx_inv_category        category;        /* Manager / overlay / disabled */
    const char             *context;         /* "Controllers tab", etc.  */
    cbx_inv_widget_type     widget_type;     /* Widget kind              */
    const char             *controller_path;  /* Controller-path desc     */
    cbx_inv_path_availability pointer_path_avail; /* Pointer path applicable? */
    const char             *pointer_path;     /* Pointer-path desc or "n/a" */
    const char             *semantic_outcome; /* Expected semantic outcome */
    const char             *dispatch_path;    /* Production dispatch path  */
    cbx_inv_verify_status   verify_status;    /* Current test status       */
    const char             *evidence_task;    /* Task providing evidence   */
} cbx_interaction_entry;

/*
 * Returns a pointer to the static inventory array.
 * The array is terminated by an entry whose `id` field is NULL.
 */
const cbx_interaction_entry *cbx_interaction_inventory_get(void);

/*
 * Returns the number of entries in the inventory (excluding the NULL
 * terminator entry).
 */
size_t cbx_interaction_inventory_count(void);

/*
 * Returns the entry for the given control ID (e.g. "M05"), or NULL if
 * not found.
 */
const cbx_interaction_entry *cbx_interaction_inventory_find(const char *id);

/*
 * Runtime verification ledger (SPEC §5.7).
 *
 * The static table's `verify_status` is a declaration of intent — it is
 * NOT runtime truth.  A control is only considered verified when a
 * passing production-dispatch test marks it via
 * cbx_interaction_inventory_mark_verified() (called after the test's
 * assertions all pass).  This ties the ledger's verified flags to actual
 * test pass status rather than to a hardcoded claim.
 *
 * The runtime ledger is a mutable side table separate from the read-only
 * descriptive array, so tests that run the controls populate it and a
 * ledger test can assert that no control is "verified" unless it was
 * marked by a passing test in the same process.
 */

/*
 * Mark a control as verified at runtime.  Returns 0 on success (entry
 * found and marked) or -EINVAL if the ID is unknown.
 *
 * Dispatch tests call this only after their assertions all pass, so the
 * ledger reflects real pass status, not declared intent.
 */
int cbx_interaction_inventory_mark_verified(const char *id);

/*
 * Returns 1 if the control was marked verified at runtime, 0 otherwise
 * (or -1 if the ID is unknown).
 */
int cbx_interaction_inventory_is_verified(const char *id);

/*
 * Reset all runtime verification marks (used to prove the ledger is not
 * pre-seeded with static claims, e.g. at the start of a ledger test).
 */
void cbx_interaction_inventory_reset(void);

#endif /* CBX_INTERACTION_INVENTORY_H */