# Spec Compliance Audit — Task 4: Re-render overlay on host-mode entry and reconcile dirty triggers (W1, W2)

**Scope:** SPEC §4.4 (Host Mode), §4.9 (Rendering & performance / pre-built surface dirty policy), §4.10 (Visual acceptance), §11.1 (rendering verification).
**Method:** read `docs/SPEC.md` and the task acceptance in `.factory/artifacts/implementation-plan.md`; traced every `mark_dirty_all`/`mark_dirty` call site and every host-mode enter/exit path in `src/`; inspected the W1/W2 test suites; independently re-ran both task verification commands.

## Verification (independently reproduced)

| Command | Result |
|---|---|
| `ctest --test-dir build -R 'test_overlay_visual\|test_overlay_interaction\|test_golden' --output-on-failure` (via nix-shell) | **3/3 passed, exit 0** |
| `./scripts/verify.sh` (via nix-shell) | **100% passed (91/91 incl. skips honored), exit 0** — only runner-gated skips `test_kernel_controller`, `test_backend_smoke` |
| `ctest --test-dir build -R 'test_host_mode' --output-on-failure` | **1/1 passed** |

## Acceptance 1 — W1: host-mode entry/exit marks the pre-built surface dirty — **PASS**

- `src/overlay/host_mode.c`: `cbx_host_mode_enter`/`cbx_host_mode_exit` fire `on_state_change` on real transitions only; exit is guarded by `was_active` (no spurious fire on a no-op exit — the documented W2 contract in `host_mode.h`).
- `src/app/overlay_service.c`: `cbx_overlay_on_host_mode_change()` marks the surface dirty and is wired as `svc->hm.on_state_change` in `run_overlay_service` (step 10, after `cbx_host_mode_init`, before the poll loop). Both production dispatch paths reach it: DBus `cbx_overlay_input_cb` (R3 → `cbx_player_mode_handle` → `CBX_PM_RESULT_HOST` → `cbx_host_mode_toggle`; host R3 → `cbx_host_mode_handle` → exit fires once, dispatch does **not** re-exit) and the SDL path in `cbx_overlay_service_step` (same structure). Neither dispatch re-exits on `CBX_HM_RESULT_EXIT`, so the R3-exit trigger fires exactly once.
- The step loop re-renders only when active **and** dirty (`cbx_overlay_service_step` step 7), so the presented frame reflects HOST/SELECTED/FROZEN row visuals on entry — satisfying §4.10's "materially different frame" requirement.
- Tests are genuine and registered: `tests/test_overlay_visual.c::test_host_entry_dirty_render_differs` (dirty flag observed *before* the re-render helper, then `fb_frames_differ(..., 5)` — >5% pixel difference through the production render callback); `tests/test_overlay_interaction.c::test_hm_entry_marks_dirty_dbus` / `test_hm_exit_marks_dirty_dbus` / `test_hm_transition_fires_once` (production DBus dispatch, single-fire counting wrapper over the production callback, no-op redundant exit does not fire); `tests/test_host_mode.c::test_exit_noop_no_transition` plus `test_state_change_on_enter_exit` / `test_state_change_on_toggle`; golden baseline `tests/golden/overlay_host_mode.png` via `test_golden_overlay_host_mode` (regeneration deliberately gated by `CBX_GENERATE_GOLDEN`, tolerance-based compare — §11.1.3 compliant).

## Acceptance 2 — W2: dirty-trigger policy reconciled to SPEC §4.9 — **PASS**

Every trigger in the reconciled set exists and is deliberate:

