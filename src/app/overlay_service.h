/*
 * overlay_service.h — overlay service entry point (Tasks 3–4).
 *
 * Declares run_overlay_service(), the production startup path for the
 * overlay service mode of controller-box (SPEC §2.3, §2.4).
 *
 * The function is kept in a separate translation unit (not main.c) so
 * that cmocka tests can link against it without pulling in main().
 */
#ifndef CBX_OVERLAY_SERVICE_H
#define CBX_OVERLAY_SERVICE_H

#include <stdbool.h>

/*
 * Run the overlay service.
 *
 * When dry_run is non-zero, prints a mode banner and returns 0 without
 * performing any initialization — headless-safe for acceptance checks.
 *
 * When dry_run is zero:
 *   1. Initialises SDL video and creates a hidden renderer.
 *   2. Connects to the system DBus; if InputPlumber is unavailable,
 *      logs an error to stderr and returns non-zero (SPEC §2.4).
 *   3. Enumerates composite devices via GetManagedObjects.
 *   4. Builds and pre-renders the overlay grid surface.
 *   5. Registers the overlay trigger on all composite devices.
 *   6. Initialises the overlay lifecycle state machine.
 *   7. Enters the poll loop: polls InterceptMode at 50 ms (DEC-002),
 *      processes SDL events for grid navigation, handles SIGTERM/SIGINT
 *      for clean shutdown.
 *
 * Returns 0 on clean shutdown, non-zero on init failure.
 */
int run_overlay_service(int dry_run);

/* --- Signal handling (exposed for testing — Task 4) ------------------- */

/*
 * Install SIGTERM/SIGINT handlers that request clean shutdown.
 * Returns 0 on success, -1 on sigaction failure.
 */
int cbx_overlay_service_install_signal_handlers(void);

/*
 * Returns true if a shutdown signal (SIGTERM/SIGINT) has been received.
 */
bool cbx_overlay_service_shutdown_requested(void);

/*
 * Reset the shutdown flag to false (for testing).
 */
void cbx_overlay_service_reset_shutdown(void);

#endif /* CBX_OVERLAY_SERVICE_H */