# Open Bugs

Canonical queue of defects awaiting maintenance.

Schema: `ralph-bug-ledger/v1`

```json
[
  {
    "id": "BUG-0014",
    "title": "Manager shows no controller diagram in production launch",
    "status": "open",
    "severity": "critical",
    "reported": "2026-08-19",
    "external": [],
    "contract_change": false,
    "reproduction": "Launch the installed/real production binary with a real window server: ./build-check/controller-box --manager under Xvfb. The manager window opens, but the controller diagram area is blank — no controller schematic/grid is drawn. This was observed by a human operator on the real launch path.",
    "expected": "The manager production window renders a recognizable controller diagram (controller figure, binding lights, select-screen grid) through the real SDL rendering path, and a production-window/installed-path semantic test proves recognizable diagram content — not merely a non-NULL texture, a fallback path, or a broad pixel-count change.",
    "actual": "Launching ./build-check/controller-box --manager shows no controller diagram. Existing coverage (test_manager_visual.c, test_golden.c, test_overlay_visual.c) passed without catching this, so those goldens/visual tests do not prove the production diagram rendering path.",
    "acceptance": "A Ralph-owned product diagnosis and fix: the real manager window renders the controller diagram through the production path; a new semantic test drives a real production window (installed path under Xvfb or equivalent) and asserts recognizable diagram content (expected regions/figures), not just non-NULL textures or broad pixel changes; the full project gate passes; the diagram row in the conformance matrix is re-verified only with this evidence.",
    "resolution": "",
    "verification": "",
    "closed": null
  },
  {
    "id": "BUG-0015",
    "title": "Manager reports Topology incomplete: 0 of 4 virtual controllers active",
    "status": "open",
    "severity": "critical",
    "reported": "2026-08-19",
    "external": [],
    "contract_change": false,
    "reproduction": "Launch the real production manager binary (./build-check/controller-box --manager). The manager reports 'Topology incomplete: 0 of 4 virtual controllers active' and none of the virtual controllers work. This is a human-observed failure on the real launch path; the controller-box runtime could not reach/activate the four expected target/controller objects.",
    "expected": "With real InputPlumber system-bus acceptance, four expected target/controller objects are present and usable through production dispatch: the manager reports all four virtual controllers active, and controller events flow through the real DBus backend to the InputPlumber service (no private mock or /dev/uinput presence substitutes for this).",
    "actual": "0 of 4 virtual controllers active; no virtual controller works. The plan currently classifies rows such as SYS-06, DBUS-02, DBUS-05 (and related ARCH/OVL/MGR rows) as verified using private/native-signature sd-bus tests (test_native_dbus.c, test_overlay_native.c, test_manager_native.c) and inferred requests, but the inputplumber-system-dbus capability is NOT declared in .factory/environment.toml, so real system-bus acceptance is unavailable and must not be claimed.",
    "acceptance": "A Ralph-owned product diagnosis and fix: real InputPlumber system-bus acceptance showing four expected target/controller objects active and usable through production dispatch (real DBus connection + real event dispatch), with the capability declared and proven by exact-commit receipt. If inputplumber-system-dbus remains unavailable, it is preserved as an explicit blocking finding and the plan/matrix/docs do not claim the affected rows verified. No fabricated evidence.",
    "resolution": "",
    "verification": "",
    "closed": null
  }
]
```
