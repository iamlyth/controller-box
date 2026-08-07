---
spec_path: docs/SPEC.md
spec_commit: 2f2903d10094980d206e4dc4cd570dc54e209fbc
spec_blob: f56978746e13aef4b6bec8f801dcce9e64a1c7ad
base_commit: 15b9ea4d1528204ab0f2d03e9a24a687c04c42a2
status: active
---

# Implementation Plan — Framebuffer-Backed Visual Acceptance

## Goal

The committed specification (commit 2f2903d) added three new sections —
§4.10 (overlay visual acceptance), §5.6 (manager visual acceptance), and
§11.1 (seven-layer rendering verification and release evidence) — that
require deterministic framebuffer pixel readback, region-level
assertions, golden images, failure artifacts, installed production smoke
tests, and backend smoke coverage. The existing implementation completed
44 tasks covering all functional components (overlay state machine,
manager tabs, DBus integration, config, icons, identification), but
**zero tests call `SDL_RenderReadPixels`** and no golden images or
failure artifacts exist. Additionally, the overlay service run path in
`main.c` is still a stub, and the overlay grid renderer has no
conflict-specific red rendering. This plan closes those gaps.

## Non-goals

- Re-implementing existing functional components (overlay modules,
  manager tabs, DBus wrappers, config, icons, identification — all
  complete and tested at the structural/state-machine level).
- Changing the architecture, widget toolkit, or DBus integration
  approach.
- Adding new features beyond what the committed spec requires.
- Human release acceptance (§11.1.7) is a process gate documented in
  the final task, not automated.

## Architecture and constraints

- **Language:** C11 (DEC-001). All new test code is C.
- **Test framework:** cmocka + CTest + SDL2 dummy driver (DEC-003).
- **Renderer:** SDL2 software renderer for deterministic tests;
  accelerated/GLES for backend smoke. The existing `test_harness`
  already forces `SDL_VIDEODRIVER=dummy` + `SDL_RENDERER_SOFTWARE`.
- **Production composition path:** Overlay tests must call
  `cbx_overlay_surface_render(surface, renderer, cbx_select_grid_render_cb, &ctx)`
  — the same callback-based composition the overlay service uses.
  Manager tests must use `cbx_manager_init()` / `cbx_manager_render()` —
  no manual module attachment (§5.6).
- **Build deps:** SDL2, SDL2_ttf, SDL2_image, libsystemd, libyaml,
  cmocka — available via `nix-shell` (mem context). Verification uses
  `scripts/verify-project.sh`.
- **Build directory:** All verification commands assume `build-check` is
  configured first: `cmake -S . -B build-check -DCMAKE_BUILD_TYPE=Debug`.
  The full-suite gate `scripts/verify-project.sh` uses its own
  `build-maintenance-verify` directory and handles configuration
  internally.
- **Texture access:** Target textures use `SDL_TEXTUREACCESS_TARGET`.
  Pixel readback uses `SDL_RenderReadPixels` on the renderer's current
  target, not `SDL_LockTexture` on non-streaming textures (§11.1.1).
- **Poll interval note:** The spec text says ~500 ms in §2.5, §10.3, and
  §11, but the implementation uses 50 ms per DEC-002 (confidence 70).
  The plan follows the implementation (50 ms). Task 11 includes
  correcting the spec text to match.

## Task list

