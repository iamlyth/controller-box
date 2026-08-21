# Open Bugs

Canonical queue of defects awaiting maintenance.

Schema: `ralph-bug-ledger/v1`

```json
[
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
  },
  {
    "id": "BUG-0016",
    "title": "Proxy evidence is promoted to production verification by the acceptance machinery",
    "status": "open",
    "severity": "critical",
    "reported": "2026-08-19",
    "external": [],
    "contract_change": false,
    "reproduction": "A completed plan can claim conformance rows `verified` on evidence that does not exercise the normative production path: pixel/offscreen framebuffer assertions are counted as real visual acceptance, private/session-scoped sd-bus services are counted as the real system service, a uinput producer is counted as the target consumer, and a runner declaration or prose evidence line is counted as executed evidence. The conformance matrix is free text inside `.factory/artifacts/implementation-plan.md`; `scripts/validate-implementation-plan.py` accepts any evidence cell for a `verified` row, so the same false-positive acceptance that produced BUG-0014 (blank production diagram behind passing goldens/visual tests) and BUG-0015 (private/native-signature sd-bus tests counted as system-bus evidence) can be reproduced by any future cycle.",
    "expected": "Every conformance requirement carries a machine-readable sidecar entry declaring classification, evidence tier, required capabilities, the exact evidence commit, and receipt/artifact refs. `verified` normative real-system/visual/hardware rows cannot be satisfied by a lower evidence tier or by an undeclared/unevidenced capability. Missing contract, probe, receipt, or a skipped probe means unevidenced and is never auto-reclassified. Capability contracts define probe argv, must-execute, must-not-skip, and deny-simulated markers, and are tracked for declared/required capabilities only. Coordinator-executed commands in audits carry machine receipts bound to digest and exit 0; subagent prose cannot certify runtime. Pixel/offscreen checks are not real visual acceptance, private/session DBus is not the real system service, a uinput producer is not the target consumer, and an evidence declaration is not evidence.",
    "actual": "The factory has no conformance sidecar, no evidence tiers, no capability-contract file or receipt checker hooks, no machine audit receipts for coordinator-executed commands, and no read-only visual/runner/evidence/spec reviewer roles. The implementation/plan/audit prompts and the final gates do not reject free-text verified rows, lower-tier evidence for normative rows, skipped probes, BLOCKED-in-pass audits, fabricated command prose, missing receipts, or not_applicable misuse.",
    "acceptance": "The false-positive-acceptance redesign is implemented and tested: a machine-readable conformance sidecar/schema and validator; tracked capability contracts plus receipt checker hooks with Controller-specific definitions only for declared/required capabilities and product-neutral generic templates; machine audit receipts for coordinator-executed commands requiring matching digest/exit 0 with BLOCKED evidence forcing `result: findings`; tightened implementation/plan/audit prompts and final gates; specialized read-only visual/runner/evidence/spec reviewers; adversarial boilerplate tests rejecting free-text verified rows, private DBus for system capabilities, skipped GPU/kernel probes, BLOCKED-in-pass audits, fabricated command prose, missing receipts, and not_applicable misuse; and a generic parity port with no product leakage. BUG-0016 closes only after a later stage migrates the active plan to the machine-readable conformance and re-verifies with real acceptance evidence.",
    "resolution": "",
    "verification": "",
    "closed": null
  },
  {
    "id": "BUG-0018",
    "title": "Installed manager controller diagram renders but is materially incorrect (visual acceptance gap)",
    "status": "open",
    "severity": "critical",
    "reported": "2026-08-21",
    "external": [],
    "contract_change": false,
    "reproduction": "Human dogfooding run: the fresh installed build now shows the controller diagram in the profile editor (the BUG-0014 blank-diagram fix works), but the rendered diagram looks materially incorrect — wrong geometry/layout/relationships, not merely cosmetic. This was observed by a human operator on the real installed launch path.",
    "expected": "The installed production manager profile editor renders the controller diagram correctly (correct controller silhouette geometry, slot highlight placement, binding-light positions, model label, and binding list), verified by a perceptible installed-window acceptance that asserts semantic diagram content — not only silhouette/pixel counts or golden PASS. A machine visual-audit inventory (exact-commit installed screenshots, per-state provenance+hashes, parallel read-only vision review by roles with calibration) is the reproducible capture path; vision findings are supplemental falsification evidence only and never replace deterministic/compositor/human acceptance.",
    "actual": "BUG-0014 was closed on silhouette/pixel acceptance (test_installed_diagram.sh asserts black-outline pixel count, slot highlight, title, binding list) and Task 8's environment-independent profile selection. A human fresh-build run shows the diagram now renders but is materially incorrect, so the silhouette/pixel evidence did not catch a perceptible visual defect. No reproducible exact-commit screenshot inventory or parallel vision review exists yet for the manager/editor/overlay states.",
    "acceptance": "A Ralph-owned product diagnosis and fix: reproduce the materially-incorrect diagram in a fresh installed build, correct the rendering so the diagram is perceptibly correct, and add a semantic installed-window acceptance that requires recognizable correct diagram content (not merely silhouette pixels or a PASS golden). The machine visual-audit framework (.factory/visual-audit.toml, scripts/visual-capture-driver.sh, scripts/visual-audit-review*.py, sealed SDK image delivery, calibration) must produce exact-commit installed captures and structured findings for manager-main/profiles/editor/overlay states; vision output is supplemental falsification evidence only and never elevates an evidence tier. BUG-0014 closure must not be re-accepted solely from silhouette/pixel tests.",
    "resolution": "",
    "verification": "",
    "closed": null
  }
]
```
