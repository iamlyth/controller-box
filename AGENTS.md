# Controller-Box Operational Guide

Keep this file brief and operational. Progress, task status, and verification evidence belong in `.factory/artifacts/implementation-plan.md`; the fresh Python factory keeps canonical mutable loop state in `.factory-state/factory-loop.json` and campaign-scoped runtime state below `.factory-state/campaigns/` (methodology: `docs/FACTORY-LOOP-SPEC.md`).

## Sources of truth

- Product contract: `docs/SPEC.md`
- Declared factory capabilities: `.factory/environment.toml` (never invent undeclared runners)
- Active work and evidence: `.factory/artifacts/implementation-plan.md`
- Ordinary defects: `.factory/bugs/open.md` and `.factory/bugs/closed.md`
- Work only on `develop`; the human promotes to `main`.
- Do not use Git worktrees or change the committed specification during implementation.

## Build

Use Nix so SDL2, systemd, YAML, cmocka, Xvfb, xdotool, and image tools are consistent:

```bash
nix-shell --run 'cmake -S . -B build-check -DCMAKE_BUILD_TYPE=Debug && cmake --build build-check --parallel'
```

For a targeted rebuild, use `cmake --build build-check --target <target>`.

## Immediate validation

```bash
# Targeted CTest by exact name or regex
nix-shell --run "ctest --test-dir build-check -R '<regex>' --output-on-failure"

# Complete clean project gate: build, CTest, packaging, installed smoke
nix-shell --run './scripts/verify-project.sh'

# Factory policy and orchestration
./scripts/verify-boilerplate.sh

# Strong validation of existing exact-commit runner evidence (read-only)
./scripts/check-factory-runner-evidence.py --expected-commit "${ACCEPTED_COMMIT:?exact commit}" --expected-campaign-id "${CAMPAIGN_ID:?campaign}" --expected-readiness-nonce "${READINESS_NONCE:?round-zero nonce}"

# Operator-only terminal campaign archival (never global or automatic)
./scripts/archive-factory-campaign.py --root "$PWD" --campaign-id "${CAMPAIGN_ID:?terminal campaign}"

# Runner acquisition is coordinator-only: model roles never invoke runners.
# Every real-provider campaign runs mandatory trusted round-zero readiness;
# production policy also requires exactly five rounds (non-5 fails before a model).
# Add --readiness-only to acquire/publish the same gate and stop before planner 1.
# (all declared signed runners, four-target physical routing, licensed accelerated
# compositor evidence, core/conformance mapping, and committed three-state human
# graphics approval) before planner 1. Any absent/stale/partial result fails closed.
# FACTORY_COORDINATOR_AUTH_FD must already name the inherited current-user-owned
# mode-0600 read/write coordinator authority/one-use state descriptor. The local
# factory harness never requires root (only remote runners may use root): run the
# installed rootless coordinator entrypoint, which creates/opens the per-user
# state under a private mode-0700 XDG state directory outside the repository and
# execs only the sibling campaign. One operator command (rootless; the
# rootless coordinator entrypoint forwards the campaign arguments below):
#   "${INSTALL_PREFIX:?verified production install}/.factory/bin/factory-coordinator" --root "$PWD" run --campaign-id "${CAMPAIGN_ID:?new unique id}" --rounds 5 --branch develop --provider "${PI_PROVIDER:?set provider}" --model "${PI_MODEL:?set model}" --backend "${PI2_BACKEND:?set trusted pi2 executable}" --accepted-commit "${ACCEPTED_COMMIT:?clean accepted HEAD}" --install-manifest "${INSTALL_MANIFEST:?verified manifest}" --campaign-timeout 21600 --verification-command ./scripts/verify-project.sh --runner-command ./.factory/runner/run-factory-runners.py --capability-command ./.factory/tools/check-capability-evidence.py --acceptance-command '["./scripts/final-gate.sh","--implementation"]'
"${INSTALL_PREFIX:?verified production install}/.factory/bin/factory-coordinator" --root "$PWD" run --campaign-id "${CAMPAIGN_ID:?new unique id}" --rounds 5 --branch develop --provider "${PI_PROVIDER:?set provider}" --model "${PI_MODEL:?set model}" --backend "${PI2_BACKEND:?set trusted pi2 executable}" --accepted-commit "${ACCEPTED_COMMIT:?clean accepted HEAD}" --install-manifest "${INSTALL_MANIFEST:?verified manifest}" --campaign-timeout 21600 --verification-command ./scripts/verify-project.sh --runner-command ./.factory/runner/run-factory-runners.py --capability-command ./.factory/tools/check-capability-evidence.py --acceptance-command '["./scripts/final-gate.sh","--implementation"]'
```

