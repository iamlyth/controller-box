---
spec_path: docs/SPEC.md
spec_commit: 05b866f957c02e4c89b31b3f96e82ed5c214bd34
spec_blob: b403cb933239698d4ddaedc9a438bdcf9d36a059
base_commit: 05b866f957c02e4c89b31b3f96e82ed5c214bd34
status: active
---

# Implementation Plan — Installed Functional Recovery

## Goal

Make the installed product perform its core job from a clean environment: connect to a native-signature InputPlumber service, detect and accept real controller input, create observable virtual targets, create/save/reload profiles, apply assignments, and show a compositor-visible overlay. Mock-only, keyboard-proxy, off-screen, and no-backend evidence is supplemental rather than release evidence.

## Non-goals and constraints

- Do not bundle or replace InputPlumber.
- Keep one writer on `develop`; subagents remain read-only and builds/tests remain serial.
- Preserve existing component tests, but replace false production claims with executable installed evidence.
- InputPlumber 0.78.0 DBus source at upstream commit `082f67f` is the compatibility reference.

## Specification conformance matrix

| ID | Requirement | Classification | Evidence/gap | Task |
|---|---|---|---|---|
| FR-01 | §§2.2, 10 native DBus signatures | missing | Production reads every property as `s`; required `as`, `u`, and `b` fail | Task 1 |
| FR-02 | §§2.4, 10.1 readiness/recovery/hotplug | missing | Manager treats system-bus connection as readiness and does not process DBus | Tasks 2, 9 |
| FR-03 | §§5.1, 5.7 real controller Manager input | missing | Manager initializes video only and tests inject keyboard events | Task 3 |
| FR-04 | §§5.2, 5.5 authoritative virtual topology | missing | Add picker fails on `as`; startup settings do not create targets; Add does not confirm routing | Tasks 4, 5 |
| FR-05 | §§5.3–5.4 clean-home profile workflow | missing | No shipped Default; Empty cannot add first binding; save/discard is implicit | Tasks 6, 7 |
| FR-06 | §§4.1–4.7 assignment/profile backend application | partial | Overlay changes grid/config without confirming InputPlumber state; profiles not loaded | Task 8 |
| FR-07 | §§4.9–4.10 compositor-visible reusable overlay | missing | Production overlay window remains hidden and lifecycle is one-shot | Task 9 |
| FR-08 | §§9, 11.1 installed functional acceptance | missing | Installed smoke accepts missing backend, keyboard proxy, and hidden overlay | Task 10 |
| FR-09 | §§5.6–5.7 degraded/error semantics | partial | Backend failures are silent no-ops and controls look enabled | Tasks 2, 4, 7 |
| FR-10 | §11.2 autonomous definition of done | missing | Previous all-verified matrix relied on mocks and skipped backend outcomes | Tasks 10, 11, 12 |

## Interaction acceptance inventory

Production acceptance covers these semantic workflows through normal production dispatch:

- **Manager/controller:** hotplug/open a real or kernel-backed SDL game controller; D-pad navigates, A activates, B cancels, and focus changes are visible. Keyboard events are proxy-only.
- **Manager/pointer:** derive clicks from final rendered bounds for Controllers Add/Remove/Type, Profiles Create/Edit/Save/Delete, and Settings persistence; outcomes are independently observed over DBus or filesystem.
- **Controllers/backend:** native `as` type picker → create/attach/confirm target → type change → remove/unassign, with unavailable/denied/failed operations visibly recoverable.
- **Profiles:** clean HOME → shipped Default copy and Empty sequential capture → explicit Save → file exists and parses → Manager restart reloads it; explicit Discard and validation errors create no file.
- **Overlay/controller:** native DBus InputEvent opens the mapped window, moves assignments, cycles a real enumerated profile, enters/exits Host Mode, resolves conflicts, applies backend state, and restores PASS on the activating composite.
- **Recovery:** backend name loss disables operations without exiting; reacquisition and ObjectManager hotplug reconcile production state within specified bounds.

## Task 1: Native typed DBus transport and compatibility fixture
- Status: pending
- Dependencies: none
- Scope: production-owned DBus contract header, `src/dbus/dbus_client.c`, property/method wrappers, mock fidelity, private sd-bus fixture tests.
- Acceptance criteria: native `s`, `as`, `u`, and `b` properties round-trip against a private service; Manager Version uses `/Manager`; wrong signatures fail tests; production no longer depends on a string-only property contract.
- Verification: clean build plus `ctest -R 'test_native_dbus|test_connection|test_manager_calls|test_composite_calls|test_source_props|test_target_props'`.
- Documentation impact: `docs/DBus-API.md` compatibility reference.

## Task 2: InputPlumber readiness, degraded UI, and owner recovery
- Status: pending
- Dependencies: Task 1
- Scope: `ip_connection`, Manager/overlay startup and event loops, visible status/disabled controls, owner-change recovery.
- Acceptance criteria: raw bus connection is not readiness; unavailable, denied, incompatible, and enumeration failures are distinct; both loops process DBus; owner acquisition/loss recovers without restart.
- Verification: native fixture scenarios plus connection, Manager production, overlay service, framebuffer degraded/recovery tests.
- Documentation impact: README and `docs/OPERATIONS.md` diagnostics.