## Task 1: Framebuffer test infrastructure
- Status: pending
- Dependencies: none
- Scope: `tests/fb_assert.c`, `tests/fb_assert.h`, `tests/test_fb_assert.c`, `tests/CMakeLists.txt`
- Acceptance criteria:
  - `fb_assert.h` provides:
    - `fb_read_pixels(renderer, rect, buf, buf_len)` — wraps `SDL_RenderReadPixels` into an RGBA buffer.
    - `fb_region_has_content(buf, w, h, rect, bg_color, tolerance)` — returns true if the region contains any pixel whose RGB channels differ from `bg_color` by more than `tolerance`.
    - `fb_region_has_color(buf, w, h, rect, target_rgb, tolerance)` — returns true if the region contains pixels matching `target_rgb` within `tolerance` per channel.
    - `fb_frames_differ(buf_a, buf_b, w, h, threshold_pct)` — returns true if more than `threshold_pct` of pixels differ between the two captures.
    - `fb_golden_compare(capture, w, h, golden_path, per_pixel_tol, image_tol_pct)` — loads a golden PNG via SDL2_image, compares per-pixel within `per_pixel_tol` per channel, returns true if the percentage of differing pixels is below `image_tol_pct`.
    - `fb_save_png(buf, w, h, path)` — writes a PNG via `IMG_SavePNG`.
    - `fb_save_diff(actual, expected, w, h, path)` — writes a visual diff PNG highlighting differing pixels.
  - `fb_assert.c` implements all functions with deterministic, dependency-free integer logic (no floating-point comparisons; integer tolerance bands).
  - `test_fb_assert.c` self-test: renders a known colored rect via SDL software renderer, reads back pixels, verifies `fb_region_has_content` passes for the rect region and fails for a background-only region, verifies `fb_region_has_color` finds the correct color, generates a golden PNG and verifies `fb_golden_compare` passes against it, and verifies `fb_frames_differ` distinguishes two different renders.
  - CMake registers `test_fb_assert` as a ctest.
- Verification: `nix-shell --run "cmake -S . -B build-check -DCMAKE_BUILD_TYPE=Debug && cmake --build build-check --target test_fb_assert && ctest --test-dir build-check -R test_fb_assert --output-on-failure"`
- Documentation impact: none (test infrastructure)

## Task 2: Conflict red rendering in overlay grid
- Status: pending
- Dependencies: 1
- Scope: `src/overlay/grid_render.c`, `src/overlay/grid_render.h`, `tests/test_conflict.c`
- Acceptance criteria:
  - `cbx_grid_render_ctx` gains a `const cbx_conflict_list *conflicts` field (optional, NULL-safe).
  - `cbx_select_grid_render()` draws conflicted rows with a red indicator: the cell border or background for the conflicted row's current column uses a red color (e.g., `{220, 40, 40, 255}`) instead of the default highlight/dim.
  - Non-conflicted rows render unchanged.
  - Existing structural conflict tests (`test_conflict.c`) still pass.
  - A new test in `test_conflict.c` (or `test_grid_render.c`) renders a grid with a conflict list via `cbx_overlay_surface_render(surface, renderer, cbx_select_grid_render_cb, &ctx)`, reads back pixels with `fb_read_pixels`, and asserts via `fb_region_has_color` that the conflicted cell region contains red pixels.
- Verification: `nix-shell --run "cmake --build build-check --target test_conflict test_grid_render && ctest --test-dir build-check -R 'test_conflict|test_grid_render' --output-on-failure"`
- Documentation impact: none (internal rendering detail)

## Task 3: Overlay service initialization
- Status: pending
- Dependencies: none
- Scope: `src/app/main.c`
- Acceptance criteria:
  - `run_overlay_service()` in `main.c` is no longer a stub. It:
    1. Initializes SDL video, creates a hidden `cbx_renderer`.
    2. Connects to the system DBus via `ip_connection`; if InputPlumber is unavailable, logs a clear error to stderr ("InputPlumber not found" or DBus error) and returns non-zero (§2.4).
    3. Enumerates composite devices via `cbx_objectmanager_enumerate`.
    4. Initializes the overlay surface: `cbx_overlay_surface_init(surface, renderer, w, h, opacity)` → `cbx_overlay_surface_mark_dirty_all(surface)` → `cbx_overlay_surface_render(surface, renderer, cbx_select_grid_render_cb, &ctx)`.
    5. Registers the overlay trigger via `cbx_trigger_register_all` and sets `InterceptMode = PASS` on all composites.
    6. Initializes `cbx_overlay_lifecycle` with the surface, renderer, and callbacks.
    7. Enters the poll loop (delegating to Task 4 for the loop body).
  - `controller-box --overlay-service --dry-run` still works (prints banner, exits 0).
  - `controller-box --overlay-service` without `--dry-run` attempts real initialization; if SDL cannot initialize (set `SDL_VIDEODRIVER=nonexistent`), returns non-zero with stderr message, not a crash.
  - If DBus connection fails, returns non-zero with "InputPlumber not found" or equivalent error.
  - Existing `--manager` path is unaffected.
  - A cmocka test verifies: (a) `run_overlay_service(0)` with `SDL_VIDEODRIVER=nonexistent` returns non-zero; (b) `run_overlay_service(1)` (dry-run) returns 0.
