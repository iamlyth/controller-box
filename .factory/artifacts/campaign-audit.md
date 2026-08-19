---
schema: ralph-campaign-audit/v1
round: 1
audit_base_commit: 26df6c05a5319214f1c0a97a2c3c6f763128b1c6
plan_commit: b36a56474bc994a109e19dab42770497283eb1dc
plan_blob: c89f0e137c3b93649c59ffcb8cf61d1dd2929620
environment_blob: 0a54fa8893b542386a61312c28c97e5e79ff5366
runner_evidence_sha256: 7e29a2b045fe42251d04848a2fa5940e7178b458e93cad5710357ddfa4e0d098
result: findings
---
# Campaign Round 1 Independent Gap Audit

## Evidence reviewed

- Specification: `docs/SPEC.md` §§2.4–2.5, 4.10, 5.6–5.7, 10.1, 11.1.1–11.1.7, 11.2, 12 — architecture, overlay/manager visual and interaction acceptance, DBus native type fidelity, rendering verification layers, definition of done, and out-of-scope boundaries challenged against production code and runner evidence.
- Production paths: `src/app/main.c:82–138` initialization (font discovery, manager init, mode dispatch); `src/config/config_paths.c:179–186` `cbx_icon_dir()` ICON_DIR/SOURCE_ICON_DIR fallback with `access()`; `src/icons/icon_cache.c:72–108` SVG rasterization `{icon_dir}/svg/{name}.svg` with path traversal protection; `src/icons/icon_lookup.c:48–265` PNG override with `realpath()` + safe-directory whitelist; `src/manager/manager.c:441–655` event dispatch through tab system + hit-testing + focus chain; `src/overlay/lifecycle.c:1–280` IDLE→ACTIVATING→VISIBLE→CLOSING state machine; `src/overlay/surface_build.c:35–105` pre-built texture show/hide; `src/dbus/dbus_client.c` native sd-bus types (u, b, as, s, v); `src/dbus/ip_connection.c:22–155` NameOwnerChanged subscription with arg0 filter; `src/dbus/ip_intercept_poll.c:58–130` 50ms SDL timer poll; `src/manager/profile_editor_list.c:104–145` diagram SVG via production `cbx_icon_dir()`; `src/manager/manager.c:395–425` and `src/app/overlay_service.c:1290–1338` shutdown paths freeing all resources including externally-managed DBus.
- Executable evidence: `.factory-state/runner-evidence/dev-runner-vm/26df6c05a5319214f1c0a97a2c3c6f763128b1c6/manifest.json` PASS (exit_code=0, result=pass, capabilities=[installed-package, kernel-uinput, remote-project-gate, systemd-user], commit=26df6c0, environment_blob=0a54fa88, tree=a104e03b); `.factory-state/runner-evidence/dev-runner-vm/26df6c05a5319214f1c0a97a2c3c6f763128b1c6/stdout.log` — 97/98 tests passed (1 skip: `test_backend_smoke` GPU), `test_kernel_controller` Passed 10.88s, `test_installed_functional` Passed 10.97s, `test_installed_smoke` Passed 18.22s, `test_installed_binary` Passed 27.55s, Flatpak build `PASS: flatpak --version: controller-box 0.1.0`, `PASS: all packaging integration checks passed`; all capability contract sections passed. This is real runner evidence (not synthetic) — SSH runner `dev-runner-vm` executed `scripts/verify-project.sh` against the exact audit base commit.
- Environment limits: `.factory/environment.toml` declares `capabilities = ["remote-project-gate", "systemd-user", "kernel-uinput", "installed-package"]` for runner `dev-runner-vm` (SSH transport). Undeclared: `gpu-compositor`, `physical-controller`, `inputplumber-system-dbus`, `target-consumer`. `check-factory-runner-evidence.py` reports "aggregate Git/environment binding is stale" (aggregate commit 26df6c0 ≠ HEAD 696ed40) — expected because the campaign-audit checkpoint commit adds only the audit artifact, not new runner evidence; the aggregate is valid for the audit base commit.

## Finding 1: Stale conformance matrix — runner-evidenced capabilities marked as blocked or partial

- Requirement: SPEC §11.2.1 (complete conformance matrix — every requirement classified `verified` with specific evidence; no requirement remains `partial`, `missing`, or verified only by prose) and §11.2.8 (documentation matches observed behavior).
- Production evidence: The runner receipt at the audit base commit (26df6c0) demonstrates that `test_kernel_controller` PASSES (10.88s, kernel-uinput capability) and a clean Flatpak build PASSES (`PASS: flatpak --version: controller-box 0.1.0`, installed-package capability). The environment.toml at that commit declares both capabilities. However, the implementation plan conformance matrix at the same commit still marks:
  - **VRF-05** (§11.1.5 installed functional): "partial" — claims `test_kernel_controller` still skips (exit 77) and `/dev/uinput` is not provisioned. The runner evidence contradicts this.
  - **PKG-01** (§9.1 Flatpak): "partial" — claims "no clean Flatpak build has passed installed functional gate." The runner evidence shows a Flatpak build passing.
  - **DBUS-02** (§10.1 native type fidelity): "partial" — claims "no test against real InputPlumber system DBus service." SPEC §10.1 explicitly allows "real/private sd-bus service"; the private native-signature DBus tests pass on the runner.
  - **DOD-03** (§11.2.3 interaction traversal): "partial" — claims "kernel-backed controller test is blocked." The runner evidence shows kernel-backed controller test passing.
  - **DOD-05** (§11.2.5 regression gates): "partial" — claims "Task 9 blocked (no `/dev/uinput`)." The runner evidence shows `test_kernel_controller` passing with `/dev/uinput`.
  - Tasks 9 and 13 are marked `blocked` with block evidence that is directly contradicted by the runner receipt at the same commit.
