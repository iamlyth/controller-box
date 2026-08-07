/* src/app/main.c — dual-mode entry point (Task 2).
 *
 * Controller-Box is a single binary with two modes (SPEC §2.3):
 *
 *   controller-box [--overlay-service]   overlay service (default)
 *   controller-box --manager             manager configuration app
 *
 * Flags:
 *   --version        print the build version and exit
 *   --dry-run        print the selected mode and exit without running
 *                    (kept headless-safe for acceptance checks)
 *   -h, --help       print usage and exit
 *
 * The manager run-path is implemented in Task 34. The overlay service
 * run-path is filled in by later tasks. --dry-run forces the stub path
 * so the acceptance checks remain runnable in a headless sandbox.
 */
#include "config.h"
#include "controllerbox.h"
#include "config/config_paths.h"
#include "manager/manager.h"

#include <stdio.h>
#include <string.h>

typedef enum { CB_MODE_OVERLAY = 0, CB_MODE_MANAGER = 1 } cb_mode_t;

static const char *mode_name(cb_mode_t mode)
{
    return mode == CB_MODE_MANAGER ? "manager" : "overlay-service";
}

static void print_version(void)
{
    printf("controller-box %s\n", CONTROLLER_BOX_VERSION);
}

static void print_usage(const char *argv0)
{
    /* SPEC §2.3 invocation table. */
    printf(
        "Usage: %s [MODE] [OPTIONS]\n"
        "\n"
        "Controller-Box: a controller-only SDL2 GUI wrapping InputPlumber.\n"
        "\n"
        "Modes (SPEC §2.3):\n"
        "  --overlay-service  Overlay service (default). Runs as a systemd\n"
        "                      user service, always resident.\n"
        "  --manager          Manager configuration app. Launched on demand.\n"
        "\n"
        "Options:\n"
        "  --dry-run          Print the selected mode and exit without running.\n"
        "  --version          Print the build version and exit.\n"
        "  -h, --help         Show this help and exit.\n",
        argv0);
}

/* Run-paths. Real implementations arrive in later tasks; for now each prints a
 * mode banner and returns success. --dry-run annotates the banner so the
 * acceptance checks can distinguish a forced stub from a not-yet-built run. */
static int run_overlay_service(int dry_run)
{
    printf("controller-box: %s mode%s\n", mode_name(CB_MODE_OVERLAY),
           dry_run ? " (dry-run)" : "");
    /* TODO overlay service loop (pre-built surface, intercept poll). */
    return 0;
}

static int run_manager(int dry_run)
{
    printf("controller-box: %s mode%s\n", mode_name(CB_MODE_MANAGER),
           dry_run ? " (dry-run)" : "");
    if (dry_run)
        return 0;

    /* Discover a system font at runtime so the manager can render text.
     * Without a font the manager would show a blank, unusable window. */
    const char *font_path = cbx_font_path();
    if (!font_path) {
        fprintf(stderr,
                "controller-box: no system font found; install DejaVuSans "
                "or a TTF font in standard font directories\n");
        return 1;
    }

    /* Manager skeleton with tab bar (SPEC §5.1). */
    cbx_manager mgr;
    int rc = cbx_manager_init(&mgr, font_path);
    if (rc != 0) {
        fprintf(stderr, "controller-box: manager init failed: %d\n", rc);
        return 1;
    }
    rc = cbx_manager_run(&mgr);
    cbx_manager_shutdown(&mgr);
    return rc;
}

int main(int argc, char **argv)
{
    cb_mode_t mode = CB_MODE_OVERLAY; /* default: overlay service (SPEC §2.3) */
    int dry_run = 0;
    int show_version = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--overlay-service") == 0) {
            mode = CB_MODE_OVERLAY;
        } else if (strcmp(argv[i], "--manager") == 0) {
            mode = CB_MODE_MANAGER;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else if (strcmp(argv[i], "--version") == 0) {
            show_version = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "controller-box: unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 2;
        }
    }

    if (show_version) {
        print_version();
        return 0;
    }

    if (mode == CB_MODE_MANAGER) {
        return run_manager(dry_run);
    }
    return run_overlay_service(dry_run);
}