- Verification: `nix-shell --run "cmake --build build-check --target controller-box && ./build-check/controller-box --overlay-service --dry-run && SDL_VIDEODRIVER=nonexistent ./build-check/controller-box --overlay-service 2>/dev/null; test \$? -ne 0"`
- Documentation impact: `docs/OPERATIONS.md` — document overlay service startup behavior and prerequisites.

## Task 4: Overlay service poll loop and shutdown
- Status: pending
- Dependencies: 3
- Scope: `src/app/main.c`, `src/overlay/lifecycle.c` (if wiring helpers needed)
- Acceptance criteria:
  - The poll loop (entered by Task 3's `run_overlay_service`):
    1. Polls `InterceptMode` at 50 ms intervals (DEC-002) via `ip_intercept_poll`.
    2. On `InterceptMode = ALL` (activation detected): calls `cbx_overlay_lifecycle_activate()`, which shows the pre-built surface.
    3. Processes SDL events (input via DBus `InputEvent` signals or SDL controller events): Left/Right moves slot, Up/Down cycles profile, R3 toggles Host Mode, B closes.
    4. On close: `cbx_overlay_lifecycle_close()` → conflict resolution → assignment save → `InterceptMode = PASS` → hide surface (not destroy).
    5. Handles `SIGTERM`/`SIGINT` for clean shutdown: closes overlay if visible, disconnects DBus, destroys renderer, exits 0.
  - The overlay appears in <10 ms from activation (pre-built surface, single `SDL_RenderCopy` + `SDL_RenderPresent`).
  - `--dry-run` path is unaffected.
  - A test verifies the poll loop exits cleanly on SIGTERM (send signal to self, assert clean return).
- Verification: `nix-shell --run "cmake --build build-check --target controller-box && ./build-check/controller-box --overlay-service --dry-run && timeout 2 ./build-check/controller-box --overlay-service 2>/dev/null; test \$? -ne 0 || test \$? -eq 124"`
  (The timeout kills the service after 2s; exit 124 = timeout = service ran without crashing. A non-zero exit before timeout indicates a clean init failure, which is acceptable only when DBus is unavailable.)
- Documentation impact: `docs/OPERATIONS.md` — document overlay lifecycle, poll interval, and signal handling.

## Task 5: Overlay deterministic framebuffer visual tests (§4.10)
- Status: pending
- Dependencies: 1, 2
- Scope: `tests/test_overlay_visual.c`, `tests/CMakeLists.txt`
- Acceptance criteria:
  - Test uses `test_harness_sdl_init()` (software renderer, dummy driver) with a deterministic fixture: mock composite devices (3 controllers), settings (4 virtual controllers), assignments, profiles, text cache (compiled-in `CBX_FONT_PATH`), icon cache, icon map, theme (`cbx_theme_default`).
  - Renders through the production composition path: `cbx_select_grid_build()` → `cbx_overlay_surface_init()` → `cbx_overlay_surface_render(surface, renderer, cbx_select_grid_render_cb, &ctx)` → `fb_read_pixels(renderer, NULL, buf, buf_len)`.
  - Tests for each §4.10 state:
    1. **Player Mode grid** — assert `fb_region_has_content` for grid cell regions, text regions (controller model + profile name), and icon regions.
    2. **Host Mode** — assert `fb_frames_differ` between Player Mode and Host Mode captures (different highlight/focus region).
    3. **Conflict highlighting** — set two controllers to the same P-slot; assert `fb_region_has_color` with red target `{220, 40, 40}` in the conflicted row's cell region.
    4. **Unassigned + ≥2 player columns** — assert `fb_region_has_content` in all column header regions and at least 2 player slot regions.
    5. **Controller model/profile text** — assert `fb_region_has_content` with text-colored pixels (`theme.text_primary`) in the profile text region for each row.
    6. **Virtual-device icons** — assert `fb_region_has_content` in icon regions for each slot.
  - Assert that transitions between states (Player Mode → Host Mode, no-conflict → conflict) produce materially different frames via `fb_frames_differ(buf_a, buf_b, w, h, 5.0)`.
  - Tests fail if text, icons, rows, columns, or highlights are absent even when in-memory objects are valid (each assertion checks pixel content, not struct fields).
  - CMake registers `test_overlay_visual` as a ctest; environment sets `SDL_VIDEODRIVER=dummy`.
- Verification: `nix-shell --run "cmake --build build-check --target test_overlay_visual && ctest --test-dir build-check -R test_overlay_visual --output-on-failure"`
- Documentation impact: none (test code)

## Task 6: Manager DBus backend injection point
- Status: pending
- Dependencies: none
- Scope: `src/manager/manager.c`, `src/manager/manager.h`
- Acceptance criteria:
  - Adds `cbx_manager_init_with_dbus(cbx_manager *mgr, const char *font_path, ip_dbus_backend *backend, ip_bus_handle *bus)` — a variant that accepts optional backend/bus pointers. When `backend` or `bus` is NULL, falls back to the current behavior (`ip_dbus_sd_backend()` + real connect).
  - `cbx_manager_init(mgr, font_path)` calls `cbx_manager_init_with_dbus(mgr, font_path, NULL, NULL)` — no behavior change for existing callers.
  - When a non-NULL mock backend is passed, `cbx_manager_init_with_dbus` uses it instead of calling `ip_dbus_sd_backend()` and `backend->connect()`. This enables production-path connected-mode testing without post-init field replacement.
  - Existing `test_manager_production.c` and `test_manager_tabs.c` still pass (they call `cbx_manager_init` which is unchanged).
  - A new test verifies `cbx_manager_init_with_dbus(mgr, font, &mock_backend, &mock_bus)` initializes the manager with the mock backend and that `mgr->ct->backend == &mock_backend`.
- Verification: `nix-shell --run "cmake --build build-check --target test_manager_production test_manager_tabs && ctest --test-dir build-check -R 'test_manager_production|test_manager_tabs' --output-on-failure"`
- Documentation impact: none (internal API extension)

## Task 7: Manager deterministic framebuffer visual tests (§5.6)
- Status: pending
- Dependencies: 1, 6
- Scope: `tests/test_manager_visual.c`, `tests/CMakeLists.txt`
- Acceptance criteria:
  - Test uses `cbx_manager_init_with_dbus()` (production startup path — no manual module attachment) with SDL dummy driver. Two modes:
    - **Connected mode:** passes mock DBus backend with mock composite devices, target devices, and profiles.
    - **Degraded mode:** passes NULL backend (InputPlumber unavailable — what happens naturally in CI).
  - Renders through `cbx_manager_render()` → `fb_read_pixels(renderer, NULL, buf, buf_len)`.
  - Tests for each §5.6 state:
    1. **Controllers tab (connected mode)** — assert `fb_region_has_content` in control regions (add/remove buttons, device list, type picker). Verify non-background pixels in the body region below the tab bar.
    2. **Controllers tab (degraded mode)** — assert the degraded-mode body content is non-background (error message or empty-state controls visible). Assert `fb_frames_differ` between connected and degraded captures.
    3. **Profiles tab** — assert `fb_region_has_content` in the profile list region (Default profile text visible) and create/edit/delete button regions.
    4. **Settings tab** — assert `fb_region_has_content` for every setting label region and its value region (overlay trigger, launch at boot, theme, overlay opacity, virtual controller count/types).
    5. **Profile editor — binding list mode** — open editor via profiles tab; assert `fb_region_has_content` in the controller diagram region, binding list region, and highlighted binding region.
    6. **Profile editor — sequential binding mode** — assert `fb_region_has_content` in the sequential prompt region and progress bar region. Set partial completion (3 of 6 bindings captured) and assert the progress bar region shows intermediate fill that differs from both empty and complete states via `fb_frames_differ`.
    7. **Profile editor — validation error** — trigger a missing-binding validation error; assert `fb_frames_differ` between clean and error states, and assert error-indicator pixels (e.g., red text or error icon) in the error region.
  - Assert that switching tabs produces a materially different frame via `fb_frames_differ`.
  - Assert that switching editor modes (list → sequential) produces a materially different frame.
  - Tests fail if required controls/text are absent even when child counts and visibility flags are valid (each assertion checks pixel content, not struct fields).
  - CMake registers `test_manager_visual` as a ctest.
- Verification: `nix-shell --run "cmake --build build-check --target test_manager_visual && ctest --test-dir build-check -R test_manager_visual --output-on-failure"`
- Documentation impact: none (test code)

## Task 8: Golden image baselines and comparison
- Status: pending
- Dependencies: 5, 7
- Scope: `tests/golden/` (new directory), `tests/test_golden.c`, `tests/CMakeLists.txt`, `scripts/generate-golden.sh`
- Acceptance criteria:
  - `tests/golden/` contains reviewed baseline PNG images for every state tested in Tasks 5 and 7: overlay Player Mode, Host Mode, conflict, Unassigned+columns, text, icons; manager Controllers (connected + degraded), Profiles, Settings, profile editor (list, sequential, validation error, partial progress).
  - `test_golden.c` loads each baseline and compares against a live deterministic capture using `fb_golden_compare` with per-pixel tolerance ±3 per channel and per-image tolerance <2% of pixels may differ.
  - On mismatch, `fb_save_png` and `fb_save_diff` write actual/expected/diff PNGs to `tests/golden-fail/` with the test name and renderer metadata (backend name, window size).
  - `scripts/generate-golden.sh` regenerates all baselines from the deterministic test renderer (explicit reviewed action, never automatic). The script runs the test binary with a `--generate-golden` flag or environment variable that writes baselines instead of comparing.
  - Baseline update is a manual, reviewed commit — the test does not auto-update baselines.
  - CMake registers `test_golden` as a ctest.
- Verification: `nix-shell --run "cmake --build build-check --target test_golden && ctest --test-dir build-check -R test_golden --output-on-failure"`
- Documentation impact: `docs/OPERATIONS.md` — document golden image workflow, tolerance values (±3 per channel, <2% image), and baseline update procedure.

## Task 9: Installed production smoke test (§11.1.5)
- Status: pending
- Dependencies: 4
- Scope: `tests/test_installed_smoke.sh`, `tests/CMakeLists.txt`, `scripts/verify-project.sh`
- Acceptance criteria:
  - `test_installed_smoke.sh`:
    1. Builds and installs the binary to a staging prefix (`.test-install`).
    2. Launches `controller-box --manager` under `Xvfb :99` (headless X11) with `SDL_VIDEODRIVER=x11` and `DISPLAY=:99`.
    3. Sends simulated input events via `xdotool` key presses (Tab/Arrow keys) to navigate between tabs.
    4. Captures the window via `import -window root` (ImageMagick) to a PNG.
    5. Verifies the capture is nonblank: uses `identify -verbose` or `convert ... -format '%[mean]' info:` to check that pixel variance in the top 48px region (tab bar height) and the body region (below 48px) exceeds a threshold (e.g., mean > 5.0 on 0-255 scale, indicating non-background content).
    6. Launches `controller-box --overlay-service` under the same compositor; triggers activation via mock DBus or simulated input; captures the overlay; verifies nonblank via the same pixel-variance check.
  - Test does not bypass production initialization — uses the real `main()` entry point and real SDL/DBus initialization paths (no `--dry-run`).
  - If `Xvfb` or `ImageMagick` is unavailable, the test exits with `SKIP_RETURN_CODE` (77) and a clear message (not a failure).
  - `scripts/verify-project.sh` is updated to run `test_installed_smoke.sh` as part of the full verification gate.
  - CMake registers the script as a ctest with `SKIP_RETURN_CODE 77`.
- Verification: `nix-shell --run "cmake --build build-check && ctest --test-dir build-check -R test_installed_smoke --output-on-failure"` (requires Xvfb + ImageMagick in the nix shell)
- Documentation impact: `docs/OPERATIONS.md` — document smoke test prerequisites (Xvfb, ImageMagick) and execution.

## Task 10: Backend smoke coverage (§11.1.6)
- Status: pending
- Dependencies: 1
- Scope: `tests/test_backend_smoke.c`, `tests/CMakeLists.txt`
- Acceptance criteria:
  - Test creates an SDL2 renderer with `SDL_RENDERER_ACCELERATED` (falling back to software if no GPU). Detects the backend name via `SDL_GetRendererInfo`.
  - If an accelerated backend (OpenGL/OpenGL ES) is available:
    1. Renders a representative overlay grid frame via `cbx_overlay_surface_render(surface, renderer, cbx_select_grid_render_cb, &ctx)` and a manager tab frame via `cbx_manager_render()`.
    2. Reads back pixels via `fb_read_pixels`.
    3. Asserts broad framebuffer invariants: `fb_region_has_content` in expected regions (grid cells, tab bar, body), no all-black or all-background frames.
    4. Asserts that the accelerated output is broadly consistent with the software renderer output (same regions have content, within `fb_golden_compare` tolerance).
  - If no accelerated backend is available, the test is skipped with a clear message (exit 77).
  - Object creation or draw-call success alone is insufficient — pixel content must be verified via `fb_read_pixels` and `fb_region_has_content`.
  - CMake registers `test_backend_smoke` as a ctest with `SKIP_RETURN_CODE 77`.
- Verification: `nix-shell --run "cmake --build build-check --target test_backend_smoke && ctest --test-dir build-check -R test_backend_smoke --output-on-failure"`
- Documentation impact: `docs/OPERATIONS.md` — document backend smoke test and hardware requirements.

## Task 11: Final documentation and specification audit
- Status: pending
- Dependencies: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
- Scope: `README.md`, `docs/OPERATIONS.md`, `docs/` (all docs), `docs/SPEC.md` (spec text correction), `scripts/verify-project.sh`
- Acceptance criteria:
  - `README.md` reflects the actual verification suite: framebuffer tests, golden images, installed smoke test, backend smoke coverage.
  - `docs/OPERATIONS.md` documents: overlay service lifecycle, golden image workflow, tolerance values, baseline update procedure, smoke test prerequisites, backend smoke test, and the human release acceptance checklist (§11.1.7).
  - A human release acceptance checklist is documented (legibility, clipping, focus indication, contrast, controller-only usability) with instructions for capturing and reviewing representative manager and overlay frames on target hardware.
  - `scripts/verify-project.sh` runs the complete suite: build, ctest (including visual/golden/installed/backend tests), packaging checks.
  - Specification coverage audit: every requirement in §4.10, §5.6, and §11.1 (layers 1–6 automated; layer 7 documented) is mapped to a test or documented process.
  - Spec text correction: §2.5, §10.3 gap #1, and §11 of `docs/SPEC.md` are updated from "~500 ms" to "~50 ms (DEC-002)" to match the implementation. (This is a spec text fix, not a behavior change.)
  - Clean Git state: all changes committed, no uncommitted artifacts.
- Verification: `nix-shell --run "./scripts/verify-project.sh"` passes with all tests green.
- Documentation impact: `README.md`, `docs/OPERATIONS.md`, `docs/SPEC.md` — comprehensive update.

## Final completion gates

1. **Specification coverage:** Every requirement in §4.10, §5.6, and §11.1 layers 1–6 is covered by an automated test; layer 7 (human release acceptance) is documented.
2. **Tests:** All existing tests (63+) plus new visual/golden/installed/backend tests pass under `ctest`.
3. **Documentation:** README and OPERATIONS docs match actual behavior and verification suite.
4. **Clean Git state:** All changes committed on `develop`; no uncommitted artifacts.