| Trigger | Call site | Category |
|---|---|---|
| Device change (hotplug) | `overlay_service.c` `cbx_overlay_reconcile_hotplug` | §4.9-enumerated |
| Device change (backend ready) | `overlay_service.c` `overlay_backend_ready` | §4.9-enumerated |
| Slot change | `overlay_service.c` `cbx_overlay_on_slot_change` (§4.9 comment) | §4.9-enumerated |
| Profile change | `overlay_service.c` `cbx_overlay_on_profile_change` (§4.9 comment) | §4.9-enumerated |
| Host-mode enter/exit | `overlay_service.c` `cbx_overlay_on_host_mode_change` (documented as deliberate W2 extension of §4.9) | task-sanctioned |
| Show / fade-in | `lifecycle.c` `show_surface` / `begin_fade_in` (commented) | deliberate lifecycle |
| Save | `overlay_service.c` `cbx_overlay_on_save` (W2 reconciliation comment) | deliberate lifecycle |
| Close | dirties only via the save callback during close; `hide_surface` deliberately does not mark | deliberate |
| Startup pre-build | `run_overlay_service` initial mark+render | §4.9 itself |

The earlier double-exit defect is repaired (only explanatory comments remain at both dispatch sites); single-fire behavior is regression-tested. Naming follows project conventions (`cbx_hm_state_change_cb` / `cbx_overlay_on_host_mode_change` mirror `cbx_overlay_on_slot_change` etc.); the header documents the no-op-exit contract.

**No BLOCKER findings.** The residuals below are documentation/consistency WARNs and observations.

## Findings

### WARN 1 — Redundant, undocumented SLOT dirty-mark at both host-mode dispatch sites
- **Files:** `src/app/overlay_service.c` — DBus path `cbx_overlay_input_cb` (~lines 1268–1276) and SDL path `cbx_overlay_service_step` (~lines 1329–1338).
- **Description:** For `CBX_HM_RESULT_SLOT`, the surface is dirtied twice: once inside `cbx_host_mode_handle` via `hm->on_slot_change` → `cbx_overlay_on_slot_change` (which marks dirty), and again by the dispatch path's uniform `MOVED || SLOT` mark. The `MOVED` arm is required (no callback fires on host row navigation) and correct; the `SLOT` arm is redundant and inconsistent with the Player Mode path, which relies solely on callbacks ("Surface dirty flag is set by callbacks"). Neither dispatch site carries a dirty-trigger policy comment — the surrounding comment covers only the EXIT double-fire. This exact residual was flagged as WARN by an earlier in-cycle audit and remains unaddressed/undocumented, leaving the one ad-hoc call site the acceptance asked to eliminate. Harmless in behavior (`cbx_dirty_rect_add_all` is idempotent — resets to one full-screen rect), so not a blocker.
- **Recommendation:** Either (a) restrict the explicit dispatch mark to `CBX_HM_RESULT_MOVED` and let `on_slot_change` cover SLOT (consistent with Player Mode), or (b) keep the uniform catch-all and add a one-line comment documenting it as deliberate W2 policy.

### WARN 2 — Reconciled dirty policy not documented in the module that owns the dirty API; two trigger sites lack policy comments; show doc misdescribes behavior
- **Files:** `src/overlay/surface_build.h` (header policy statement, ~lines 12–15), `src/overlay/lifecycle.c` (`show_surface` doc, ~lines 59–61), `src/app/overlay_service.c` (hotplug site ~line 995; `overlay_backend_ready` ~line 1090).
- **Description:** The canonical policy statement in `surface_build.h` still says dirt happens "when device/slot/profile state changes (SPEC §4.9)" and does not mention host-mode state transitions (or the deliberate save/show triggers) that this task reconciled — a consumer of `cbx_overlay_surface_mark_dirty_all` reading that header cannot discover the reconciled trigger set, which currently lives only in scattered comments in `overlay_service.c`. `lifecycle.c`'s `show_surface` doc says "mark dirty, render (if renderer available), show" — the function never renders; the re-render happens later in `cbx_overlay_service_step` (step 7), so the comment misdescribes behavior. The hotplug site carries only a WHAT-comment ("Mark surface dirty for re-render.") and the backend-ready mark is bare, while the slot/profile/host-mode/save sites received WHY-comments this cycle — an inconsistency in exactly the dimension the acceptance calls "deliberate/consistent." (These were the in-cycle linting audit's WARN #5; only the slot/save/host-mode sites were repaired.)
- **Recommendation:** Extend the `surface_build.h` policy sentence to the reconciled trigger set (device/slot/profile + host-mode transitions + deliberate lifecycle points); correct the `show_surface` doc to "mark dirty + present; re-render occurs in the service step"; replace the WHAT-comment and add a brief WHY at the hotplug and backend-ready sites.

