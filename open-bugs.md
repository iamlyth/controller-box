# Open Bugs

Canonical queue of defects awaiting maintenance.

Schema: `ralph-bug-ledger/v1`

```json
[
  {
    "id": "BUG-0002",
    "title": "CreateComposite XDG runtime test is order-dependent and leaks temp directories",
    "status": "triaged",
    "severity": "medium",
    "reported": "2026-08-06",
    "external": [],
    "contract_change": false,
    "reproduction": "Inside nix-shell, configure and build the project, then run ctest --test-dir build-manual -E '^test_packaging$' --output-on-failure while one or more unrelated files matching /tmp/controller-box-* exist. test_create_composite_xdg_runtime_dir_preferred may fail at tests/test_create_composite.c:272 with a nonzero count such as '3 != 0'; the same test may pass when rerun alone.",
    "expected": "The XDG runtime directory test proves that its CreateCompositeDevice call does not create a fallback file in /tmp without depending on or deleting unrelated pre-existing /tmp/controller-box-* files, and it always removes its own /tmp/cbx-xdg-* directory.",
    "actual": "The test asserts that the global count of /tmp/controller-box-* is exactly zero instead of comparing before and after. Pre-existing or concurrent files make the test fail nondeterministically, and an assertion failure bypasses rmdir(), leaving its cbx-xdg temporary directory behind.",
    "acceptance": "Seed an unrelated /tmp/controller-box-* file and verify test_create_composite still passes without modifying that file; compare relevant /tmp state before and after or otherwise identify only files created by the operation; guarantee cleanup of the test-owned XDG directory on success and failure; repeated standalone and full CTest runs pass.",
    "resolution": "",
    "verification": "",
    "closed": null
  }
]
```
