# Open Bugs

Canonical queue of defects awaiting maintenance.

Schema: `ralph-bug-ledger/v1`

```json
[
  {
    "id": "BUG-0007",
    "title": "Controller diagram SVG never loaded in production \u2014 NULL svg_path passed to cbx_profile_diagram_init",
    "status": "open",
    "severity": "high",
    "reported": "2026-08-16",
    "external": [],
    "contract_change": false,
    "reproduction": "Build and install controller-box. Launch manager mode. Navigate to Profiles tab, select a profile, and open the profile editor. The controller diagram area (left panel, 300x300 region) shows only button labels and highlight rectangles against the background \u2014 the controller outline image is absent. Inspect src/manager/profile_editor_list.c line 242: cbx_profile_diagram_init(&ed->diagram, renderer, NULL, theme) passes NULL as the svg_path argument. The init function in src/manager/profile_diagram.c only loads base_texture when svg_path is non-NULL (line 214: if (svg_path && renderer)). Therefore base_texture is always NULL in production and the SVG controller outline is never rendered.",
    "expected": "The profile editor diagram renders a controller outline image (loaded from an SVG file in data/icons/svg/) with button highlights overlaid, as specified in docs/SPEC.md. The diagram should visually represent the controller layout so users can see which button is currently being bound.",
    "actual": "The diagram region contains only sparse button label text and highlight rectangles (approximately 14% non-background pixels concentrated in a few rows). The controller outline SVG is never loaded because NULL is passed as the svg_path. The golden baseline images (tests/golden/manager_editor_list.png, manager_editor_sequential.png) were captured from this broken state and match the broken rendering, so the golden image comparison test passes as a false positive.",
    "acceptance": "Production code passes a valid SVG file path to cbx_profile_diagram_init so that base_texture is non-NULL and the controller outline renders visibly. Golden baseline images are regenerated to include the controller outline. A test asserts base_texture is non-NULL when a valid SVG path is provided. The fb_region_has_content check in test_manager_visual.c verifies content across the full diagram region (not just sparse label pixels). Full project verifier passes.",
    "resolution": "",
    "verification": "",
    "closed": null
  }
]
```