### INFO 3 — `cbx_host_mode_enter` lacks the mirror of the W2 no-op guard
- **Files:** `src/overlay/host_mode.c` (`cbx_host_mode_enter`, ~lines 30–39), `src/overlay/host_mode.h` (enter doc says only "Enter host mode. The given row_idx becomes the host.").
- **Description:** Exit fires `on_state_change` only on a real active→inactive transition, but enter fires unconditionally — entering an already-active object would emit a spurious transition (one extra idempotent re-render) and silently relocate `host_row`. Unreachable in production (the only caller, `cbx_host_mode_toggle`, guards with `if (!hm->active)`), so observation only.
- **Recommendation:** For contract symmetry, either add an `if (!hm->active)`-style guard or document the precondition in `host_mode.h`.

### INFO 4 — Test-file organization leftovers from the linting audit (unaddressed)
- **Files:** `tests/test_host_mode.c` (line 704: `/* --- Main --- */` banner mislabels the "State-change dirty trigger (W1)" section; `main()` at line 799 is banner-less; the header "Tests:" list omits the three new state-change tests), `tests/test_overlay_visual.c` (header enumerates tests 1–7 (+2b) but omits "8. W1 — `test_host_entry_dirty_render_differs`").
- **Description:** The repairs fixed the O13 inventory-ID collision (tests renamed to `test_hm_*`, header note added) and documented the W2 no-op contract in `host_mode.h`, but skipped the banner placement and stale file-header TOCs. Cosmetic only; no behavioral or coverage impact.
- **Recommendation:** Move the `Main` banner directly above `main()`; add the missing header entries.

### INFO 5 — SPEC §4.9 literal wording vs. the reconciled dirty set (no violation; consider a spec clarification)
- **Files:** `docs/SPEC.md` §4.9; `src/app/overlay_service.c` comments.
- **Description:** §4.9 literally says the surface is "dirtied only on device/slot/profile change events." This task deliberately adds host-mode state transitions (and, necessarily, host row navigation for the SELECTED-row highlight) — explicitly sanctioned by the task acceptance and required by §4.10's "transitions must produce a materially different frame." The in-code comments honestly phrase this as "a deliberate W2 extension of the §4.9 pre-build dirty policy … not a device/slot/profile change themselves" (repair commit `79a3e404` removed the earlier overbroad §4.9 citations). Not a violation.
- **Recommendation:** For durability, consider a §4.9 errata/policy note enumerating the reconciled trigger set so future audits do not re-flag the extension; until then the W2 comments in `overlay_service.c` are the authoritative record.

### INFO 6 — Dirty-flag assertions exist only for the host-mode triggers
- **Files:** `tests/test_overlay_interaction.c`, `tests/test_host_mode.c`, `tests/test_hotplug.c`.
- **Description:** The task added direct dirty-flag/single-fire assertions for host-mode enter/exit, but the pre-existing slot/profile/device-change triggers have no direct dirty-flag assertions (slot/profile callbacks are exercised for their state outcomes; hotplug reconcile asserts model changes, not the flag). The SDL-keyboard dispatch path's host-mode dirty trigger is exercised only indirectly through the shared `on_state_change` callback (the production DBus controller path is asserted directly). The task acceptance does not require per-trigger flag tests, and the policy's behavior is correct; noting for completeness.
- **Recommendation:** Optionally add cheap flag assertions (one line each) after a slot move, profile cycle, and hotplug reconcile to lock the whole §4.9 policy against regression.

## Conclusion

Task 4 is **spec-compliant**. Both acceptance criteria are implemented on real production paths and verified by genuinely-executing tests; both verification commands were independently reproduced with exit 0. **No BLOCKER findings.** The two WARNs are documentation/consistency residuals (the redundant SLOT mark and the policy-documentation gaps) that do not affect behavior or the acceptance outcome; the INFO items are symmetry, hygiene, and coverage observations.