- Required remediation: The next fresh plan must reconcile the conformance matrix with the runner evidence at commit 26df6c0. Rows VRF-05, PKG-01, DBUS-02, DOD-03, and DOD-05 must move to `verified` (or remain blocked only where genuinely unevidenced — e.g., GPU smoke). Task 9 and Task 13 block evidence must be updated or the tasks marked `complete`. The plan's §2 constraints paragraph must reflect the four declared capabilities, not just two.

## Finding 2: Documentation inaccuracies — stale capability claims and incorrect interaction inventory counts

- Requirement: SPEC §11.2.8 (README and operational documentation match observed behavior) and §5.7 (machine-readable or test-enumerated inventory of every interactive control).
- Production evidence: Multiple documentation artifacts contain factually incorrect claims:
  - `README.md` lines 204, 283: states "no `kernel-uinput` runner capability declared in `.factory/environment.toml`" — the capability IS declared.
  - `docs/OPERATIONS.md`: repeats the same stale claim about `kernel-uinput`.
  - `tests/CMakeLists.txt:23`: comment says "no kernel-uinput runner capability declared in environment.toml" — stale.
  - `README.md` and `docs/OPERATIONS.md`: interaction inventory states "52/59 entries verified, 6 NOT_APPLICABLE, 1 DEFERRED." The machine-readable inventory in `tests/interaction_inventory.c` contains 7 NOT_APPLICABLE entries (M13, M14, M16, M32, M34, M35, M36) and 51 verified — correct counts are 51 verified, 7 NOT_APPLICABLE, 1 DEFERRED.
  - Implementation plan DOD-03 row: states "M01–M39" but `tests/interaction_inventory.c` contains entries M01–M38 only. M39 (first-run service install) is described in the plan's narrative section but was never added to the inventory data structure.
  - Implementation plan Task 10 block evidence: says "only `remote-project-gate`, `systemd-user`, `kernel-uinput` declared" — omits `installed-package`.
- Required remediation: Update README.md, OPERATIONS.md, tests/CMakeLists.txt, and the implementation plan to reflect the actual environment.toml declarations, correct interaction inventory counts (51/7/1), and add M39 to the inventory data structure or correct the plan reference to M01–M38.

## Finding 3: GPU backend smoke test not evidenced (VRF-06, DOD-05)

- Requirement: SPEC §11.1.6 (backend smoke coverage — exercise deployment renderer backend with accelerated OpenGL/OpenGL ES; successful object creation or draw calls alone are insufficient) and §11.2.5 (regression and quality gates — no unexplained skips).
- Production evidence: `test_backend_smoke` skips (exit 77) on the runner at commit 26df6c0 — the only skipped test out of 98. The `gpu-compositor` capability is not declared in `.factory/environment.toml` and no runner receipt evidences an accelerated GPU backend. `test_backend_smoke_sw.c` provides software-renderer coverage as partial evidence but does not satisfy the hardware-backend acceptance requirement.
- Required remediation: Provision a runner with an accelerated GPU backend (OpenGL/OpenGL ES) and run `test_backend_smoke` to exit 0, or document a human-approved deferral per SPEC §11.2.6.

## Finding 4: aarch64 build target not evidenced (SYS-01)

- Requirement: SPEC §3 (architectures: x86_64 and aarch64 — both build targets required for tarball).
- Production evidence: Only x86_64 build is evidenced. No aarch64 cross-compilation or native build has been executed or evidenced. The `target-consumer` capability is not declared in `.factory/environment.toml`. The implementation plan (Task 11) documents `cmake/aarch64-toolchain.cmake` as a partial deliverable but the cross-compiler is unavailable.
- Required remediation: Provision an aarch64 cross-compiler (nixpkgs `pkgsCross.aarch64-multiplatform`) or a native aarch64 runner and evidence a zero-warning build, or document a human-approved deferral.

## Finding 5: Pi 4 latency and human release acceptance not evidenced (SYS-02, PERF-01, VRF-07)

- Requirement: SPEC §11 (overlay appearance ≤75ms p99, ≤100ms max on minimum supported hardware; <10ms p99 detection-to-present), §3 (minimum hardware: Raspberry Pi 4), and §11.1.7 (human release acceptance — human reviews representative captures on target hardware before promotion to `main`).
- Production evidence: `test_overlay_latency.c` measures latency on x86_64 but not on Pi 4 or equivalent ARM64 with GLES 3.0. The `target-consumer` capability is not declared. No human release acceptance artifact exists (no reviewer name, date, hardware, captures, or criteria checklist). The button-to-frame ≤75ms p99 interval is derived from poll+show computation, not measured end-to-end with a real timer on minimum supported hardware.
- Required remediation: Provision Pi 4 (or equivalent ARM64 with GLES 3.0) hardware, measure and record overlay appearance latency, and produce a signed human release acceptance artifact with reviewer name, date, hardware, representative captures, and criteria checklist, or document a human-approved deferral.