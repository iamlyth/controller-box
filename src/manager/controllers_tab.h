/*
 * controllers_tab.h — Controllers tab for the Manager UI (SPEC §5.2).
 *
 * Lists current target devices (virtual controllers) with their type,
 * provides Add / Remove / Change-type actions backed by InputPlumber's
 * DBus API.
 *
 * DBus calls:
 *   Add         → ip_manager_create_target_device (Manager iface)
 *   Remove      → ip_manager_stop_target_device   (Manager iface)
 *   Change type → ip_composite_set_target_devices  (CompositeDevice iface)
 *
 * All DBus access goes through the ip_dbus_backend vtable so the module
 * is fully unit-testable with the mock backend.
 *
 * Task 35 — Controllers tab.
 */
#ifndef CBX_CONTROLLERS_TAB_H
#define CBX_CONTROLLERS_TAB_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#include "dbus_mock.h"           /* ip_dbus_backend, ip_bus_handle */
#include "dbus/ip_device_model.h" /* cbx_device_model */
#include "ui/widget.h"           /* cbx_panel, cbx_list, cbx_button */
#include "ui/text.h"             /* cbx_text_cache */
#include "ui/theme.h"            /* cbx_theme */
#include "config/config_settings.h" /* CBX_MAX_TYPE_LEN */

/* ------------------------------------------------------------------ */
/*  Limits                                                            */
/* ------------------------------------------------------------------ */

#define CBX_CT_MAX_TYPES    32   /* max supported target device IDs  */
#define CBX_CT_MAX_DEVICES  64   /* max target devices to track       */
#define CBX_CT_LABEL_LEN   128

/* ------------------------------------------------------------------ */
/*  Controllers tab state                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    CBX_CT_MODE_LIST = 0,      /* normal device-list view */
    CBX_CT_MODE_TYPE_PICK,     /* type picker overlay     */
} cbx_ct_mode;

typedef enum {
    CBX_CT_ACTION_NONE = 0,
    CBX_CT_ACTION_ADD,        /* type pick → CreateTargetDevice  */
    CBX_CT_ACTION_CHANGE,     /* type pick → SetTargetDevices     */
} cbx_ct_action;

typedef struct {
    /* --- DBus (borrowed) ------------------------------------------- */
    const ip_dbus_backend *backend;
    ip_bus_handle          bus;

    /* --- Device model (owned) -------------------------------------- */
    cbx_device_model model;

    /* --- Supported types (parsed from SupportedTargetDeviceIds) ---- */
    char supported_types[CBX_CT_MAX_TYPES][CBX_MAX_TYPE_LEN];
    int  supported_type_count;

    /* --- Per-target device types (queried via ip_target_get_device_type) */
    char device_types[CBX_CT_MAX_DEVICES][CBX_MAX_TYPE_LEN];
    int  device_type_count;

    /* --- Widgets (owned) ------------------------------------------- */
    cbx_list   device_list;        /* list of target devices + types  */
    cbx_list   type_picker;         /* list of supported types         */
    cbx_button add_btn;             /* "Add Controller"               */
    cbx_button remove_btn;          /* "Remove"                       */
    cbx_button change_type_btn;      /* "Change Type"                  */

    /* --- Panel (borrowed) ------------------------------------------ */
    cbx_panel *panel;

    /* --- Rendering deps (borrowed) --------------------------------- */
    cbx_text_cache *text_cache;
    const cbx_theme *theme;
    int             font_id;

    /* --- UI state --------------------------------------------------- */
    cbx_ct_mode    mode;
    cbx_ct_action  pending_action;
    int            selected_device;  /* index in device_list, -1 = none */
    int            selected_type;    /* index in type_picker, -1 = none */
} cbx_controllers_tab;

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

