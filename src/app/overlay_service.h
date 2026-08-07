/*
 * overlay_service.h — overlay service entry point (Task 3).
 *
 * Declares run_overlay_service(), the production startup path for the
 * overlay service mode of controller-box (SPEC §2.3, §2.4).
 *
 * The function is kept in a separate translation unit (not main.c) so
 * that cmocka tests can link against it without pulling in main().
 */
#ifndef CBX_OVERLAY_SERVICE_H
#define CBX_OVERLAY_SERVICE_H

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
 *   7. Enters the poll loop (Task 4 fills in the full loop body).
 *
 * Returns 0 on clean shutdown, non-zero on init failure.
 */
int run_overlay_service(int dry_run);

#endif /* CBX_OVERLAY_SERVICE_H */