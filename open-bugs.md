# Open Bugs

Canonical queue of defects awaiting maintenance.

Schema: `ralph-bug-ledger/v1`

```json
[
  {
    "id": "BUG-0001",
    "title": "Manager launches with an effectively blank interface",
    "status": "planned",
    "severity": "critical",
    "reported": "2026-08-06",
    "external": [],
    "contract_change": false,
    "reproduction": "On NixOS, build and install to a local CMake prefix, then run the installed controller-box binary with --manager. The window opens, but only dark backgrounds, tab rectangles, and the window decoration are visible.",
    "expected": "The manager visibly renders the Controllers, Profiles, and Settings tab labels plus the controls, lists, status text, and other content required to operate the controller-only UI.",
    "actual": "The manager window renders structural rectangles but no application text or usable controls. Startup also prints a non-fatal OpenGL alpha-blending verification warning; the unrelated GTK theme warning originates from the desktop theme.",
    "acceptance": "A locally installed NixOS build visibly renders manager labels and controls using a reliably available font; missing font resources produce an actionable failure rather than a blank UI; an automated regression exercises real font initialization/text rendering; the full project verifier passes.",
    "resolution": "",
    "verification": "",
    "closed": null
  }
]
```
