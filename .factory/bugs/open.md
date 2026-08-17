# Open Bugs

Canonical queue of defects awaiting maintenance.

Schema: `ralph-bug-ledger/v1`

```json
[
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
