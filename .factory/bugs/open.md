# Open Bugs

Canonical queue of defects awaiting maintenance.

Schema: `ralph-bug-ledger/v1`

```json
[
  {
    "id": "BUG-0008",
    "title": "Controller diagram SVG not rendered when running from build tree",
    "status": "open",
    "severity": "high",
    "reported": "2026-08-16",
    "external": [],
    "contract_change": false,
    "reproduction": "Run ./build-check/controller-box --manager from the build tree without setting CBX_ICON_DIR. Open the profile editor. The left-panel controller diagram shows only a flat panel_bg rectangle with sparse button highlights; no controller outline is drawn.",
    "expected": "The profile editor diagram renders the generic-gamepad.svg controller outline (SPEC §5.4: always-visible controller diagram; §5.6: meaningful non-background framebuffer output).",
    "actual": "cbx_icon_dir() returns the compile-time ICON_DIR (/usr/share/controller-box/icons), which does not exist in the build tree. profile_editor_list.c builds /usr/share/controller-box/icons/svg/generic-gamepad.svg, load_svg_texture() returns NULL, and the diagram falls back to a flat rectangle. The diagram only renders when CBX_ICON_DIR is set to the source tree.",
    "acceptance": "Running the manager from the build tree (no env var) renders the controller outline. A test exercises the production cbx_icon_dir() path (no env-var injection) and asserts base_texture is non-NULL.",
    "resolution": "",
    "verification": "",
    "closed": null
  },
  {
    "id": "BUG-0009",
    "title": "Overlay icon cache path mismatch: looks in icons/ but SVGs installed to icons/svg/",
    "status": "open",
    "severity": "high",
    "reported": "2026-08-16",
    "external": [],
    "contract_change": false,
    "reproduction": "Run the overlay service. The selection grid renders without virtual-device icons. overlay_service.c passes cbx_icon_dir() (= /usr/share/controller-box/icons) to cbx_icon_cache_init(); icon_cache.c builds /usr/share/controller-box/icons/{name}.svg, but CMake installs SVGs to /usr/share/controller-box/icons/svg/{name}.svg.",
    "expected": "The overlay renders virtual-device icons in each grid cell (SPEC §4.10, §8.1).",
    "actual": "icon_cache.c:99 builds icon_dir/{name}.svg without the /svg/ subdirectory, so no icon textures load. The diagram call site (profile_editor_list.c) correctly appends /svg/, but the overlay icon cache does not. Tests bypass by passing data/icons/svg/ directly to cbx_icon_cache_init().",
    "acceptance": "The overlay loads at least one icon texture through the production cbx_icon_dir() path. A test exercises cbx_icon_dir() -> cbx_icon_cache_init() -> cbx_icon_cache_load() and asserts at least one icon loads from the installed directory structure.",
    "resolution": "",
    "verification": "",
    "closed": null
  },
  {
    "id": "BUG-0010",
    "title": "Tests bypass production resource path via CBX_ICON_DIR env-var injection",
    "status": "open",
    "severity": "high",
    "reported": "2026-08-16",
    "external": [],
    "contract_change": false,
    "reproduction": "test_manager_visual.c:333 and test_golden.c:531 setenv('CBX_ICON_DIR', '<source>/data/icons') in setup. This makes the SVG load in tests, so assert_non_null(base_texture) passes. But the real binary (no env var) hits /usr/share/controller-box/icons and fails. The tests never exercise the production path.",
    "expected": "Tests exercise the production resource path (cbx_icon_dir() without env-var injection) so a production-path regression is caught.",
    "actual": "The CBX_ICON_DIR env-var injection masks the production failure: tests pass while the real binary renders no controller outline and no overlay icons. This is the root cause of the false-positive test suite.",
    "acceptance": "No test sets CBX_ICON_DIR (or any resource-path env var) to make a resource load. The production path works in the build tree, and tests exercise it directly.",
    "resolution": "",
    "verification": "",
    "closed": null
  }
]
```