## Task 3: Real SDL game-controller input and hotplug
- Status: pending
- Dependencies: none
- Scope: SDL initialization, controller handle lifecycle, button-to-semantic mapping, Manager dispatch, controller transport tests.
- Acceptance criteria: existing and hotplugged controllers navigate/activate/cancel through `SDL_CONTROLLER*`; removal is safe; keyboard remains supplemental.
- Verification: SDL virtual-controller test traverses representative Manager controls without synthesized key events.
- Documentation impact: controller support and troubleshooting.

## Task 4: Functional Controllers tab with confirmed backend outcomes
- Status: pending
- Dependencies: Tasks 1, 2
- Scope: supported type loading, actionable errors, operation state, target confirmation, Controllers interaction tests.
- Acceptance criteria: Add picker uses native arrays; Add/Remove/Type do not silently fail; UI updates only after ObjectManager confirms exact target type/count; controls disable when unavailable.
- Verification: native fixture and pointer/controller production dispatch tests assert exact DBus and model outcomes.
- Documentation impact: Controllers workflow.

## Task 5: Authoritative startup target topology and routability
- Status: pending
- Dependencies: Task 4
- Scope: topology reconciliation service, settings integration, create/attach/stop/type transactions, assignment unapply/rollback.
- Acceptance criteria: clean startup reaches configured ordered target topology; created targets are attached/routable; removing/type-changing one slot preserves others; failures retain last confirmed topology.
- Verification: native ObjectManager topology scenarios and routed target-event observation.
- Documentation impact: startup topology semantics.

## Task 6: Shipped immutable Default profile
- Status: pending
- Dependencies: none
- Scope: bundled InputPlumber YAML, path/enumeration fallback, packaging, clean-home tests.
- Acceptance criteria: Default is always present/read-only from Controller-Box assets; Default copy succeeds without host profiles; packages install the asset.
- Verification: clean-XDG profile list, create flow, packaging/install tests, InputPlumber-compatible parse.
- Documentation impact: Profiles and packaging docs.

## Task 7: Reachable explicit profile creation, save, discard, and errors
- Status: pending
- Dependencies: Tasks 3, 6
- Scope: zero-row add/sequential entry, visible Save/Discard controls, dirty/close handling, error reporting, repeat-open visibility, installed profile workflow tests.
- Acceptance criteria: Empty can capture first and NES-minimum bindings; explicit save persists/reloads; discard/window-close behavior is explicit; production event ordering cannot consume the confirmation release; failures stay recoverable.
- Verification: clean-home controller and pointer production dispatch tests plus restart persistence.
- Documentation impact: editor controls and XDG locations.

## Task 8: Apply assignments and profiles to live InputPlumber
- Status: pending
- Dependencies: Tasks 1, 5, 6
- Scope: overlay profile enumeration/cycling, stable identity application, target/order updates, confirmed persistence and rollback.
- Acceptance criteria: slot/profile changes update verified engine state before persistence; startup/restart restores order/profile; LoadProfile failures do not appear saved.
- Verification: native fixture observes `GamepadOrder`, target attachment, `LoadProfilePath`, restart/reordered enumeration, and routed input.
- Documentation impact: assignment/profile recovery.

## Task 9: Visible reusable overlay and runtime reconciliation
- Status: pending
- Dependencies: Tasks 1, 2, 8
- Scope: show/hide/map window, per-activating-composite lifecycle, poll re-arm, hotplug reconciliation, dynamic columns, production visuals.
- Acceptance criteria: activation maps a compositor-visible frame; close restores PASS on the activating composite and permits later activations; hotplug/restart rebuilds rows, mappings, triggers, polls, and confirmed target columns.
- Verification: compositor capture plus native multi-composite/hotplug lifecycle tests and latency measurements.
- Documentation impact: compositor support matrix and service lifecycle.

## Task 10: Mandatory installed functional acceptance gate
- Status: pending
- Dependencies: Tasks 3, 5, 7, 9
- Scope: private native-signature service, kernel-backed/SDL virtual controller fixture, installed binary/package flow, `verify-project.sh`.
- Acceptance criteria: clean install detects controller, creates observable routable target, saves/reloads profile, applies assignment, maps overlay, and survives process/backend restart; missing prerequisites fail rather than skip.
- Verification: non-skippable `test_installed_functional` invoked by project verification.
- Documentation impact: exact acceptance prerequisites and commands.

## Task 11: Factory backpressure against false completion
- Status: pending
- Dependencies: Task 10
- Scope: Ralph prompts, validators/final gates, boilerplate tests/docs, reusable boilerplate branch parity.
- Acceptance criteria: final completion requires recorded installed-functional evidence; keyboard proxies, string mocks, fixture assembly, missing backend, and skips cannot mark production requirements verified; review findings mechanically append remediation tasks.
- Verification: factory regression fixtures reject the previous false-complete plan/evidence and accept valid installed evidence.
- Documentation impact: factory methodology and Huntley comparison.

## Task 12: Final documentation and specification audit
- Status: pending
- Dependencies: Tasks 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
- Scope: complete definition of done, conformance matrix, interaction inventory, open bugs, independent reviews, docs, clean builds and repository.
- Acceptance criteria: every conformance row is verified by production evidence; installed functional and interaction gates pass without skips; open defects and independent correctness/security/test/docs reviews have no blocker; documentation is current and tree is clean. Any gap appends a pending remediation task and returns this audit to pending.
- Verification: clean Debug/Release builds, full CTest, installed functional gate, packaging, `verify-project.sh`, `verify-boilerplate.sh`, and target-hardware human review recorded as release prerequisite.
- Documentation impact: README, operations, packaging, profiles, DBus API, conformance evidence.