/*
 * Initialise the controllers tab: populate the panel with a device list
 * and three buttons (Add, Remove, Change Type).  Connects to DBus via
 * the provided backend, loads supported types, and refreshes the device
 * list.
 *
 * @param tab      Output struct (overwritten).
 * @param panel    Panel to populate (borrowed, not freed by tab).
 * @param backend  DBus backend vtable (borrowed).
 * @param bus      DBus bus handle (borrowed).
 * @param cache    Text cache (borrowed).
 * @param theme    Theme (borrowed).
 * @param font_id  Font ID from text cache, or -1 for no font.
 * @return 0 on success, negative errno on error.
 */
int cbx_controllers_tab_init(cbx_controllers_tab *tab,
                               cbx_panel *panel,
                               const ip_dbus_backend *backend,
                               ip_bus_handle bus,
                               cbx_text_cache *cache,
                               const cbx_theme *theme,
                               int font_id);

/*
 * Refresh the device list: re-enumerate via GetManagedObjects, query
 * each target's DeviceType, and rebuild the list widget.
 * Returns 0 on success, negative errno on error (partial success is OK).
 */
int cbx_controllers_tab_refresh(cbx_controllers_tab *tab);

/*
 * Shut down and free all widget resources.  Safe on a zeroed struct.
 */
void cbx_controllers_tab_shutdown(cbx_controllers_tab *tab);

/* ------------------------------------------------------------------ */
/*  Actions (testable without UI events)                               */
/* ------------------------------------------------------------------ */

/*
 * Load supported target device IDs from the SupportedTargetDeviceIds
 * property.  Parses the comma-separated string into supported_types[].
 * Returns 0 on success, negative errno on error.
 */
int cbx_controllers_tab_load_supported_types(cbx_controllers_tab *tab);

/*
 * Add a new virtual controller of the given type.
 * Calls ip_manager_create_target_device, then refreshes the list.
 * Returns 0 on success, negative errno on error.
 */
int cbx_controllers_tab_add(cbx_controllers_tab *tab, const char *type);

/*
 * Remove the virtual controller at the given index.
 * Calls ip_manager_stop_target_device with the target's path, then
 * refreshes the list.  SPEC §5.2: the physical controller in that slot
 * auto-moves to Unassigned.
 * Returns 0 on success, negative errno on error.
 */
int cbx_controllers_tab_remove(cbx_controllers_tab *tab, int device_index);

/*
 * Change the type of the virtual controller at the given index.
 * Calls ip_composite_set_target_devices on the corresponding composite
 * device with the updated types CSV.  SPEC §5.2: mixed types allowed.
 * Returns 0 on success, negative errno on error.
 */
int cbx_controllers_tab_change_type(cbx_controllers_tab *tab,
                                      int device_index,
                                      const char *new_type);

/*
 * Enter type-picker mode for the given action (add or change).
 * Populates type_picker with supported_types and switches mode.
 * Returns 0 on success, negative errno on error.
 */
int cbx_controllers_tab_begin_type_pick(cbx_controllers_tab *tab,
                                          cbx_ct_action action);

/*
 * Confirm the type picker selection: executes the pending action with
 * the selected type, then returns to list mode.
 * Returns 0 on success, negative errno on error.
 */
int cbx_controllers_tab_confirm_type_pick(cbx_controllers_tab *tab);

/*
 * Cancel the type picker and return to list mode.
 */
void cbx_controllers_tab_cancel_type_pick(cbx_controllers_tab *tab);

/* ------------------------------------------------------------------ */
/*  Accessors (for testing)                                            */
/* ------------------------------------------------------------------ */

int  cbx_controllers_tab_device_count(const cbx_controllers_tab *tab);
int  cbx_controllers_tab_supported_type_count(const cbx_controllers_tab *tab);
const char *cbx_controllers_tab_device_type(const cbx_controllers_tab *tab,
                                              int index);
const char *cbx_controllers_tab_device_path(const cbx_controllers_tab *tab,
                                               int index);
const char *cbx_controllers_tab_supported_type(const cbx_controllers_tab *tab,
                                                   int index);
int  cbx_controllers_tab_selected_device(const cbx_controllers_tab *tab);
cbx_ct_mode cbx_controllers_tab_mode(const cbx_controllers_tab *tab);

#endif /* CBX_CONTROLLERS_TAB_H */