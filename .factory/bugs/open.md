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
    "reproduction": "Launch the real production manager binary (./build-check/controller-box --manager). The manager reports 'Topology incomplete: 0 of 4 virtual controllers active'; human dogfooding confirms that no virtual controllers are created. This is a human-observed failure on the real launch path, not merely an inactive UI indicator: the expected target/controller objects never become available.",
    "expected": "With real InputPlumber system-bus acceptance, four expected target/controller objects are present and usable through production dispatch: the manager reports all four virtual controllers active, and controller events flow through the real DBus backend to the InputPlumber service (no private mock or /dev/uinput presence substitutes for this).",
    "actual": "The original production observation remains unevidenced as resolved: 0 of 4 virtual controllers active and no usable routed controller. The bounded product remediation now retains CreateTargetDevice paths, confirms exact publication/type/removal with monotonic deadlines, verifies every exact composite attachment, reports categorized phase/count/deadline/cleanup diagnostics, refuses insufficient composite cardinality before mutation, and refreshes the visible Manager Controllers tab. Private native-signature sd-bus tests cover delayed/reordered/failure behavior only as supplemental software evidence. They do not prove the real InputPlumber system bus, kernel targets, or physical routing.",
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
    "reproduction": "Human dogfooding run: open the mapping editor in the fresh installed manager. The BUG-0014 blank-diagram fix makes the diagram visible, but the controller image is pixelated and stretched, and the mapped-button locations are positioned incorrectly relative to the controller drawing. This is a material geometry/layout defect observed by a human operator on the real installed launch path, not merely cosmetic raster variation.",
    "expected": "The installed production manager profile editor renders a sharp, aspect-correct controller diagram without pixelation or stretching. Every button marker/highlight is anchored to the corresponding physical control on the diagram, with correct silhouette geometry, slot highlight placement, model label, and binding list. Acceptance uses perceptible installed-window semantic assertions, not only silhouette/pixel counts or golden PASS. A machine visual-audit inventory (exact-commit installed screenshots, per-state provenance+hashes, parallel read-only vision review by roles with calibration) is the reproducible capture path; vision findings are supplemental falsification evidence only and never replace deterministic/compositor/human acceptance.",
    "actual": "Production software remediation now explicitly maps Xbox 360, Xbox One/Elite, Xbox Series, DualSense, Steam Deck, and generic diagrams to installed filenames and complete per-icon layouts. Profile sidecars reach the lazy/reused editor, supported-load failure clears stale texture/layout, and semantic provenance is observable. Initial coordinates remain implementation values pending exact visual approval; no signed real-GPU/window or human approval exists for this commit.",
    "acceptance": "A Ralph-owned product diagnosis and fix: reproduce the defect in a fresh installed build; preserve the source diagram's aspect ratio and adequate raster resolution; anchor every mapped-button marker/highlight to the correct control after layout scaling; and add semantic installed-window acceptance that detects stretching, pixelation, and marker-to-control misalignment (not merely silhouette pixels or a PASS golden). The machine visual-audit framework (.factory/visual-audit.toml, scripts/visual-capture-driver.sh, scripts/visual-audit-review*.py, sealed SDK image delivery, calibration) must produce exact-commit installed captures and structured findings for manager-main/profiles/editor/overlay states; vision output is supplemental falsification evidence only and never elevates an evidence tier. BUG-0014 closure must not be re-accepted solely from silhouette/pixel tests.",
    "resolution": "",
    "verification": "",
    "closed": null
  },
  {
    "id": "BUG-0019",
    "title": "Production factory confinement cannot execute the authenticated Pi2 backend",
    "status": "open",
    "severity": "high",
    "reported": "2026-08-27",
    "external": [],
    "contract_change": true,
    "reproduction": "Run an exact-commit installed five-round campaign with the authenticated /nix/store Pi2 wrapper, provider openai-codex, and model gpt-5.6-luna. Pi2 itself lists the model and a direct one-shot returns READY, but factory-launch terminates every planner attempt with exit 1. Direct installed factory-launch diagnostics show Landlock denying exec of the Pi2 wrapper; attempts to admit its immutable script closure proceed only to further denials for shebang interpreters, coreutils, and /dev/null. Campaign luna-production-02cb7b0-1 terminated failed after three planner attempts and completed zero rounds.",
    "expected": "The production harness securely launches the authenticated Pi2 backend for a fixed real-model provider while preserving fresh context, exact-commit binding, credential isolation, Git-command mediation, Landlock or an equivalently strong verified confinement boundary, bounded supervision, and no synthetic fallback.",
    "actual": "The factory documents Pi as the production backend but its file-granular Landlock policy is incompatible with the jailed Pi2 bootstrap. Broad Nix-store execution or exposing the host agent directory would weaken existing security boundaries, so experimental provider/closure changes were reverted rather than committed as a bypass.",
    "acceptance": "Design and security-review a Pi2 backend adapter that composes the factory boundary with Pi2's jail without broad executable grants or model-readable host credentials; add adversarial tests for absolute Git bypass, credential exfiltration, script/interpreter closure, /dev devices, fresh sessions, and exact backend identity; pass full boilerplate gates; then complete an installed monitored five-round gpt-5.6-luna campaign without intervention. Synthetic campaigns are not acceptance evidence.",
    "resolution": "",
    "verification": "",
    "closed": null
  }
]
```