Run build/test commands serially. Do not dismiss an unrelated failure as pre-existing: determine its cause, fix it when safe, or append a remediation task with evidence.

## Run and inspect

```bash
./build-check/controller-box --manager --dry-run
./build-check/controller-box --overlay-service --dry-run
```

Use `test_manager_visual`, `test_overlay_visual`, and `test_golden` for framebuffer behavior. Use `test_installed_smoke` for the installed X11 production path. Golden updates must be explicit and reviewed; never regenerate them merely to make a test pass.

A custom `CMAKE_INSTALL_PREFIX` build is for isolated install/UI testing and should exclude the default-prefix packaging assertion. Use a separate default-prefix build for full packaging verification.

## Code and test patterns

- Search before assuming behavior is missing; reuse established `src/` utilities instead of ad-hoc copies.
- Production acceptance must use real initialization, SDL event dispatch, rendering, backend, persistence, and shutdown paths.
- UI tests must assert semantic outcomes as well as pixels; direct callback tests are supplemental.
- Route InputPlumber operations through the DBus backend abstraction so tests can observe exact requests.
- Scope temporary-file assertions to files owned by the test; never assert global `/tmp/controller-box-*` emptiness.
- Do not leave placeholders, stubs, weakened assertions, unexplained skips, or test-only production bypasses.
- A runner declaration is not evidence; accept only exact-commit receipts validated by the runner evidence checker.
- Documentation records why a constraint or test matters, not iteration history.

## Git commit boundary

- Commits must carry at least one substantive tracked path; empty metadata-only commits are rejected, and retired `.ralph/**` recovery paths may only be deleted from the index. There is no lifecycle token, scratchpad exception, or final-handoff authorization. The boundary is enforced at Git level by `.factory/tools/git-commit-guard.sh` (installed as `pre-commit`, `prepare-commit-msg`, `pre-merge-commit`, `applypatch-msg`, `pre-applypatch`, `commit-msg` hooks by `.factory/tools/install-git-commit-guard.sh`, which runs on every launch) and at the model command layer by `.factory/tools/pi-cli-shims/git`.
- The boundary enforces meaningful substance via the shared classifier `.factory/loop/substance.py`: an administrative-only staged set (implementation plan, bug ledgers, campaign audit/evidence sidecars) is a valid commit only when the implementation plan carries a genuine semantic planning change (a task add/remove/reorder or a title/priority/dependencies/Scope/Acceptance edit). Evidence/status/prose/timestamp edits are not meaningful. Valid no-op planning records the phase without a commit or HEAD advance, and developer progress/completion requires at least one substantive tracked path (a plan/bug/audit/evidence-sidecar-only revision is metadata, never work). Runtime state (`.factory-state/`) stays outside Git.
- Do not bypass hooks (`--no-verify`, `core.hooksPath`, `GIT_CONFIG_*`); only `git commit` may create commits from the model command boundary.

## Acceptance evidence (BUG-0016 machinery)

- Conformance rows are machine-checked from `.factory/artifacts/conformance.json` (`ralph-conformance/v1`) by `.factory/tools/validate-conformance.py`; free-text matrix cells cannot prove acceptance, and `blocked`/`partial`/`not_applicable` rows fail implementation completion unless re-classified with evidence.
- Capability contracts live in `.factory/capability-contracts.json` (declared capabilities only; never claim undeclared/unavailable ones) and are checked by `.factory/tools/check-capability-contracts.py`; `.factory/tools/check-capability-evidence.py` requires a fresh exact-commit receipt with the probe executed, not skipped, and no simulated markers.
- Coordinator commands are recorded by `.factory/tools/machine-receipt.py --tag <tag> -- <argv...>` under `.factory-state/audit-receipts/`; audits must cite `[receipt: ...]`/`[manifest: ...]`, PASS requires exit 0, and any BLOCKED evidence forces `result: findings`.
- Pixel/offscreen checks are not real visual acceptance, private/session DBus is not the real system service, a uinput producer is not the target consumer, and declaring evidence is not evidence. The conformance sidecar is the only authority for verified claims.
